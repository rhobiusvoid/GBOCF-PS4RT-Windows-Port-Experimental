#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <tuple>
#include <set>
#include "video_core/renderer_vulkan/vk_display_registry.h"
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <xxhash.h>
#include "video_core/renderer_vulkan/ui_composition.h"
#include "video_core/renderer_vulkan/ps4rt_camera.h"
#include "ps4rt_toggles.h"
#include "ps4rt_gpu_workarounds.h"
#include "ps4rt_settings.h"
#include "video_core/texture_cache/ps4rt_transition.h"

#include "video_core/renderer_vulkan/vk_render_options.h"

#include "common/debug.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_depth_stencil_state.h"
#include "video_core/renderer_vulkan/vk_gpu_profile.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#include "ps4rt_threads.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"


namespace Vulkan {
// ps4rt (ultrawide HUD): canvas clip, shift and horizontal stretch of the current display draw.
static s32 g_uw_canvas_x0, g_uw_canvas_x1, g_uw_shift; static float g_uw_stretch = 1.0f;
// A centred movie draw: its scissor's side edges in render-target pixels, for the black bars.
static bool g_uw_movie; static s32 g_uw_movie_x0, g_uw_movie_x1;
// High-res display (opt-in): display buffers taller than 1080 (PS4RT_HIRES_DISPLAY) scale
// the HUD by height/1080. The rules above work on a virtual display of the same aspect at 1080
// height; g_uw_shift/canvas are virtual pixels, multiplied by g_hud_scale when applied.
// g_hud_pixel: top-left pixel quads (anchored at the display origin, not the centre).
static float g_hud_scale = 1.0f; static bool g_hud_pixel = false;
// World tags (projected from 3D by the game for the real display): scaled about their own anchor
// (canvas units, centre-relative, y up) so the position stays and only the size grows.
static bool g_hud_anchor = false; static float g_hud_ax = 0.0f, g_hud_ay = 0.0f;


static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

// ps4rt diagnostics: GPU profile label of a draw: its pixel shader, else its vertex shader.
static u64 ProfileHash(const GraphicsPipeline& pipeline) {
    const auto* fs = pipeline.GetStages()[u32(Shader::SwStage::Fragment)];
    return fs ? fs->pgm_hash : pipeline.GetStage(Shader::SwStage::Vertex).pgm_hash;
}

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_, Runtime& runtime_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_}, page_manager{this},
      buffer_cache{instance, scheduler, runtime, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, runtime, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool, buffer_cache.GetSparsePageShift()},
      host_markers_enabled{EmulatorSettings.IsVkHostMarkersEnabled()},
      guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {
    // Before the rasterizer is bound: Liverpool enqueues buffers only once it sees it.
    draw_prep = std::make_unique<DrawPreparation>(pipeline_cache);
    scene_targets = std::make_unique<SceneTargets>(instance, scheduler, runtime, texture_cache);
    // Object motion first: it fixes the buffer addresses the motion shader variants embed.
    object_motion = std::make_unique<ObjectMotion>(instance, scheduler);
    camera_motion = std::make_unique<CameraMotion>(instance, scheduler, texture_cache, runtime);
    camera_motion->SetObjectMotion(object_motion.get());
    upscaler = std::make_unique<TemporalUpscaler>(instance, scheduler, texture_cache, runtime,
                                                  *camera_motion, *scene_targets);
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
    }
    memory->SetRasterizer(this);
    GpuProfile::Init(instance);

    ;
    if (DrawPipeWanted()) {
        constant_ring = std::make_unique<ConstantRing>(instance, scheduler);
        draw_pipe = std::make_unique<DrawPipe>(&RunDrawPacket, this);
    }
    // ps4rt: this thread joins the texture binding helper before it changes image state.
    runtime.SetImageAccessHook(&JoinBindHelper, this);
    scheduler.SetSubmitCallback([this](Vulkan::SubmitInfo& info) {
        runtime.FlushBarriers();
        buffer_cache.SubmitPendingArenaBinds(info);
    });
}

Rasterizer::~Rasterizer() = default;

inline const AmdGpu::Regs& Rasterizer::Regs() const {
    return stage_regs ? *stage_regs : liverpool->regs;
}

inline AmdGpu::CbDbExtent Rasterizer::CbExtent(u32 index) const {
    return stage_regs ? pipe_cb_extent[index] : liverpool->last_cb_extent[index];
}

inline const AmdGpu::ComputeProgram& Rasterizer::CsRegs() const {
    return stage_cs ? *stage_cs : liverpool->GetCsRegs();
}

inline AmdGpu::CbDbExtent Rasterizer::DbExtent() const {
    return stage_regs ? pipe_db_extent : liverpool->last_db_extent;
}

namespace {
/// magic_enum::enum_contains, from a table: it scans every enumerator (per texture per draw).
bool IsKnownFormat(AmdGpu::DataFormat data_fmt, AmdGpu::NumberFormat num_fmt) {
    static const auto tables = [] {
        std::pair<std::array<bool, 256>, std::array<bool, 256>> t{};
        for (const auto value : magic_enum::enum_values<AmdGpu::DataFormat>()) {
            if (static_cast<u32>(value) < 256) {
                t.first[static_cast<u32>(value)] = true;
            }
        }
        for (const auto value : magic_enum::enum_values<AmdGpu::NumberFormat>()) {
            if (static_cast<u32>(value) < 256) {
                t.second[static_cast<u32>(value)] = true;
            }
        }
        return t;
    }();
    const u32 d = static_cast<u32>(data_fmt), n = static_cast<u32>(num_fmt);
    return d < 256 && n < 256 && tables.first[d] && tables.second[n];
}
} // namespace

bool Rasterizer::FilterDraw() {
    const auto& regs = Regs();
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        ScopedMarkerInsert("FmaskDecompress");
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        // Games may disable color buffer and enable force depth/stencil dirty and valid to
        // do a copy from one depth-stencil surface to another, without a pixel shader.
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

template <typename... Parts>
VideoCore::ImageId Rasterizer::FindTargetMemoized(VideoCore::TextureCache::ImageDesc& desc,
                                                  LastTarget& last, auto&& make_desc,
                                                  const Parts&... parts) {
    static_assert((sizeof(Parts) + ...) <= 256, "target memo key too large");
    std::array<u8, 256> key{};
    u32 key_size = 0;
    const auto append = [&](const auto& part) {
        std::memcpy(key.data() + key_size, &part, sizeof(part));
        key_size += sizeof(part);
    };
    (append(parts), ...);
    const u64 generation = texture_cache.RegistryGeneration();
    const bool memo_enabled = !Ps4RtToggle::Disabled(Ps4RtToggle::TextureBindingMemo);
    if (memo_enabled && last.generation == generation && last.key_size == key_size &&
        std::memcmp(last.key.data(), key.data(), key_size) == 0) {
        texture_cache.MarkFound(last.image_id);
        return last.image_id;
    }
    const auto remember = [&](VideoCore::ImageId image_id) {
        last.key = key;
        last.key_size = key_size;
        last.generation = generation;
        last.image_id = image_id;
    };
    u64 hash = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < key_size; i += 8) {
        u64 word;
        std::memcpy(&word, key.data() + i, 8);
        hash = (hash ^ word) * 0x100000001B3ull;
        hash ^= hash >> 29;
    }
    auto& memo = target_memo[hash % target_memo.size()];
    if (memo.generation == generation && memo.key_size == key_size &&
        std::memcmp(memo.key.data(), key.data(), key_size) == 0 && memo_enabled) {
        desc = memo.desc;
        texture_cache.MarkFound(memo.image_id);
        remember(memo.image_id);
        return memo.image_id;
    }
    make_desc();
    const VideoCore::ImageId image_id = texture_cache.FindImage(desc);
    if (generation == texture_cache.RegistryGeneration()) {
        memo.key = key;
        memo.key_size = key_size;
        memo.generation = generation;
        memo.image_id = image_id;
        memo.desc = desc;
        remember(image_id);
    } else {
        last.generation = ~0ULL;
    }
    return image_id;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = Regs();
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = CbExtent(cb);
        const u32 tag = 1;
        image_id = bound_images.emplace_back(FindTargetMemoized(
            desc, last_targets[cb], [&] { std::construct_at(&desc, col_buf, hint); }, tag,
            col_buf, hint));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    }

    if (GetEffectiveDepthStencilState(regs).needs_attachment) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = DbExtent();
        auto& [image_id, desc] = db_desc;
        const u32 tag = 2;
        image_id = bound_images.emplace_back(FindTargetMemoized(
            desc, last_targets[AmdGpu::NUM_COLOR_BUFFERS],
            [&] {
                std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                                  htile_address, hint);
            },
            tag, regs.depth_buffer, regs.depth_view, regs.depth_control, htile_address, hint));
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    } else {
        db_desc.first = {};
    }
    // ps4rt: the G-buffer pass (5+ color targets) holds the scene depth, and its constants the
    // main camera (shadow passes bind the same layout with the light's camera).
    const u32 guest_targets = key.mrt_mask & ~(key.motion_vectors ? 1u << Shader::MotionVectors::Output : 0u);
    gbuffer_draw = camera_motion->Enabled() && db_desc.first &&
        (std::popcount(key.mrt_mask) >= 5);
    if (Ps4RtSettings::Get().native_scene) {
        gbuffer_draw = false;
        // Code Fairy: the scene pass writes a G-buffer (target 0 is 10-bit, up to 6 targets) with
        // standard depth at the internal render size (16:9 or this launch's size; square probe cubes and
        // shadow maps do not match). Cutscenes can draw multi-target models before any
        // single-target draw; those must already count as scene draws (jitter, camera).
        if (camera_motion->Enabled() && db_desc.first && cb_descs[0].first &&
            (guest_targets & 1) && std::popcount(guest_targets) <= 6 &&
            regs.depth_control.depth_enable &&
            regs.depth_control.depth_write_enable) {
            const auto& color = texture_cache.GetImage(cb_descs[0].first);
            const auto& depth = texture_cache.GetImage(db_desc.first);
            const u32 w = depth.info.size.width, h = depth.info.size.height;
            gbuffer_draw = Ps4RtSettings::SceneTargetSize(w, h) &&
                color.info.size.width == w && color.info.size.height == h &&
                (depth.info.pixel_format == vk::Format::eD32Sfloat ||
                 depth.info.pixel_format == vk::Format::eD32SfloatS8Uint) &&
                (color.info.pixel_format == vk::Format::eA2B10G10R10UnormPack32 ||
                 color.info.pixel_format == vk::Format::eA2R10G10B10UnormPack32) &&
                !DisplayRegistry::IsDisplayBuffer(color.info.guest_address);
        }
    }
    if (gbuffer_draw) {
        camera_motion->OnGBufferPass(db_desc.first, {
            regs.viewport_control.zscale_enable ? regs.viewports[0].zscale : 1.f,
            regs.viewport_control.zoffset_enable ? regs.viewports[0].zoffset : 0.f,
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW ? 1.f : 0.f});
    }
    if (upscaler->Enabled() && cb_descs[0].first) {
        upscaler->OnColorTarget(cb_descs[0].first);
    }
    // ps4rt: scene color: a full-size RGBA16F target drawn with the scene depth.
    if (upscaler->Enabled() && db_desc.first && db_desc.first == camera_motion->Depth() &&
        cb_descs[0].first && std::popcount(key.mrt_mask) <= 2) {
        const auto& color = texture_cache.GetImage(cb_descs[0].first);
        const auto& depth = texture_cache.GetImage(db_desc.first);
        if (color.info.pixel_format == vk::Format::eR16G16B16A16Sfloat &&
            color.info.size.width == depth.info.size.width &&
            color.info.size.height == depth.info.size.height) {
            upscaler->OnSceneColor(cb_descs[0].first);
            // Blended geometry without depth writes (not full-screen passes): transparents and
            // effects start. Blended layers writing depth (decals, wet/blood ground) are
            // surfaces and stay in the history.
            if (regs.blend_control[0].enable && !regs.color_buffers[0].info.blend_bypass &&
                !regs.depth_control.depth_write_enable &&
                (regs.num_indices > 6 || regs.num_instances.NumInstances() > 1)) {
                upscaler->OnBlendedSceneDraw();
            } else if (regs.num_indices <= 6 && regs.num_instances.NumInstances() <= 1) {
                // A full-screen pass over the scene (the fog composite): the transparents are in.
                upscaler->OnSceneComposite();
            }
        }
    }
}

static std::pair<u32, u32> GetDrawOffsets(
    const AmdGpu::Regs& regs, const Shader::Info& info,
    const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (fetch_shader) {
        if (vertex_offset == 0 && fetch_shader->vertex_offset_sgpr != -1) {
            vertex_offset = info.UserData()[fetch_shader->vertex_offset_sgpr];
        }
        if (fetch_shader->instance_offset_sgpr != -1) {
            instance_offset = info.UserData()[fetch_shader->instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = Regs().color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf, CbExtent(0));
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    runtime.ClearImage(&image, desc.view_info.range, clear_value);
    ScopeMarkerEnd();
}

// ps4rt: two-stage draw pipeline (vk_draw_pipe.h). Packet layout, 8-byte aligned parts:
// DrawPacket, u16 block indices, u32 block words (RegDirty::BlockWords each), then per stage a
// PacketStage with its user data and flattened user data, then (verification) the full file.
namespace {
enum class PacketKind : u32 { Draw, Task, Dispatch, Special, Indirect, IndirectDispatch };
struct TaskPacket {
    PacketKind kind;
    u32 size;
    Rasterizer::OrderedTask task;
};
struct DrawPacket {
    PacketKind kind;
    const Pipeline* pipeline;
    const PreparedDraw* prepared;
    u32 index_offset;
    u8 is_indexed;
    u8 regs_reset;
    u16 num_blocks;
    u32 num_stages;
    u32 verify;
    std::array<AmdGpu::CbDbExtent, AmdGpu::NUM_COLOR_BUFFERS> cb_extent;
    AmdGpu::CbDbExtent db_extent;
    u64 ring_end; ///< constant ring position after this packet's copies
};
struct PacketStage {
    const Shader::Info* info;
    VAddr pgm_base;
    u32 ud_size;
    u32 flat_size;
    u32 num_ring;
    u32 pad;
};
constexpr u32 AlignPacket(u32 size) {
    return (size + 7) & ~7u;
}
u32 VerifyInterval() {
    static const u32 interval = [] {
        const char* env = std::getenv("PS4RT_PIPE_VERIFY");
        return env ? static_cast<u32>(std::strtoul(env, nullptr, 10)) : 0u;
    }();
    return interval;
}
} // namespace

bool Rasterizer::DrawPipeWanted() {
    const char* env = std::getenv("PS4RT_DRAW_PIPE");
    if (env && env[0]) {
        return env[0] == '1';
    }
    // Draw-pipeline threading is opt-in: some games can lose UI/fade draws.
    // Prefer the serial compatibility path unless PS4RT_DRAW_PIPE=1 is supplied.
    return false;
}

bool Rasterizer::UseDrawPipe() const {
    return draw_pipe && !host_markers_enabled && !Ps4RtToggle::Disabled(Ps4RtToggle::DrawPipeline);
}

bool Rasterizer::OnStageA() const {
#ifdef _WIN32
    // ps4rt (Windows): libc++ compares std::thread::id through winpthreads' pthread_equal,
    // which asks the kernel (GetThreadId) for both threads: two system calls per check, many
    // per draw. The numeric thread id comes from the TEB.
    return static_cast<u32>(gettid()) == liverpool->GetGpuCommandProcessorThreadId();
#else
    return std::this_thread::get_id() == liverpool->GetGpuCommandProcessorThread();
#endif
}

namespace {
/// Stage A: cycles waited per drain site (PS4RT_FRAME_STATS), keyed by function name and line.

} // namespace

void Rasterizer::DrainDrawPipe(u32 reason, u32, const char*) {
    if (draw_pipe && OnStageA()) draw_pipe->Drain(reason);
}


bool Rasterizer::IsGpuSideThread() const {
    return OnStageA() || DrawPipe::OnStageB();
}

bool Rasterizer::IsGpuSideThreadId(u32 tid) const {
    // The userfaultfd thread handles the fault while the faulting thread waits: without locks
    // only if that thread owns the caches. The GPU command thread does not while the draw
    // recording thread may be running, so its faults take the locked path of guest threads.
    if (draw_pipe) {
        return tid == draw_pipe->StageBThreadId();
    }
    return tid == liverpool->GetGpuCommandProcessorThreadId();
}

void Rasterizer::NotePendingGpuWrite(VAddr address, u64 size) {
    if (!draw_pipe || !size) {
        return;
    }
    // Draws write the same buffers over and over: an entry for the range only moves on.
    const u64 position = draw_pipe->Head();
    const VAddr end = address + size;
    for (auto& write : pending_writes) {
        if (address <= write.end && write.begin <= end) {
            write.begin = std::min(write.begin, address);
            write.end = std::max(write.end, end);
            write.position = position;
            pending_min = std::min(pending_min, address);
            pending_max = std::max(pending_max, end);
            return;
        }
    }
    pending_writes.push_back({address, end, position});
    pending_min = std::min(pending_min, address);
    pending_max = std::max(pending_max, end);
}

bool Rasterizer::PendingWriteOverlaps(VAddr address, u64 size) {
    if (pending_writes.empty() || address >= pending_max || address + size <= pending_min) {
        return false;
    }
    // Now and then drop entries whose writes the recording thread has done: the bounds shrink.
    if ((++pending_checks & 63) == 0) {
        std::erase_if(pending_writes, [&](const PendingWrite& write) {
            return draw_pipe->Reached(write.position);
        });
        pending_min = ~VAddr{0};
        pending_max = 0;
        for (const auto& write : pending_writes) {
            pending_min = std::min(pending_min, write.begin);
            pending_max = std::max(pending_max, write.end);
        }
    }
    const VAddr end = address + size;
    for (const auto& write : pending_writes) {
        if (address < write.end && write.begin < end && !draw_pipe->Reached(write.position)) {
            return true;
        }
    }
    return false;
}

void Rasterizer::CollectRingBindings(const Shader::Info& stage, const PreparedDraw* prepared,
                           boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>& out) {
    const PreparedStage* prepared_stage = nullptr;
    if (prepared && !Ps4RtToggle::Disabled(Ps4RtToggle::PreparedResources)) {
        for (u32 i = 0; i < prepared->num_stages; ++i) {
            const auto& candidate = prepared->stages[i];
            if (&candidate.program->info == &stage &&
                candidate.num_buffers == stage.buffers.size()) {
                prepared_stage = &candidate;
                break;
            }
        }
    }
    const u64 alignment = std::max<u64>(instance.StorageMinAlignment(),
                                        instance.UniformMinAlignment());
    const auto copy = [&](u32 index, const void* source, VAddr address, u64 size) {
        const auto offset = constant_ring->Allocate(size, alignment);
        if (!offset) {
            return;
        }
        u8* dst = constant_ring->Data(*offset);
        if (source) {
            std::memcpy(dst, source, size);
        } else {
            memory->CopySparseMemory(address, dst, size);
        }
        constant_ring->Flush(*offset, size);
        out.push_back({index, static_cast<u32>(size), *offset, address});
    };
    for (u32 index = 0; index < stage.buffers.size(); ++index) {
        const auto& desc = stage.buffers[index];
        if (desc.IsSpecial()) {
            if (desc.buffer_type == Shader::BufferType::Flatbuf &&
                !stage.flattened_ud_buf.empty()) {
                copy(index, stage.flattened_ud_buf.data(), 0,
                     stage.flattened_ud_buf.size() * sizeof(u32));
            }
            continue;
        }
        const auto vsharp = prepared_stage ? prepared_stage->buffer_sharps[index]
                                           : desc.GetSharp(stage);
        const VAddr address = vsharp.base_address;
        if (address == 0 || vsharp.GetSize() == 0) {
            continue;
        }
        const u64 size = memory->ClampRangeSize(address, vsharp.GetSize());
        if (desc.is_written) {
            NotePendingGpuWrite(address, size);
            continue;
        }
        // The stream path of BufferCache::ObtainBuffer, taken here: small, read-only, not
        // written by the GPU (now or by work still queued for the recording thread).
        if (size == 0 || size > VideoCore::BufferCache::STREAM_THRESHOLD ||
            buffer_cache.IsRegionGpuModified(address, size) || PendingWriteOverlaps(address, size)) {
            continue;
        }
        copy(index, nullptr, address, size);
    }
}

void Rasterizer::PostDraw(const Pipeline* pipeline, const PreparedDraw* used_prepared,
                          bool is_indexed, u32 index_offset, const AmdGpu::ComputeProgram* cs,
                          const IndirectDraw* indirect) {
    auto& dirty = liverpool->pipe_dirty;
    if (!pipe_synced) {
        // Stage B has not run yet: its register copy starts as this one.
        pipe_regs = liverpool->regs;
        dirty.Clear();
        pipe_synced = true;
    }
    std::array<u16, AmdGpu::RegDirty::NumBlocks> blocks;
    u32 num_blocks = 0;
#ifdef __GLIBCXX__
    for (size_t block = dirty.blocks._Find_first(); block < dirty.blocks.size();
         block = dirty.blocks._Find_next(block)) {
        blocks[num_blocks++] = static_cast<u16>(block);
    }
#else
    if (dirty.blocks.any()) {
        for (size_t block = 0; block < dirty.blocks.size(); ++block) {
            if (dirty.blocks.test(block)) {
                blocks[num_blocks++] = static_cast<u16>(block);
            }
        }
    }
#endif
    const auto stages =
        pipeline ? pipeline->GetStages() : std::span<const Shader::Info* const>{};
    // Constants: copied here into the ring, the recording thread only binds them.
    thread_local std::array<boost::container::static_vector<RingBinding, Shader::NUM_BUFFERS>,
                            Shader::MaxStageTypes>
        rings;
    u32 num_stages = 0;
    u32 size = sizeof(DrawPacket) + AlignPacket(num_blocks * sizeof(u16)) +
               num_blocks * AmdGpu::RegDirty::BlockWords * sizeof(u32);
    for (const auto* stage : stages) {
        if (stage) {
            auto& ring = rings[num_stages];
            ring.clear();
            if (constant_ring && !Ps4RtToggle::Disabled(Ps4RtToggle::ConstantRing)) {
                CollectRingBindings(*stage, used_prepared, ring);
            }
            ++num_stages;
            size += sizeof(PacketStage) +
                    AlignPacket(u32(stage->user_data.size() + stage->flattened_ud_buf.size()) *
                                sizeof(u32)) +
                    u32(ring.size() * sizeof(RingBinding));
        }
    }
    if (cs) {
        size += AlignPacket(sizeof(AmdGpu::ComputeProgram));
    }
    if (indirect) {
        size += AlignPacket(sizeof(IndirectDraw));
    }
    const u32 interval = VerifyInterval();
    const bool verify = interval && (draw_pipe->packets % interval) == 0;
    if (verify) {
        size += sizeof(AmdGpu::Regs);
    }

    u8* out = draw_pipe->Begin(size);
    u8* const start = out;
    auto& packet = *reinterpret_cast<DrawPacket*>(out);
    packet.kind = cs         ? (indirect ? PacketKind::IndirectDispatch : PacketKind::Dispatch)
                  : indirect ? PacketKind::Indirect
                  : pipeline ? PacketKind::Draw
                             : PacketKind::Special;
    packet.pipeline = pipeline;
    packet.prepared = used_prepared;
    packet.index_offset = index_offset;
    packet.is_indexed = is_indexed;
    packet.regs_reset = dirty.reset;
    packet.num_blocks = static_cast<u16>(num_blocks);
    packet.num_stages = num_stages;
    packet.verify = verify;
    packet.cb_extent = liverpool->last_cb_extent;
    packet.db_extent = liverpool->last_db_extent;
    packet.ring_end = constant_ring ? constant_ring->Position() : 0;
    out += sizeof(DrawPacket);
    std::memcpy(out, blocks.data(), num_blocks * sizeof(u16));
    out += AlignPacket(num_blocks * sizeof(u16));
    for (u32 i = 0; i < num_blocks; ++i) {
        std::memcpy(out,
                    liverpool->regs.reg_array.data() + blocks[i] * AmdGpu::RegDirty::BlockWords,
                    AmdGpu::RegDirty::BlockWords * sizeof(u32));
        out += AmdGpu::RegDirty::BlockWords * sizeof(u32);
    }
    u32 stage_index = 0;
    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        const auto& ring = rings[stage_index++];
        auto& header = *reinterpret_cast<PacketStage*>(out);
        header = {stage,
                  stage->pgm_base,
                  static_cast<u32>(stage->user_data.size()),
                  static_cast<u32>(stage->flattened_ud_buf.size()),
                  static_cast<u32>(ring.size()),
                  0};
        out += sizeof(PacketStage);
        std::memcpy(out, ring.data(), ring.size() * sizeof(RingBinding));
        out += ring.size() * sizeof(RingBinding);
        std::memcpy(out, stage->user_data.data(), stage->user_data.size_bytes());
        std::memcpy(out + stage->user_data.size_bytes(), stage->flattened_ud_buf.data(),
                    stage->flattened_ud_buf.size() * sizeof(u32));
        out += AlignPacket(u32(stage->user_data.size() + stage->flattened_ud_buf.size()) *
                           sizeof(u32));
    }
    if (cs) {
        std::memcpy(out, cs, sizeof(AmdGpu::ComputeProgram));
        out += AlignPacket(sizeof(AmdGpu::ComputeProgram));
    }
    if (indirect) {
        std::memcpy(out, indirect, sizeof(IndirectDraw));
        out += AlignPacket(sizeof(IndirectDraw));
    }
    if (verify) {
        std::memcpy(out, &liverpool->regs, sizeof(AmdGpu::Regs));
        out += sizeof(AmdGpu::Regs);
    }
    dirty.Clear();
    draw_pipe->Commit(static_cast<u32>(out - start));
    ;
}

void Rasterizer::RetireSubmission() {
    if (!draw_pipe || !OnStageA()) {
        return;
    }
    while (!pipe_keepalive.empty() && draw_pipe->Reached(pipe_keepalive.front().first)) {
        pipe_keepalive.pop_front();
    }
    if (!draw_pipe->Idle()) {
        pipe_keepalive.emplace_back(draw_pipe->Head(), draw_prep->KeepCurrent());
    }
}

bool Rasterizer::RunInOrder(OrderedTask task, const void* data, u32 size, u64 toggle) {
    if (!UseDrawPipe() || Ps4RtToggle::Disabled(toggle)) {
        DrainDrawPipe(DrawPipe::ReasonRasterizer);
        task(*this, static_cast<const u8*>(data));
        return false;
    }
    const u32 total = sizeof(TaskPacket) + AlignPacket(size);
    u8* out = draw_pipe->Begin(total);
    *reinterpret_cast<TaskPacket*>(out) = {PacketKind::Task, size, task};
    std::memcpy(out + sizeof(TaskPacket), data, size);
    draw_pipe->Commit(total);
    return true;
}

void Rasterizer::RunDrawPacket(void* context, const u8* data, u32 size) {
    auto& self = *static_cast<Rasterizer*>(context);
    if (*reinterpret_cast<const PacketKind*>(data) == PacketKind::Task) {
        const auto& task = *reinterpret_cast<const TaskPacket*>(data);
        task.task(self, data + sizeof(TaskPacket));
        return;
    }
    const auto& packet = *reinterpret_cast<const DrawPacket*>(data);
    const u8* in = data + sizeof(DrawPacket);
    const auto* blocks = reinterpret_cast<const u16*>(in);
    in += AlignPacket(packet.num_blocks * sizeof(u16));
    auto& regs = self.pipe_regs;
    if (packet.regs_reset) {
        regs.SetDefaults();
    }
    for (u32 i = 0; i < packet.num_blocks; ++i) {
        std::memcpy(regs.reg_array.data() + blocks[i] * AmdGpu::RegDirty::BlockWords, in,
                    AmdGpu::RegDirty::BlockWords * sizeof(u32));
        in += AmdGpu::RegDirty::BlockWords * sizeof(u32);
    }
    ASSERT(packet.num_stages <= Shader::Info::MaxUdSnapshots);
    for (u32 i = 0; i < packet.num_stages; ++i) {
        const auto& stage = *reinterpret_cast<const PacketStage*>(in);
        in += sizeof(PacketStage);
        self.ring_stages[i] = {stage.info, reinterpret_cast<const RingBinding*>(in),
                               stage.num_ring};
        in += stage.num_ring * sizeof(RingBinding);
        const auto* words = reinterpret_cast<const u32*>(in);
        Shader::Info::ud_snapshots[i] = {stage.info, words, stage.ud_size, words + stage.ud_size,
                                         stage.flat_size, stage.pgm_base};
        in += AlignPacket((stage.ud_size + stage.flat_size) * sizeof(u32));
    }
    Shader::Info::num_ud_snapshots = packet.num_stages;
    self.num_ring_stages = packet.num_stages;
    const AmdGpu::ComputeProgram* cs = nullptr;
    if (packet.kind == PacketKind::Dispatch || packet.kind == PacketKind::IndirectDispatch) {
        cs = reinterpret_cast<const AmdGpu::ComputeProgram*>(in);
        in += AlignPacket(sizeof(AmdGpu::ComputeProgram));
    }
    const IndirectDraw* indirect = nullptr;
    if (packet.kind == PacketKind::Indirect || packet.kind == PacketKind::IndirectDispatch) {
        indirect = reinterpret_cast<const IndirectDraw*>(in);
        in += AlignPacket(sizeof(IndirectDraw));
    }
    if (packet.verify) {
        // The resource tables the GPU thread walked must read the same now: else a write
        // still queued here (WriteData, DMA) changed them after that thread read them.
        static u32 flat_reports = 0;
        for (u32 i = 0; i < packet.num_stages && flat_reports < 32; ++i) {
            const auto& snapshot = Shader::Info::ud_snapshots[i];
            const auto& srt = snapshot.info->srt_info;
            // Stages the pipeline selection did not refresh (no user data) keep their tables.
            if (!srt.walker_func || snapshot.flat_size == 0 || snapshot.user_data_size == 0) {
                continue;
            }
            std::vector<u32> flat(snapshot.flat_size);
            std::memcpy(flat.data(), snapshot.user_data,
                        std::min(snapshot.user_data_size, snapshot.flat_size) * sizeof(u32));
            // Pointers in the tables may be stale by now: a fault only skips the check.
            sigjmp_buf recover;
            if (sigsetjmp(recover, 0)) {
                runtime_fault_recover = nullptr;
                continue;
            }
            runtime_fault_recover = &recover;
            srt.walker_func(snapshot.user_data, flat.data());
            runtime_fault_recover = nullptr;
            if (std::memcmp(flat.data(), snapshot.flat, snapshot.flat_size * sizeof(u32)) != 0) {
                u32 first = 0, count = 0;
                for (u32 w = 0; w < snapshot.flat_size; ++w) {
                    if (flat[w] != snapshot.flat[w]) {
                        first = count++ ? first : w;
                    }
                }
                std::printf("Draw pipe: resource tables of shader %016llx changed after the GPU "
                            "thread read them (%u of %u words, first %u: %#x -> %#x, user data "
                            "%u words)\n",
                            static_cast<unsigned long long>(snapshot.info->pgm_hash), count,
                            snapshot.flat_size, first, snapshot.flat[first], flat[first],
                            snapshot.user_data_size);
                ++flat_reports;
            }
        }
        const auto* live = reinterpret_cast<const AmdGpu::Regs*>(in);
        in += sizeof(AmdGpu::Regs);
        static u32 reports = 0;
        for (u32 block = 0; block < AmdGpu::RegDirty::NumBlocks && reports < 32; ++block) {
            const u32 at = block * AmdGpu::RegDirty::BlockWords;
            if (std::memcmp(regs.reg_array.data() + at, live->reg_array.data() + at,
                            AmdGpu::RegDirty::BlockWords * sizeof(u32)) != 0) {
                for (u32 w = at; w < at + AmdGpu::RegDirty::BlockWords; ++w) {
                    if (regs.reg_array[w] != live->reg_array[w]) {
                        std::printf("Draw pipe: register copy differs at word %#x (%#x vs %#x)\n",
                                    w, regs.reg_array[w], live->reg_array[w]);
                        ++reports;
                        break;
                    }
                }
            }
        }
    }
    ASSERT(static_cast<u32>(in - data) == size);
    stage_regs = &regs;
    self.pipe_cb_extent = packet.cb_extent;
    self.pipe_db_extent = packet.db_extent;
    if (packet.kind == PacketKind::Special) {
        // A draw FilterDraw handles (fast clear elimination, resolve, depth copy, skip).
        ;
        self.scheduler.PopPendingOperations();
        self.FilterDraw();
    } else if (cs) {
        stage_cs = cs;
        const auto* pipeline = static_cast<const ComputePipeline*>(packet.pipeline);
        if (indirect) {
            self.DispatchIndirectRecord(pipeline, indirect->args, indirect->stride);
        } else {
            self.DispatchRecord(pipeline);
        }
        stage_cs = nullptr;
    } else if (indirect) {
        self.DrawIndirectRecord(static_cast<const GraphicsPipeline*>(packet.pipeline),
                                packet.is_indexed, *indirect);
    } else {
        self.DrawRecord(static_cast<const GraphicsPipeline*>(packet.pipeline), packet.prepared,
                        packet.is_indexed, packet.index_offset);
    }
    Shader::Info::num_ud_snapshots = 0;
    self.num_ring_stages = 0;
    if (self.constant_ring) {
        self.constant_ring->Stamp(packet.ring_end);
    }
}



bool Rasterizer::FilterDrawPasses() const {
    // FilterDraw's checks without its side effects: false when it would skip the draw or run
    // a pass of its own (fast clear elimination, resolve, depth/stencil copy).
    const auto& regs = Regs();
    using Mode = AmdGpu::ColorControl::OperationMode;
    const auto mode = regs.color_control.mode;
    if (mode == Mode::EliminateFastClear || mode == Mode::FmaskDecompress ||
        mode == Mode::Resolve || regs.primitive_type == AmdGpu::PrimitiveType::None) {
        return false;
    }
    const bool depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const bool stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    return !(mode == Mode::Disable && (depth_copy || stencil_copy));
}


void Rasterizer::Draw(bool is_indexed, u32 index_offset, const PreparedDraw* prepared) {
    RENDERER_TRACE;
    ;

    // ps4rt: with the draw pipeline this thread only selects the pipeline and hands the draw
    // to the recording thread (DrawRecord there); draws FilterDraw handles itself run here.
    const bool pipelined = UseDrawPipe();
    if (pipelined && !FilterDrawPasses() &&
        !Ps4RtToggle::Disabled(Ps4RtToggle::PipelinedMemoryWrites)) {
        PostDraw(nullptr, nullptr, false, 0);
        return;
    }
    if (!pipelined || !FilterDrawPasses()) {
        DrainDrawPipe(DrawPipe::ReasonDraw);
        ;
        scheduler.PopPendingOperations();
        if (!FilterDraw()) {
            return;
        }
    }
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline({}, prepared);
    const PreparedDraw* used_prepared = pipeline_cache.UsedPrepared();
    if (prepared) {
        ;
    }
    if (!pipeline) {
        return;
    }
    if (pipelined && FilterDrawPasses()) {
        PostDraw(pipeline, used_prepared, is_indexed, index_offset);
        return;
    }
    DrawRecord(pipeline, used_prepared, is_indexed, index_offset);
}





void Rasterizer::DrawRecord(const GraphicsPipeline* pipeline, const PreparedDraw* used_prepared,
                            bool is_indexed, u32 index_offset) {
    if (DrawPipe::OnStageB()) {
        ;
        scheduler.PopPendingOperations();
    }
    ;
    const auto& regs = Regs();
    // Depth-only clears retain color-buffer registers from the preceding copy.
    // Only a draw actually writing MRT0 can begin a new displayed frame.
    const bool writes_display = (pipeline->GetGraphicsKey().mrt_mask & 1) &&
        regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable &&
        regs.color_target_mask.GetMask(0) && regs.color_buffers[0] &&
        DisplayRegistry::IsDisplayBuffer(regs.color_buffers[0].Address());
    if (writes_display) ;
    const bool frame_start = writes_display && (!Ps4RtSettings::Get().native_scene ||
        Ps4RtCamera::SceneCopy(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                                  pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash));
    // ps4rt: the pass copying a finished frame to a display buffer; the previous draw's
    // target is that frame.
    if (camera_motion->Enabled() && frame_start) {
        last_display_buffer = regs.color_buffers[0].Address();
        scene_started = false;
        upscaler->BeforeDisplay();
        camera_motion->OnDisplayPass(cb_descs[0].first);
        if (upscaler->OnFrameStart()) {
            object_motion->InvalidateHistory();
            camera_motion->InvalidateHistory();
        }
        camera_motion->SetJitter(upscaler->Jitter());
        object_motion->OnFrameStart();
        ;



    } else if (writes_display &&
               regs.color_buffers[0].Address() != last_display_buffer) {
        // Without an upscaler: still note the display buffer, once per display buffer since the
        // HUD draws into it as well. (Diagnostics count frames at guest flips: OnGuestFlip.)
        // ponytail: assumes flips rotate display buffers; a single-buffer game counts one frame.
        last_display_buffer = regs.color_buffers[0].Address();
        ;
    }
    bind_prepared = used_prepared;
    motion_draw = pipeline->GetGraphicsKey().motion_vectors;
    motion_geometry = 0;

    if constexpr (Ps4RtDiag::Built) {
        SnapshotDraw(pipeline);
    }
    PrepareRenderState(pipeline);
    ;
    {
        { // ps4rt (ultrawide): the HUD is laid out on a centred 1920x1080 canvas and parks
          // hidden elements just outside it; clip display-buffer draws (not FXAA) to the canvas.
            g_uw_canvas_x0 = g_uw_canvas_x1 = g_uw_shift = 0; g_uw_stretch = 1.0f; g_uw_movie = false;
            g_hud_scale = 1.0f; g_hud_pixel = false; g_hud_anchor = false;
            // Dialogue emotion bubbles are 3D billboards: a quad of world units (~0.3 across) at the
            // speaker's head, seen through a perspective camera, so the GPU already places them for
            // the real display and the canvas rules below (HUD scale, ultrawide shift/clip) would
            // move them. Every HUD sprite carries the same kind of 176-byte camera buffer, but canvas
            // elements (tags included) are laid out in canvas pixels: one under 2 units across on
            // both axes would be a sub-2-pixel dot, so world-unit geometry identifies the billboards.
            auto perspective_draw = [&] {
                const Shader::Info* vsi = nullptr;
                for (const auto* stage : pipeline->GetStages())
                    if (stage && stage->sw_stage == Shader::SwStage::Vertex) vsi = stage;
                if (!vsi) return false;
                bool persp = false;
                const float* world = nullptr;
                for (const auto& desc : vsi->buffers) {
                    if (desc.IsSpecial()) continue;
                    const auto b = desc.GetSharp(*vsi);
                    const u32 size = b.GetSize();
                    if ((size != 176 && size != 128) || !b.base_address ||
                        !memory->IsValidGpuMapping(b.base_address, size) ||
                        memory->ClampRangeSize(b.base_address, size) != size) continue;
                    const float* m = reinterpret_cast<const float*>(b.base_address);
                    if (size == 176)
                        persp |= m[11] < -0.5f && std::abs(m[15]) < 1e-4f && m[0] > 0.0f && m[5] > 0.0f;
                    else if (!world)
                        world = m;
                }
                const auto& fetch = pipeline->GetFetchShader();
                if (!persp || !world || !fetch) return false;
                for (const auto& attrib : fetch->attributes) {
                    if (attrib.semantic != 0 || attrib.num_elements < 2) continue;
                    const auto b = attrib.GetSharp(*vsi);
                    const u32 stride = b.GetStride(), records = b.num_records;
                    const u64 bytes = u64(stride) * records;
                    if (stride < 8 || !records || records > 64 || !b.base_address ||
                        !memory->IsValidGpuMapping(b.base_address, bytes) ||
                        memory->ClampRangeSize(b.base_address, bytes) != bytes) return false;
                    float lo = 1e9f, hi = -1e9f, ylo = 1e9f, yhi = -1e9f;
                    for (u32 r = 0; r < records; ++r) {
                        const float* v = reinterpret_cast<const float*>(b.base_address + u64(r) * stride);
                        const float z = attrib.num_elements > 2 ? v[2] : 0.0f;
                        const float x = world[0] * v[0] + world[4] * v[1] + world[8] * z + world[12];
                        const float y = world[1] * v[0] + world[5] * v[1] + world[9] * z + world[13];
                        lo = std::min(lo, x); hi = std::max(hi, x);
                        ylo = std::min(ylo, y); yhi = std::max(yhi, y);
                    }
                    // Collapsed (zero-size) sprites are excluded: inactive tag pieces still set the tag pivot below.
                    return hi - lo > 1e-4f && hi - lo < 2.0f && yhi - ylo > 1e-4f && yhi - ylo < 2.0f;
                }
                return false;
            };
            if (cb_descs[0].first) {
                const auto& ci = texture_cache.GetImage(cb_descs[0].first).info;
                if (ci.size.height >= 1080 && (ci.size.width > 1920 || ci.size.height > 1080) &&
                    DisplayRegistry::IsDisplayBuffer(ci.guest_address) && !perspective_draw()) {
                    // Virtual 1080-tall display of the same aspect (= the real one at 1080).
                    const float hud_scale = float(ci.size.height) / 1080.0f;
                    const u32 virtual_w = u32(std::lround(float(ci.size.width) / hud_scale));
                    bool hud_draw = true; // false: full-display draws (FXAA) keep the game's viewport
                    // vs 8d0e6514: top-left pixel quads. Full-size sources (FXAA) cover the
                    // display; smaller ones (pilot feeds) use 1920-canvas pixels: shift them.
                    bool screen_quad = false, full_source = false;
                    if (pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash == 0x8d0e6514ull) {
                        screen_quad = true;
                        for (const auto* stage : pipeline->GetStages()) {
                            if (!stage) continue;
                            for (const auto& im : stage->images)
                                full_source |= im.GetSharp(*stage).width + 1 >= ci.size.width;
                        }
                    }
                    bool full_canvas = false, have_range = false;
                    float range_lo = 0.0f, range_hi = 0.0f, anchor_x = 0.0f, anchor_y = 0.0f, hole_x = 0.0f,
                          hole_y = 0.0f, range_ylo = 0.0f, range_yhi = 0.0f,
                          range_zlo = 0.0f, range_zhi = 0.0f, world_sx = 0.0f, world_sy = 0.0f;
                    if (screen_quad && !full_source) {
                        // Pilot feeds: attr0 is in 1920-canvas pixels (top-left origin).
                        const Shader::Info* vsi = nullptr;
                        for (const auto* stage : pipeline->GetStages())
                            if (stage && stage->sw_stage == Shader::SwStage::Vertex) vsi = stage;
                        const auto& fetch = pipeline->GetFetchShader();
                        if (vsi && fetch) {
                            for (const auto& attrib : fetch->attributes) {
                                if (attrib.semantic != 0 || attrib.num_elements < 2) continue;
                                const auto b = attrib.GetSharp(*vsi);
                                const u32 stride = b.GetStride(), records = b.num_records;
                                const u64 bytes = u64(stride) * records;
                                if (!stride || stride < 8 || !records || records > 4096 || !b.base_address ||
                                    !memory->IsValidGpuMapping(b.base_address, bytes) ||
                                    memory->ClampRangeSize(b.base_address, bytes) != bytes) break;
                                float lo = 1e9f, hi = -1e9f;
                                for (u32 r = 0; r < records; ++r) {
                                    const float x = *reinterpret_cast<const float*>(b.base_address + u64(r) * stride);
                                    lo = std::min(lo, x); hi = std::max(hi, x);
                                }
                                range_lo = lo - 960.0f; range_hi = hi - 960.0f; have_range = true;
                                break;
                            }
                        }
                    }
                    if (!screen_quad) {
                        // Element x range on the canvas: local attr0 positions x the draw's
                        // 128-byte world matrix (translation in floats 12-14).
                        const Shader::Info* vsi = nullptr;
                        for (const auto* stage : pipeline->GetStages())
                            if (stage && stage->sw_stage == Shader::SwStage::Vertex) vsi = stage;
                        const float* world = nullptr;
                        if (vsi) {
                            for (const auto& desc : vsi->buffers) {
                                if (desc.IsSpecial()) continue;
                                const auto b = desc.GetSharp(*vsi);
                                const u32 wsize = b.GetSize();
                                if ((wsize == 128 || wsize == 64) && b.base_address &&
                                    memory->IsValidGpuMapping(b.base_address, wsize) &&
                                    memory->ClampRangeSize(b.base_address, wsize) == wsize) {
                                    world = reinterpret_cast<const float*>(b.base_address);
                                    break;
                                }
                            }
                        }
                        const auto& fetch = pipeline->GetFetchShader();
                        if (vsi && world && fetch) {
                            for (const auto& attrib : fetch->attributes) {
                                if (attrib.semantic != 0 || attrib.num_elements < 2) continue;
                                const auto b = attrib.GetSharp(*vsi);
                                const u32 stride = b.GetStride(), records = b.num_records;
                                const u64 bytes = u64(stride) * records;
                                if (records > 4096) {
                                    // Few vertices out of a large shared buffer (skip-prompt ring):
                                    // use the draw's origin as its position.
                                    range_lo = range_hi = anchor_x = world[12];
                                    anchor_y = world[13];
                                    range_ylo = range_yhi = world[13];
                                    range_zlo = range_zhi = world[14];
                                    world_sx = world[0]; world_sy = world[5];
                                    have_range = true;
                                    break;
                                }
                                if (!stride || stride < 8 || !records || !b.base_address ||
                                    !memory->IsValidGpuMapping(b.base_address, bytes) ||
                                    memory->ClampRangeSize(b.base_address, bytes) != bytes) break;
                                float lo = 1e9f, hi = -1e9f, inner_sum = 0.0f, inner_ysum = 0.0f;
                                float ylo = 1e9f, yhi = -1e9f, zlo = 1e9f, zhi = -1e9f;
                                u32 inner_n = 0;
                                for (u32 r = 0; r < records; ++r) {
                                    const float* v = reinterpret_cast<const float*>(b.base_address + u64(r) * stride);
                                    const float z = attrib.num_elements > 2 ? v[2] : 0.0f;
                                    const float x = world[0] * v[0] + world[4] * v[1] + world[8] * z + world[12];
                                    const float y = world[1] * v[0] + world[5] * v[1] + world[9] * z + world[13];
                                    lo = std::min(lo, x); hi = std::max(hi, x);
                                    ylo = std::min(ylo, y); yhi = std::max(yhi, y);
                                    const float wz = world[2] * v[0] + world[6] * v[1] + world[10] * z + world[14];
                                    zlo = std::min(zlo, wz); zhi = std::max(zhi, wz);
                                    if (std::abs(x) < 940.0f && std::abs(y) < 520.0f) {
                                        inner_sum += x; inner_ysum += y; ++inner_n;
                                    }
                                }
                                full_canvas = lo <= -900.0f && hi >= 900.0f;
                                hole_x = inner_n ? inner_sum / float(inner_n) : 0.0f;
                                hole_y = inner_n ? inner_ysum / float(inner_n) : 0.0f;
                                range_ylo = ylo; range_yhi = yhi; range_zlo = zlo; range_zhi = zhi;
                                range_lo = lo; range_hi = hi; have_range = true; anchor_x = world[12]; anchor_y = world[13];
                                world_sx = world[0]; world_sy = world[5];
                                break;
                            }
                        }
                    }
                    // Side of the last full-canvas depth mask this frame: the masked content
                    // drawn after it (minimap) must stay with the hole.
                    // Minimap depth mask: this frame's, else the previous frame's (draw order varies).
                    struct UwMask { s32 side; float z, hx, hy; };
                    static UwMask mask_now{}, mask_last{};
                    // Frame boundary: the display buffer flips (works in cutscenes too).
                    struct UwBox { float lo, hi, ylo, yhi; };
                    static std::vector<UwBox> boxes_cur, boxes_prev;
                    struct UwPanel { float lo, hi, ylo, yhi; s32 side; bool base = true; float z = 0.0f; };
                    static std::vector<UwPanel> panels_cur, panels_prev;
                    static std::vector<UwPanel> movers_cur, movers_prev;
                    static std::unordered_map<u64, u32> fam_cur, fam_prev; // wide background layers
                    static bool menu_layout_cur = false, menu_layout_prev = false;
                    static std::unordered_map<u64, u32> fx_cur, fx_prev;
                    // Head-tag pivots of this frame, one per unit (see world tags below).
                    static std::vector<std::pair<float, float>> tag_pivots;
                    // Tag roots: the zero-size sprite each tag draws at its unit's projected point.
                    static std::vector<std::pair<float, float>> tag_roots_cur, tag_roots_prev;
                    // Projected HUD-layer panels of this frame (repair gauge): box, depth, anchor.
                    struct UwProj { float lo, hi, ylo, yhi, z, ax, ay; };
                    static std::vector<UwProj> proj_panels;
                    static VAddr uw_disp = 0;
                    if (ci.guest_address != uw_disp) {
                        uw_disp = ci.guest_address;
                        mask_last = mask_now; mask_now = {};
                        boxes_prev.swap(boxes_cur); boxes_cur.clear();
                        panels_prev.swap(panels_cur); panels_cur.clear();
                        movers_prev.swap(movers_cur); movers_cur.clear();
                        menu_layout_prev = menu_layout_cur; menu_layout_cur = false;
                        fx_prev.swap(fx_cur); fx_cur.clear();
                        fam_prev.swap(fam_cur); fam_cur.clear();
                        tag_pivots.clear();
                        tag_roots_prev.swap(tag_roots_cur); tag_roots_cur.clear();
                        proj_panels.clear();
                    }
                    const UwMask& mask = mask_now.side ? mask_now : mask_last;
                    const s32 mask_side = mask.side;
                    const float mask_z = mask.z;
                    const s32 margin_px = s32(virtual_w - 1920) / 2;
                    // A movie frame: the game draws it as a UI quad whose pixel shader converts the
                    // decoder's 4:2:0 planes (FUN_00addef0 binds Y/U/V(/A) to texture slots 3-6):
                    // one single-channel 8-bit plane W x H and two at half its size. Additive
                    // ones (the "MISSION START" overlay) only light the scene: not a movie frame.
                    const auto movie_planes = [&] {
                        if (regs.blend_control[0].enable &&
                            regs.blend_control[0].color_dst_factor == AmdGpu::BlendControl::BlendFactor::One)
                            return false;
                        u32 w[8], h[8], n = 0;
                        for (const auto* stage : pipeline->GetStages()) {
                            if (!stage || stage->sw_stage != Shader::SwStage::Fragment) continue;
                            for (const auto& im : stage->images) {
                                const auto sh = im.GetSharp(*stage);
                                if (sh.GetDataFmt() != AmdGpu::DataFormat::Format8 || n == 8) continue;
                                w[n] = sh.width + 1; h[n] = sh.height + 1; ++n;
                            }
                        }
                        for (u32 y = 0; y < n; ++y) {
                            u32 half = 0;
                            for (u32 c = 0; c < n; ++c)
                                half += c != y && w[c] == (w[y] + 1) / 2 && h[c] == (h[y] + 1) / 2;
                            if (w[y] >= 64 && half >= 2) return true;
                        }
                        return false;
                    };
                    const s32 hole_side = hole_x <= -400.0f ? -1 : hole_x >= 400.0f ? 1 : 0;
                    const float display_w = float(virtual_w);
                    if (screen_quad && have_range && range_hi - range_lo >= display_w - 2.0f) {
                        // Pixel quad already covering the display: leave it alone.
                        hud_draw = false;
                    } else if (screen_quad && have_range && range_lo <= -950.0f && range_hi >= 950.0f) {
                        // Pixel quad covering the 1920 canvas (fades): stretch it from the left
                        // edge over the display.
                        g_uw_stretch = display_w / 1920.0f;
                        g_uw_shift = s32(0.5f * display_w * (g_uw_stretch - 1.0f));
                    } else if (full_canvas && mask_side && regs.depth_control.depth_enable && range_zhi < mask_z &&
                               range_yhi - range_ylo >= 600.0f && range_zlo >= -24.0f) {
                        // range_zlo >= -24: minimap depths only. The pause menu's full-screen dim
                        // (z -120..-115) is behind the mask too but must stretch over the display
                        // (it was clipped to the right half).
                        // Canvas-sized content behind the mask (large minimap tiles, ≥1000 tall):
                        // stays with the mask; it is not a mask of its own even when it writes
                        // depth. Short full-width strips at negative depth (the top cockpit band)
                        // are not map content: they fall through to the stretch below.
                        g_uw_shift = mask_side * margin_px;
                        g_uw_canvas_x0 = margin_px + g_uw_shift;
                        g_uw_canvas_x1 = g_uw_canvas_x0 + 1920;
                    } else if (full_canvas && regs.depth_control.depth_write_enable && hole_side) {
                        // Full-canvas depth mask with a hole (minimap circle): move the whole
                        // mask with its hole instead of stretching the hole.
                        mask_now = {hole_side, range_zlo, hole_x, hole_y};
                        g_uw_shift = hole_side * margin_px;
                        g_uw_canvas_x0 = margin_px + g_uw_shift;
                        g_uw_canvas_x1 = g_uw_canvas_x0 + 1920;
                    } else if (full_canvas && movie_planes()) {
                        // Pre-rendered movies are 16:9: keep them centred on the canvas (black
                        // sides) instead of stretching them like a full-canvas overlay.
                        g_uw_canvas_x0 = margin_px;
                        g_uw_canvas_x1 = margin_px + 1920;
                        g_uw_movie = true;
                    } else if (full_canvas) {
                        g_uw_stretch = float(virtual_w) / 1920.0f;
                    } else if (!(screen_quad && full_source)) {
                        const s32 margin = s32(virtual_w - 1920) / 2;
                        // Edge anchoring: elements clearly on one side of the canvas move with
                        // that edge of the display. PS4RT_UW_HUD: unset or "battle" (default) =
                        // only on the battle HUD (minimap mask present), menus, dialogue and load
                        // screens keep their 16:9 layout centred; "edges" = on every screen;
                        // "center" = never (the whole HUD stays centred).
                        static const char* const uw_hud = std::getenv("PS4RT_UW_HUD");
                        static const bool edges = !(uw_hud && std::string_view(uw_hud) == "center");
                        static const bool battle_only =
                            !(uw_hud && (std::string_view(uw_hud) == "edges" || std::string_view(uw_hud) == "center"));
                        s32 side = 0;
                        // Depth-tested draws behind the mask (minimap content) only show through
                        // its hole: keep them with the mask.
                        const float cx = 0.5f * (range_lo + range_hi), cy = 0.5f * (range_ylo + range_yhi);
                        const bool reaches_hole = range_lo < mask.hx + 250.0f && range_hi > mask.hx - 250.0f &&
                                                  range_ylo < mask.hy + 250.0f && range_yhi > mask.hy - 250.0f;
                        // Minimap content is centred inside the circle (≤190 px from the hole centre)
                        // or is a large map tile; tags passing near the minimap are neither.
                        // Tag-sized pieces (< 100 px) only count once this frame's mask is drawn:
                        // ally tags/chevrons come before the mask, blips after it. The minimap
                        // backing disc (334 px, also before the mask) keeps the old behaviour.
                        // Depth floor: the HUD is drawn back to front and no minimap piece is deeper
                        // than its backing disc (z -23.1); head tags and their bars (z -30..-40) that
                        // pass over the minimap are never its content (2026-11-04 frame trace).
                        // Minimap pieces sit at -23.1..-10 (disc, tiles, blips) or >= 0; the ally
                        // chevron markers draw at -6.4..-6.8 and pass the minimap's lower edge
                        // (139 px from its centre), so that band is never map content.
                        const bool marker_band = range_zlo > -8.0f && range_zhi < -1.5f;
                        const bool map_depth = range_zlo >= -24.0f && !marker_band;
                        // Small pieces must sit inside the disc itself (radius ~128; every small map
                        // piece logged is within 129 px of the centre). Larger map pieces (bearing
                        // label, rim strips) reach 190. A 28x32 world marker passing just below the
                        // minimap (181 px) was taken for map content and jumped for a frame.
                        const bool small_piece = range_hi - range_lo < 100.0f && range_yhi - range_ylo < 100.0f;
                        const float circle_r = small_piece ? 140.0f : 190.0f;
                        const bool in_circle = map_depth && (mask_now.side != 0 || range_hi - range_lo >= 100.0f) &&
                                               (cx - mask.hx) * (cx - mask.hx) + (cy - mask.hy) * (cy - mask.hy) <
                                               circle_r * circle_r;
                        const bool behind_mask = mask_side && have_range && !screen_quad && reaches_hole &&
                                                 regs.depth_control.depth_enable && range_zhi < mask_z &&
                                                 (in_circle || range_hi - range_lo >= 400.0f);
                        const bool over_map = behind_mask ||
                                              (mask_side && have_range && !screen_quad && range_hi - range_lo < 600.0f &&
                                               in_circle);
                        if (edges && have_range) {
                            const float c = 0.5f * (range_lo + range_hi);
                            if (over_map) side = mask_side;
                            else if (range_hi - range_lo < 1200.0f && c <= -400.0f && range_hi < 300.0f) side = -1;
                            else if (range_hi - range_lo < 1200.0f && c >= 400.0f && range_lo > -300.0f) side = 1;
                            // Large off-centre panels follow their own origin.
                            else if (anchor_x <= -500.0f && c <= -200.0f) side = -1;
                            else if (anchor_x >= 500.0f && c >= 200.0f) side = 1;
                        }
                        const bool point = have_range && range_hi == range_lo;
                        const u64 fx_key = (pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash * 31) ^
                                           u64(s64(range_zlo * 16.0f));
                        bool in_box = false, followed = false; // followed: moved with a panel or a grid element
                        if (edges && have_range && !over_map && !screen_quad) {
                            // Parts of a centred box (dialogue bubble and its icon, the speaker
                            // portrait beside it) stay with the box.
                            const float w = range_hi - range_lo;
                            for (const auto* list : {&boxes_cur, &boxes_prev}) {
                                for (const auto& bx : *list) {
                                    if (range_yhi < bx.ylo || range_ylo > bx.yhi) continue;
                                    // Mostly inside vertically (≥50% of the element's height): the tutorial
                                    // window (250..930, y -206..194) grazes the comm box by 6 px, the
                                    // shortcut menu's left slot (y -221..-153) by 21 of 68 px.
                                    const float yov = std::min(range_yhi, bx.yhi) - std::max(range_ylo, bx.ylo);
                                    if (!point && yov < 0.5f * (range_yhi - range_ylo)) continue;
                                    const float ov = std::min(range_hi, bx.hi) - std::max(range_lo, bx.lo);
                                    if (point ? (range_lo >= bx.lo && range_lo <= bx.hi) : ov >= 0.3f * w) {
                                        side = 0; in_box = true;
                                    }
                                }
                            }
                        }
                        // Corner panels (HUD layer, z >= 0): small pieces inside one whose own
                        // extent sits just inside the centre band (the "1" badge on the health
                        // bar) follow the panel.
                        if (edges && have_range && !over_map && !in_box && !screen_quad && range_zlo >= 0.0f) {
                            const float w = range_hi - range_lo, h = range_yhi - range_ylo;
                            const float cx2 = 0.5f * (range_lo + range_hi), cy2 = 0.5f * (range_ylo + range_yhi);
                            // < 200: pilot icons (162x162) in the Customize screen's Pilot panel.
                            if (side == 0 && w < 200.0f && h < 200.0f) {
                                for (const auto* list : {&panels_cur, &panels_prev})
                                    for (const auto& pn : *list)
                                        // Contents sit just above their container (0..20 in z): the pause
                                        // dialog's "NO" (z 105) is not part of a hidden slot at z 0.
                                        if (cx2 >= pn.lo && cx2 <= pn.hi && cy2 >= pn.ylo && cy2 <= pn.yhi &&
                                            range_zlo >= pn.z - 0.5f && range_zlo <= pn.z + 20.0f)
                                            side = pn.side, followed = true;
                            } else if (side != 0 && !point && w >= 300.0f && w <= 800.0f && h >= 80.0f &&
                                       h <= 500.0f && range_zlo < 80.0f && panels_cur.size() < 16) {
                                // h <= 500: weapon shortcut menu frame (250..930 x -226..214) holds
                                // its left column (centres < 400); battle tutorial window likewise.
                                panels_cur.push_back({range_lo, range_hi, range_ylo, range_yhi, side, true, range_zlo});
                            }
                        }
                        // Grids split by the centre band (weapon shortcut menu, 3x3 slots; left
                        // column centres < 400): a centred element (>= 100x50) with the same size
                        // and row as an element that moved is its sibling and moves too; a smaller
                        // piece centred inside an element that moved (>= 100x50) goes with it.
                        if (edges && have_range && !over_map && !in_box && !screen_quad && !point &&
                            range_zlo >= 0.0f) {
                            const float w = range_hi - range_lo, h = range_yhi - range_ylo;
                            const float cx2 = 0.5f * (range_lo + range_hi), cy2 = 0.5f * (range_ylo + range_yhi);
                            if (side == 0 && w < 400.0f && h < 400.0f) {
                                for (const auto* list : {&movers_cur, &movers_prev}) {
                                    for (const auto& mv : *list) {
                                        const float mw = mv.hi - mv.lo, mh = mv.yhi - mv.ylo;
                                        if (range_zlo < mv.z - 0.5f || range_zlo > mv.z + 20.0f) continue; // see above
                                        const bool sibling = mv.base && w >= 100.0f && h >= 50.0f && std::abs(mw - w) <= 2.0f &&
                                                             std::abs(mh - h) <= 2.0f && std::abs(mv.ylo - range_ylo) <= 2.0f;
                                        const bool inside = mw >= 100.0f && mh >= 50.0f && w < mw && h < mh &&
                                                            cx2 >= mv.lo && cx2 <= mv.hi && cy2 >= mv.ylo && cy2 <= mv.yhi;
                                        if (sibling || inside) side = mv.side, followed = true;
                                    }
                                }
                            }
                            // Not z >= 80: a 694x422 overlay (237..931 x -512..-90, z 90/93) is drawn every
                            // battle frame and pulled the "MISSION START" letters (z 95) to the right.
                            // Followers are recorded but never seed siblings (no chains across a text row).
                            if (side != 0 && w >= 20.0f && h >= 20.0f && w < 800.0f && h < 500.0f &&
                                range_zlo < 80.0f && movers_cur.size() < 256)
                                movers_cur.push_back({range_lo, range_hi, range_ylo, range_yhi, side, !followed, range_zlo});
                        }
                        // Runs of point draws (dotted lines, tile effects) move as one: the first
                        // decides. A long run not tied to a box is an effect: stretch it.
                        static u64 run_key = 0; static s32 run_side = 0; static bool run_fx = false, run_point = false;
                        bool effect = false;
                        if (point && run_point && fx_key == run_key) {
                            side = run_side; effect = run_fx;
                        } else if (point) {
                            const auto it = fx_prev.find(fx_key);
                            effect = edges && !in_box && !behind_mask && it != fx_prev.end() && it->second > 12;
                            run_side = side; run_fx = effect;
                        }
                        run_key = fx_key; run_point = point;
                        if (point) ++fx_cur[fx_key];
                        // Background layer wider than the canvas outside battle (load screen diamond
                        // lattice: hundreds of tiles/strips/points, same pipeline, z 26, many past
                        // x ±1000): once any piece of the layer reaches past the canvas, the whole
                        // layer (same vs/ps/z, this or previous frame) is stretched like an effect,
                        // so all pieces stay consistent (per-piece sides made a "tetris" pattern).
                        if (edges && have_range && !mask_side && !screen_quad && range_zlo >= 0.0f) {
                            const u64 fam_key = ((pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash * 31) ^
                                                 pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash) * 31 ^
                                                u64(s64(range_zlo * 16.0f));
                            // Far past the canvas only: the Customize screen's bottom bar (-971..832 x
                            // -590..-390, z 0) pokes out by tens of px and must not stretch its layer
                            // (the whole UI at z 0); the lattice reaches x 1360..2009 / y -1191.
                            // ...and it must overlap the canvas: hidden text is parked at y 10000-20000
                            // and would stretch every string at its depth (Customize LV labels).
                            const bool on_canvas = range_lo < 960.0f && range_hi > -960.0f &&
                                                   range_ylo < 540.0f && range_yhi > -540.0f;
                            if (!point && on_canvas && (range_lo < -1200.0f || range_hi > 1200.0f ||
                                                        range_ylo < -700.0f || range_yhi > 700.0f))
                                ++fam_cur[fam_key];
                            if (fam_cur.count(fam_key) || fam_prev.count(fam_key)) effect = true;
                        }
                        if (!point && side == 0 && have_range && !screen_quad && boxes_cur.size() < 16) {
                            const float w = range_hi - range_lo, h = range_yhi - range_ylo;
                            // Menus (no minimap mask): the large centred backing panel of a composed
                            // layout (MS/Pilot sortie menu: -852..669 x -547..113) also keeps its
                            // contents centred. Not in battle: a 1398x700 overlay drawn every battle
                            // frame would capture the ammo gauge.
                            // z >= 0: the world-map background art (z -25, ~1500 wide) is not a
                            // container (it captured the left nav menu's selector bars).
                            const bool menu_box = !mask_side && w <= 1600.0f && h <= 700.0f && range_zlo >= 0.0f;
                            if (w >= 1000.0f && h >= 100.0f && ((w <= 1500.0f && h <= 500.0f) || menu_box))
                                boxes_cur.push_back({range_lo, range_hi, range_ylo, range_yhi});
                            // A backing taller than any dialogue/comm box (> 500) marks a composed
                            // menu screen: the whole layout stays as in 16:9 (see below).
                            if (menu_box && w >= 1000.0f && h > 500.0f) menu_layout_cur = true;
                        }
                        // Ally tags over 3D positions (battle HUD present, negative depth from the
                        // scene, small): keep them where the game put them (see below). Pieces drawn
                        // consecutively at about the same depth are one tag.
                        static bool tag_run = false; static float tag_z = 0.0f, tag_ref = 0.0f, tag_ref_y = 0.0f;
                        bool world_tag = false;
                        // A tag already under way keeps its consecutive pieces at about the same
                        // depth even when one passes the minimap circle or a centred box (the
                        // triple chevron is 3 sprites x shadow/fill = 6 draws).
                        const bool tag_piece = edges && mask_side && have_range && !screen_quad &&
                                               range_zhi < -1.5f && range_hi - range_lo < 400.0f;
                        // Not excluded by centred boxes: at 1080 box content and tags both stay put,
                        // but with a high-res display box content scales about the display centre
                        // and head tags passing the top tutorial band (z -38..-40) jumped away.
                        float pivot_x = 0.0f, pivot_y = 0.0f;
                        if (tag_piece && (!over_map ||
                                          (tag_run && std::abs(range_zlo - tag_z) <= 1.5f))) {
                            // One pivot per unit's tag cluster: an enemy tag is drawn in depth layers
                            // (frame/portraits z -40, purple bars -38, lock icons -36) and its health
                            // bar and pilot icon come later, after other units' tags. Every piece
                            // within 250 x 200 px of a pivot already used this frame shares it
                            // (nearest wins); otherwise its own anchor starts a new one. Draw order
                            // no longer decides the grouping, so the cluster cannot flip per frame.
                            // The unit's own point wins when known: the zero-size root sprite the
                            // tag draws there (nearest of this and last frame's - it can come after
                            // the pieces). Otherwise an objective marker drawn first ("Follow
                            // Helena!" chevrons, 125 px above the head) became the pivot and the
                            // tag scaled about it dropped onto the unit's body.
                            if (range_hi - range_lo < 0.5f && range_yhi - range_ylo < 0.5f &&
                                tag_roots_cur.size() < 64)
                                tag_roots_cur.emplace_back(anchor_x, anchor_y);
                            const std::pair<float, float>* root = nullptr;
                            float root_d = 0.0f;
                            for (const auto* roots : {&tag_roots_cur, &tag_roots_prev}) {
                                for (const auto& r : *roots) {
                                    const float dx = std::abs(anchor_x - r.first);
                                    const float dy = std::abs(anchor_y - r.second);
                                    if (dx > 250.0f || dy > 200.0f) continue;
                                    if (!root || dx + dy < root_d) root = &r, root_d = dx + dy;
                                }
                            }
                            int best = -1;
                            float best_d = 0.0f;
                            for (int i = 0; !root && i < int(tag_pivots.size()); ++i) {
                                const float dx = std::abs(anchor_x - tag_pivots[i].first);
                                const float dy = std::abs(anchor_y - tag_pivots[i].second);
                                if (dx > 250.0f || dy > 200.0f) continue;
                                if (best < 0 || dx + dy < best_d) best = i, best_d = dx + dy;
                            }
                            if (!root && best < 0 && tag_pivots.size() < 64) {
                                tag_pivots.emplace_back(anchor_x, anchor_y);
                                best = int(tag_pivots.size()) - 1;
                            }
                            if (root) tag_ref = root->first, tag_ref_y = root->second;
                            else if (best >= 0) tag_ref = tag_pivots[best].first, tag_ref_y = tag_pivots[best].second;
                            else tag_ref = anchor_x, tag_ref_y = anchor_y;
                            tag_run = true; tag_z = range_zlo; world_tag = true;
                            pivot_x = tag_ref, pivot_y = tag_ref_y;
                        } else {
                            tag_run = false;
                        }
                        // Projected panels on the HUD layer: Chapter 2's "REPAIR GAUGE" box over a
                        // crouched MS follows a 3D point like a head tag but draws at z 0 (not the
                        // -30..-40 tag layer), so edge anchoring pushed it to either screen edge.
                        // Its frame is a 128x40 texture drawn 128x40; the frame and the pieces drawn
                        // inside it afterwards (the percentage digits, z ~4) stay where the game put
                        // them and scale about the frame's anchor.
                        if (!world_tag && edges && mask_side && have_range && !screen_quad) {
                            const float w = range_hi - range_lo, h = range_yhi - range_ylo;
                            bool proj_frame = false;
                            if (range_zlo > -1.5f && range_zlo < 1.0f && std::abs(w - 128.0f) < 2.0f &&
                                std::abs(h - 40.0f) < 2.0f) {
                                for (const auto* stage : pipeline->GetStages()) {
                                    if (!stage) continue;
                                    for (const auto& im : stage->images) {
                                        const auto sh = im.GetSharp(*stage);
                                        proj_frame |= sh.width + 1 == 128 && sh.height + 1 == 40;
                                    }
                                }
                            }
                            if (proj_frame) {
                                if (proj_panels.size() < 16)
                                    proj_panels.push_back({range_lo, range_hi, range_ylo, range_yhi, range_zlo, anchor_x, anchor_y});
                                world_tag = true; pivot_x = anchor_x; pivot_y = anchor_y;
                            } else {
                                const float pcx = 0.5f * (range_lo + range_hi), pcy = 0.5f * (range_ylo + range_yhi);
                                for (const auto& pp : proj_panels) {
                                    if (pcx >= pp.lo && pcx <= pp.hi && pcy >= pp.ylo && pcy <= pp.yhi &&
                                        range_zlo >= pp.z - 0.5f && range_zlo <= pp.z + 10.0f) {
                                        world_tag = true; pivot_x = pp.ax; pivot_y = pp.ay;
                                        break;
                                    }
                                }
                            }
                        }
                        // Composed menu screen (sortie MS/Pilot menu): keep the 16:9 layout, nav
                        // menu and Rank included (user preference: nav close to the boxes).
                        if (!mask_side && (menu_layout_cur || menu_layout_prev || battle_only) && !effect)
                            side = 0;
                        const s32 offset = side * margin;
                        g_uw_canvas_x0 = margin + offset;
                        g_uw_canvas_x1 = g_uw_canvas_x0 + 1920;
                        g_uw_shift = (screen_quad ? margin : 0) + offset;
                        if (effect) {
                            g_uw_canvas_x0 = g_uw_canvas_x1 = g_uw_shift = 0;
                            g_uw_stretch = float(virtual_w) / 1920.0f;
                        }
                        if (world_tag) {
                            g_uw_stretch = 1.0f; // a tag piece is never an effect
                            // Projected with the original 16:9 maths at the same vertical FOV, the
                            // centre-relative canvas position already equals the display position:
                            // no side shift, no canvas clip.
                            g_uw_canvas_x0 = g_uw_canvas_x1 = g_uw_shift = 0;
                            g_hud_anchor = true; g_hud_ax = pivot_x; g_hud_ay = pivot_y;
                        }
                    } else {
                        hud_draw = false; // full-size pixel quad source (FXAA)
                    }
                    if (hud_draw && ci.size.height != 1080) {
                        g_hud_scale = hud_scale;
                        g_hud_pixel = screen_quad;
                    }
                }
            }
        }
    }
    if (upscaler->Enabled() && std::popcount(pipeline->GetGraphicsKey().mrt_mask) == 1) {
        const auto& viewport = Regs().viewports[0];
        upscaler->OnDraw(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                         cb_descs[0].first, db_desc.first,
                         UiComposition::NativeViewport(viewport.xscale * 2, viewport.yscale * 2),
                         regs.blend_control[0].enable && !regs.color_buffers[0].info.blend_bypass,
                         pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash);
    }
    const PreparedDraw* draw_prepared = bind_prepared;
    // ps4rt: vertex and index buffers are resolved while the helper binds textures (their
    // commands are recorded after BeginRendering, as before).
    draw_inputs = {pipeline, draw_prepared, index_offset, is_indexed, true, false};
    const bool bound = BindResources(pipeline);
    bind_prepared = nullptr; // indirect draws and dispatches bind without prepared sharps
    const bool inputs_resolved = draw_inputs.resolved;
    draw_inputs.pending = false;
    if (!bound) {
        return;
    }
    if constexpr (Ps4RtDiag::Built) {
        if (DiagDraw(pipeline)) {
            ResetBindings(false);
            return;
        }
    }
    const auto state = BeginRendering(pipeline);
    

    if (!inputs_resolved) {
        ResolveVertexBuffers(pipeline, draw_prepared);
        if (is_indexed) {
            ResolveIndexBuffer(index_offset);
        }
    }
    EmitVertexBuffers();
    if (is_indexed) {
        EmitIndexBuffer();
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    // ps4rt: screen-space (clip disabled) draws into the upscaler's output-size images.
    push_data.xscale *= target_scale[0];
    push_data.xoffset *= target_scale[0];
    push_data.yscale *= target_scale[1];
    push_data.yoffset *= target_scale[1];
    if (motion_draw && motion_geometry) {
        const auto& vs = pipeline->GetStage(Shader::SwStage::Vertex);
        const auto [base_vertex, first_instance] =
            GetDrawOffsets(regs, vs, pipeline->GetFetchShader());
        const u32 index_size =
            regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2u : 4u;
        const VAddr index_address = is_indexed
            ? regs.index_base_address.Address<VAddr>() + u64(index_offset) * index_size : 0;
        // Without a character-size skeleton the draw is gated on its bone palette changing
        // since last frame (model constants follow the camera, the palette does not). The
        // check comes first: most gated draws are static world pieces with long index lists.
        static const bool all_motion = [] {
            const char* value = std::getenv("PS4RT_OBJECT_MOTION_ALL");
            return value && value[0] == '1';
        }();
        // Track the selected geometry; a new port may adjust motion heuristics.
        bool gated = !all_motion && !Ps4RtSettings::Get().native_scene;
        u64 palette = 0;
        for (const auto& resource : vs.buffers) {
            if (resource.IsSpecial()) continue;
            const auto buffer = resource.GetSharp(vs);
            const u32 size = buffer.GetSize();
            if (!buffer.Valid() || buffer.GetStride() != 16) continue;
            const auto role = Motion::ClassifyBuffer(size);
            if (role == Motion::BufferRole::Skeleton) {
                gated = false;
                break;
            }
            const VAddr address = buffer.base_address;
            if (role == Motion::BufferRole::SmallSkeleton &&
                memory->IsValidGpuMapping(address, 0) &&
                memory->ClampRangeSize(address, size) == size) {
                palette = XXH3_64bits_withSeed(reinterpret_cast<const void*>(address), size,
                                               palette);
            }
        }
        push_data.motion_param = 0;
        if (!gated || object_motion->Moving({.shader = vs.pgm_hash,
                                             .geometry = motion_geometry,
                                             .indices = index_address,
                                             .index_count = regs.num_indices,
                                             .instances = regs.num_instances.NumInstances(),
                                             .first_instance = first_instance},
                                            palette)) {
            Motion::VertexRange range{base_vertex, regs.num_indices};
            u64 topology = 0;
            if (is_indexed) {
                range = {};
                const u64 bytes = u64(regs.num_indices) * index_size;
                if (index_address && memory->IsValidGpuMapping(index_address, 0) &&
                    memory->ClampRangeSize(index_address, bytes) == bytes) {
                    const bool restart = regs.enable_primitive_restart != 0;
                    const u32 count = regs.num_indices;
                    const auto scanned = object_motion->IndexRange(
                        {index_address, count, index_size, s32(base_vertex), restart}, [&] {
                            const auto* data = reinterpret_cast<const void*>(index_address);
                            const auto scan_range =
                                index_size == 2
                                    ? Motion::IndexedRange(
                                          std::span(static_cast<const u16*>(data), count),
                                          s32(base_vertex), restart)
                                    : Motion::IndexedRange(
                                          std::span(static_cast<const u32*>(data), count),
                                          s32(base_vertex), restart);
                            return Motion::IndexRangeCache::Result{scan_range,
                                                                   XXH3_64bits(data, bytes)};
                        });
                    range = scanned.range;
                    topology = scanned.topology;
                }
            }
            // ES+GS: one slot per emitted vertex, primitive id x max output vertices + emit
            // index (spirv emit_spirv_special.cpp). Instanced draws reuse primitive ids.
            const bool geometry_motion = pipeline->GetStages()[u32(Shader::SwStage::Geometry)] &&
                regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::EsGs;
            u64 shader_key = vs.pgm_hash;
            bool skip = false;
            if (geometry_motion) {
                const u32 n = regs.num_indices;
                u32 primitives = 0;
                switch (regs.primitive_type) {
                case AmdGpu::PrimitiveType::TriangleList: primitives = n / 3; break;
                case AmdGpu::PrimitiveType::TriangleStrip:
                case AmdGpu::PrimitiveType::TriangleFan: primitives = n > 2 ? n - 2 : 0; break;
                case AmdGpu::PrimitiveType::LineList: primitives = n / 2; break;
                case AmdGpu::PrimitiveType::LineStrip: primitives = n > 1 ? n - 1 : 0; break;
                case AmdGpu::PrimitiveType::PointList: primitives = n; break;
                default: break;
                }
                const u64 slots = u64(primitives) * regs.vgt_gs_max_vert_out;
                skip = !slots || slots > (1u << 22) || regs.num_instances.NumInstances() != 1;
                range = {0, u32(slots)};
                shader_key ^= pipeline->GetStage(Shader::SwStage::Geometry).pgm_hash * 0x9e3779b97f4a7c15ull;
            }
            if (!skip)
            push_data.motion_param = object_motion->PrepareDraw({
                .shader = shader_key,
                .geometry = motion_geometry,
                .indices = index_address,
                .topology = topology,
                .index_count = regs.num_indices,
                .instances = geometry_motion ? 1u : regs.num_instances.NumInstances(),
                .first_instance = geometry_motion ? 0u : first_instance,
                .vertices = range,
            });
        }
    }
    pipeline->BindResources(set_writes, push_data);
    // ps4rt: jitter geometry drawn with the scene depth, not full-screen passes (a shifted
    // full-screen quad leaves an edge column unwritten).
    draw_jitter = {};
    // Code Fairy: 3D geometry runs ES+GS; its single-quad draws (decals such as the Zaku
    // emblem) must follow the scene's jitter or they shimmer against the jittered model.
    if (upscaler->Enabled() && db_desc.first && db_desc.first == camera_motion->Depth() &&
        (regs.num_indices > 6 || regs.num_instances.NumInstances() > 1 ||
         (Ps4RtSettings::Get().native_scene &&
          regs.stage_enable.raw == AmdGpu::ShaderStageEnable::VgtStages::EsGs))) {
        draw_jitter = upscaler->Jitter();
    }
    UpdateDynamicState(pipeline, is_indexed);
    diag_hide_color = false;
    scheduler.BeginRendering(state);
    if (g_uw_movie && state.num_color_attachments > 0) {
        // ps4rt (ultrawide): a centred movie leaves the canvas sides showing whatever the
        // game drew behind it (hidden on a 16:9 screen); clear them to black bars.
        g_uw_movie = false;
        const s32 w = state.width, x0 = std::clamp(g_uw_movie_x0, 0, w),
                  x1 = std::clamp(g_uw_movie_x1, x0, w);
        std::array<vk::ClearRect, 2> rects{};
        u32 n = 0;
        if (x0 > 0) rects[n++] = vk::ClearRect{vk::Rect2D{{0, 0}, {u32(x0), state.height}}, 0, 1};
        if (x1 < w) rects[n++] = vk::ClearRect{vk::Rect2D{{x1, 0}, {u32(w - x1), state.height}}, 0, 1};
        if (n) {
            const vk::ClearAttachment black{vk::ImageAspectFlagBits::eColor, 0,
                                            vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 1.0f}}};
            scheduler.Record([=](vk::CommandBuffer cmdbuf) {
                cmdbuf.clearAttachments(black, vk::ArrayProxy<const vk::ClearRect>(n, rects.data()));
            });
        }
    }

    const auto& vs_info = pipeline->GetStage(Shader::SwStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] = GetDrawOffsets(regs, vs_info, fetch_shader);

    const vk::Pipeline handle = pipeline->Handle();
    const vk::Pipeline square = SquarePass(*pipeline);
    const u32 num_indices = regs.num_indices;
    const u32 num_instances = regs.num_instances.NumInstances();
    const u32 first_vertex = vertex_offset;
    const u32 first_instance = instance_offset;
    GpuProfile::Scope profile; // until the closure is recorded
    const u32 profile_before = profile.Slot(GpuProfile::Other, ProfileHash(*pipeline));
    const u32 profile_after = profile.Slot(GpuProfile::Draw, ProfileHash(*pipeline));
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        GpuProfile::Write(cmdbuf, profile_before);
        const auto draw = [&] {
            if (is_indexed) {
                cmdbuf.drawIndexed(num_indices, num_instances, 0, s32(first_vertex),
                                   first_instance);
            } else {
                cmdbuf.draw(num_indices, num_instances, first_vertex, first_instance);
            }
        };
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, handle);
        draw();
        if (square) [[unlikely]] {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, square);
            draw();
        }
        GpuProfile::Write(cmdbuf, profile_after);
    });
    
    DebugState.IncDrawCall();

    ResetBindings(false);
    scheduler.KickRecording();
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address, u16 vertex_sgpr_offset,
                              u16 instance_sgpr_offset) {
    RENDERER_TRACE;
    // ps4rt: like direct draws, handed to the recording thread after the pipeline selection
    // (else the GPU thread waited here for a whole frame of queued draws).
    const bool pipelined = UseDrawPipe() && FilterDrawPasses() &&
                           !Ps4RtToggle::Disabled(Ps4RtToggle::PipelinedIndirectDraws);
    if (!pipelined) {
        DrainDrawPipe();
        scheduler.PopPendingOperations();
        if (!FilterDraw()) {
            return;
        }
    }

    const DrawIndirectParams params = {
        .vertex_sgpr_offset = vertex_sgpr_offset,
        .instance_sgpr_offset = instance_sgpr_offset,
    };
    const GraphicsPipeline* pipeline = pipeline_cache.GetGraphicsPipeline(params);
    if (!pipeline) {
        return;
    }
    const IndirectDraw indirect = {arg_address + offset, count_address, stride, max_count};
    if (pipelined) {
        PostDraw(pipeline, nullptr, is_indexed, 0, nullptr, &indirect);
        return;
    }
    DrawIndirectRecord(pipeline, is_indexed, indirect);
}

void Rasterizer::DrawIndirectRecord(const GraphicsPipeline* pipeline, bool is_indexed,
                                    const IndirectDraw& indirect) {
    if (DrawPipe::OnStageB()) {
        ;
        scheduler.PopPendingOperations();
    }
    const VAddr count_address = indirect.count;
    const u32 stride = indirect.stride;
    const u32 max_count = indirect.max_count;

    // Indirect arguments may be GPU-written: leave these draws on camera fallback.
    motion_draw = false;
    motion_geometry = 0;
    PrepareRenderState(pipeline);
    ;
    if (upscaler->Enabled() && std::popcount(pipeline->GetGraphicsKey().mrt_mask) == 1) {
        const auto& regs = Regs();
        const auto& viewport = regs.viewports[0];
        upscaler->OnDraw(pipeline->GetStage(Shader::SwStage::Vertex).pgm_hash,
                         cb_descs[0].first, db_desc.first,
                         UiComposition::NativeViewport(viewport.xscale * 2, viewport.yscale * 2),
                         regs.blend_control[0].enable && !regs.color_buffers[0].info.blend_bypass,
                         pipeline->GetStage(Shader::SwStage::Fragment).pgm_hash);
    }
    if (!BindResources(pipeline)) {
        return;
    }
    if constexpr (Ps4RtDiag::Built) {
        if (DiagDraw(pipeline)) {
            ResetBindings(false);
            return;
        }
    }
    const auto state = BeginRendering(pipeline);

    BindVertexBuffers(pipeline);
    if (is_indexed) {
        BindIndexBuffer();
    }

    const auto [buffer, base] =
        buffer_cache.ObtainBuffer(indirect.args, stride * max_count, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, stride * max_count);

    const VideoCore::Buffer* count_buffer;
    u64 count_offset;
    if (count_address != 0) {
        std::tie(count_buffer, count_offset) = buffer_cache.ObtainBuffer(count_address, 4, false);
        needs_barrier |= runtime.IsBufferAccessed(count_buffer, count_offset, 4);
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }

    // ps4rt: screen-space (clip disabled) draws into the upscaler's output-size images.
    push_data.xscale *= target_scale[0];
    push_data.xoffset *= target_scale[0];
    push_data.yscale *= target_scale[1];
    push_data.yoffset *= target_scale[1];
    pipeline->BindResources(set_writes, push_data);
    draw_jitter = {};
    if (upscaler->Enabled() && db_desc.first && db_desc.first == camera_motion->Depth()) {
        draw_jitter = upscaler->Jitter();
    }
    UpdateDynamicState(pipeline, is_indexed);
    diag_hide_color = false;
    scheduler.BeginRendering(state);

    ASSERT(stride == (is_indexed ? sizeof(VkDrawIndexedIndirectCommand)
                                 : sizeof(VkDrawIndirectCommand)));
    const vk::Pipeline handle = pipeline->Handle();
    const vk::Pipeline square = SquarePass(*pipeline);
    const vk::Buffer args = buffer->Handle();
    const u64 args_offset = base;
    const vk::Buffer counts = count_address != 0 ? count_buffer->Handle() : vk::Buffer{};
    const u64 counts_offset = count_address != 0 ? count_offset : 0;
    GpuProfile::Scope profile; // until the closure is recorded
    const u32 profile_before = profile.Slot(GpuProfile::Other, ProfileHash(*pipeline));
    const u32 profile_after = profile.Slot(GpuProfile::Draw, ProfileHash(*pipeline));
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        GpuProfile::Write(cmdbuf, profile_before);
        for (const vk::Pipeline bound : {handle, square}) {
            if (!bound) {
                break;
            }
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, bound);
            if (is_indexed) {
                if (counts) {
                    cmdbuf.drawIndexedIndirectCount(args, args_offset, counts, counts_offset, max_count,
                                                    stride);
                } else {
                    cmdbuf.drawIndexedIndirect(args, args_offset, max_count, stride);
                }
            } else if (counts) {
                cmdbuf.drawIndirectCount(args, args_offset, counts, counts_offset, max_count, stride);
            } else {
                cmdbuf.drawIndirect(args, args_offset, max_count, stride);
            }
        }
        GpuProfile::Write(cmdbuf, profile_after);
    });
    DebugState.IncDrawCall();

    ResetBindings(false);
    scheduler.KickRecording();
}


void Rasterizer::DispatchDirect() {
    RENDERER_TRACE;
    ;
    // ps4rt: like draws, handed to the draw recording thread after the pipeline selection.
    const bool pipelined = UseDrawPipe() && !Ps4RtToggle::Disabled(Ps4RtToggle::PipelinedDispatch);
    if (!pipelined) {
        DrainDrawPipe();
    }
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }
    if (pipelined) {
        PostDraw(pipeline, nullptr, false, 0, &CsRegs());
        return;
    }
    DispatchRecord(pipeline);
}

void Rasterizer::DispatchRecord(const ComputePipeline* pipeline) {
    ;
    gbuffer_draw = false;

    scheduler.PopPendingOperations();
    ;

    const auto& cs_program = CsRegs();

    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    // After the resource binding: its transfers (image uploads, downloads) mark themselves.
    const auto mark = [&] {
        
    };
    if (upscaler->Enabled()) {
        upscaler->OnDispatch(cs.pgm_hash);
    }
    if (ExecuteShaderHLE(cs, Regs(), cs_program, *this)) {
        return;
    }

    if (!BindResources(pipeline)) {
        return;
    }

    if (needs_barrier) {
        runtime.FlushBarriers();
    }
    if constexpr (Ps4RtDiag::Built) {
        DrawList(nullptr, &cs);
        if (DiagHideDispatch(cs.pgm_hash)) {
            ResetBindings(true);
            return;
        }
    }

    scheduler.EndRendering();
    mark();
    pipeline->BindResources(set_writes, push_data);

    const vk::Pipeline handle = pipeline->Handle();
    const u32 dim_x = cs_program.dim_x, dim_y = cs_program.dim_y, dim_z = cs_program.dim_z;
    GpuProfile::Scope profile; // until the closure is recorded
    const u32 profile_before = profile.Slot(GpuProfile::Other, cs.pgm_hash);
    const u32 profile_after = profile.Slot(GpuProfile::Dispatch, cs.pgm_hash);
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        GpuProfile::Write(cmdbuf, profile_before);
        if (Scheduler::FullBarriers()) {
            Scheduler::RecordFullBarrier(cmdbuf);
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, handle);
        cmdbuf.dispatch(dim_x, dim_y, dim_z);
        if (Scheduler::FullBarriers()) {
            Scheduler::RecordFullBarrier(cmdbuf);
        }
        GpuProfile::Write(cmdbuf, profile_after);
    });
    DebugState.IncDispatch();

    

    ResetBindings(true);
    scheduler.KickRecording();
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    RENDERER_TRACE;
    // ps4rt: handed to the recording thread like direct dispatches.
    const bool pipelined = UseDrawPipe() && !Ps4RtToggle::Disabled(Ps4RtToggle::PipelinedIndirectDraws);
    if (!pipelined) {
        DrainDrawPipe();
    }
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }
    if (pipelined) {
        const IndirectDraw indirect = {address + offset, 0, size, 1};
        PostDraw(pipeline, nullptr, false, 0, &CsRegs(), &indirect);
        return;
    }
    DispatchIndirectRecord(pipeline, address + offset, size);
}

void Rasterizer::DispatchIndirectRecord(const ComputePipeline* pipeline, VAddr address,
                                        u32 size) {
    scheduler.PopPendingOperations();
    const u32 offset = 0;

    if (!BindResources(pipeline)) {
        return;
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, base, size);

    if (needs_barrier) {
        runtime.FlushBarriers();
    }
    const auto& cs = pipeline->GetStage(Shader::SwStage::Compute);
    if constexpr (Ps4RtDiag::Built) {
        DrawList(nullptr, &cs);
        if (DiagHideDispatch(cs.pgm_hash)) {
            ResetBindings(true);
            return;
        }
    }

    scheduler.EndRendering();
    pipeline->BindResources(set_writes, push_data);

    const vk::Pipeline handle = pipeline->Handle();
    const vk::Buffer args = buffer->Handle();
    const u64 args_offset = base;
    GpuProfile::Scope profile; // until the closure is recorded
    const u32 profile_before = profile.Slot(GpuProfile::Other, cs.pgm_hash);
    const u32 profile_after = profile.Slot(GpuProfile::Dispatch, cs.pgm_hash);
    scheduler.Record([=](vk::CommandBuffer cmdbuf) {
        GpuProfile::Write(cmdbuf, profile_before);
        if (Scheduler::FullBarriers()) {
            Scheduler::RecordFullBarrier(cmdbuf);
        }
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, handle);
        cmdbuf.dispatchIndirect(args, args_offset);
        if (Scheduler::FullBarriers()) {
            Scheduler::RecordFullBarrier(cmdbuf);
        }
        GpuProfile::Write(cmdbuf, profile_after);
    });
    DebugState.IncDispatch();

    ResetBindings(true);
    scheduler.KickRecording();
}

u64 Rasterizer::Flush() {
    DrainDrawPipe();
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::Finish() {
    DrainDrawPipe();
    scheduler.Finish();
}

void Rasterizer::OnSubmit() {
    DrainDrawPipe();
    if (fault_process_pending) {
        fault_process_pending = false;
        buffer_cache.ProcessFaultBuffer();
    }
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    runtime.TickFrame();
}

const PreparedStage* Rasterizer::FindPreparedStage(const Shader::Info& stage) const {
    // Sharps a draw-preparation worker read from the same flattened user data.
    if (!bind_prepared || Ps4RtToggle::Disabled(Ps4RtToggle::PreparedResources)) {
        return nullptr;
    }
    for (u32 i = 0; i < bind_prepared->num_stages; ++i) {
        const auto& candidate = bind_prepared->stages[i];
        if (&candidate.program->info == &stage &&
            candidate.num_images == stage.images.size() &&
            candidate.num_samplers == stage.samplers.size() &&
            candidate.num_buffers == stage.buffers.size()) {
            return &candidate;
        }
    }
    return nullptr;
}

bool Rasterizer::BindHelperWanted() {
    // A spinning helper pays off only with cores to spare (the recording thread spins too).
    const char* env = std::getenv("PS4RT_TEXTURE_HELPER");
    if (env && env[0]) {
        return env[0] == '1';
    }
    // Measured neutral on 16 threads (docs/parallel_gpu.md): opt-in.
    return false;
}

bool Rasterizer::HelperEligible(const Pipeline* pipeline) const {
    if (pipeline->IsCompute() || !bind_helper.Available() || false ||
        Ps4RtToggle::Disabled(Ps4RtToggle::TextureBindHelper) ||
        Ps4RtToggle::Disabled(Ps4RtToggle::TextureBindingMemo) ||
        Ps4RtToggle::Disabled(Ps4RtToggle::UpdateImageFastPath)) {
        return false;
    }
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        // Mip arrays make the descriptor layout depend on the T#s.
        for (const auto& image : stage->images) {
            if (image.mip_fallback_mode != Shader::MipStorageFallbackMode::None) {
                return false;
            }
        }
        // Storage writes invalidate cached images from the buffer side.
        for (const auto& buffer : stage->buffers) {
            if (!buffer.IsSpecial() && buffer.is_written) {
                return false;
            }
        }
    }
    return true;
}

bool Rasterizer::TexturesBindableOnHelper(const Shader::Info& stage,
                                          const PreparedStage* prepared) {
    if (VideoCore::Ps4RtTransition::Consumer(stage.pgm_hash)) return false;
    const u64 generation = texture_cache.RegistryGeneration();
    for (u32 image_index = 0; image_index < stage.images.size(); ++image_index) {
        const auto& image_desc = stage.images[image_index];
        if (image_desc.is_written) {
            return false;
        }
        const auto tsharp =
            prepared ? prepared->image_sharps[image_index] : image_desc.GetSharp(stage);
        if (image_desc.CanSkipBinding(tsharp, [&](u32 i) {
                return prepared ? prepared->sampler_sharps[i] : stage.samplers[i].GetSharp(stage);
            })) {
            continue;
        }
        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid ||
            !memory->IsValidGpuMapping(tsharp.Address(), 0) ||
            !IsKnownFormat(data_fmt, num_fmt)) {
            continue; // null descriptor
        }
        const auto& entry =
            prepared ? CachedImageDescEntry(tsharp, image_desc, prepared->image_hashes[image_index])
                     : CachedImageDescEntry(tsharp, image_desc);
        if (entry.found_generation != generation) {
            return false;
        }
        VideoCore::ImageId image_id = entry.found_id;
        auto* image = &texture_cache.GetImage(image_id);
        if (const auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
            image_id = depth_image_id;
            image = &texture_cache.GetImage(image_id);
        }
        if (image->binding.needs_rebind || !texture_cache.IsUpToDate(image_id) ||
            scene_targets->Tracks(image->image_uid) ||
            (upscaler->Enabled() && upscaler->RedirectsSampled(image_id))) {
            return false;
        }
    }
    return true;
}

void Rasterizer::RunTextureTask(void* context) {
    auto& self = *static_cast<Rasterizer*>(context);
    auto& task = self.texture_task;
    for (u32 i = 0; i < task.count; ++i) {
        auto& stage = task.stages[i];
        if (!self.TexturesBindableOnHelper(*stage.info, stage.prepared)) {
            return;
        }
        auto binding = stage.binding;
        u32 write_index = stage.write_index;
        self.BindTextures(*stage.info, stage.prepared, binding, write_index, task.barrier, true);
        task.completed = i + 1;
    }
}

void Rasterizer::JoinBindHelper(void* context) {
    if (!BindHelper::OnHelper()) {
        static_cast<Rasterizer*>(context)->bind_helper.Join();
    }
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    if (IsComputeImageCopy(pipeline) || IsComputeMetaClear(pipeline) ||
        IsComputeImageClear(pipeline)) {
        return false;
    }

    set_write_index = 0;
    set_writes.clear();
    buffer_infos.clear();
    image_infos.clear();
    // ps4rt: G-buffer passes sample material textures for a scene rendered below the output
    // size; the temporal upscaler restores the detail of the output's mip level (FSR guide:
    // log2(render / output)). Shadows, post-processing and UI keep the guest's bias.
    sampler_lod_bias = 0.0f;
    if (!pipeline->IsCompute() && !Ps4RtToggle::Disabled(Ps4RtToggle::SceneMipBias) &&
        std::popcount(static_cast<const GraphicsPipeline*>(pipeline)->GetGraphicsKey().mrt_mask &
                      0xff) >= 5) {
        sampler_lod_bias = upscaler->SceneMipBias();
        static float reported = 0.0f;
        if (sampler_lod_bias != reported) {
            reported = sampler_lod_bias;
            std::printf("Upscaler: scene texture LOD bias %.2f\n", sampler_lod_bias);
        }
    }

    bool uses_dma = false;

    

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    push_data = MakeUserData(Regs());
    if (!HelperEligible(pipeline)) {
        ++helper_serial;
        for (const auto* stage : pipeline->GetStages()) {
            if (!stage) {
                continue;
            }
            set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                              stage->samplers.size());
            stage->PushUd(binding, push_data);
            const PreparedStage* prepared = FindPreparedStage(*stage);
            BindBuffers(*stage, prepared, binding, push_data, set_write_index);
            BindTextures(*stage, prepared, binding, set_write_index, needs_barrier);
            uses_dma |= stage->uses_dma;
        }
    } else {
        // ps4rt: textures on the helper, buffers here. Without mip arrays every stage's
        // descriptors are [buffers][images][samplers], one each, so both sides know their
        // binding numbers and descriptor write slots up front.
        struct BufferStage {
            const Shader::Info* info;
            const PreparedStage* prepared;
            Shader::Backend::Bindings binding;
            u32 write_index;
        };
        std::array<BufferStage, Shader::MaxStageTypes> buffer_stages;
        auto& task = texture_task;
        task.count = 0;
        task.completed = 0;
        task.barrier = false;
        for (const auto* stage : pipeline->GetStages()) {
            if (!stage) {
                continue;
            }
            const u32 num_buffers = static_cast<u32>(stage->buffers.size());
            const u32 num_descriptors =
                num_buffers + static_cast<u32>(stage->images.size() + stage->samplers.size());
            set_writes.resize(set_writes.size() + num_descriptors);
            stage->PushUd(binding, push_data);
            const PreparedStage* prepared = FindPreparedStage(*stage);
            buffer_stages[task.count] = {stage, prepared, binding, set_write_index};
            auto texture_binding = binding;
            texture_binding.buffer += num_buffers;
            texture_binding.unified += num_buffers;
            task.stages[task.count] = {stage, prepared, texture_binding,
                                       set_write_index + num_buffers};
            ++task.count;
            binding.buffer += num_buffers;
            binding.unified += num_descriptors;
            set_write_index += num_descriptors;
            uses_dma |= stage->uses_dma;
        }
        bind_helper.Fork(&RunTextureTask, this);
        for (u32 i = 0; i < task.count; ++i) {
            auto& stage = buffer_stages[i];
            BindBuffers(*stage.info, stage.prepared, stage.binding, push_data, stage.write_index);
        }
        if (draw_inputs.pending && !Ps4RtToggle::Disabled(Ps4RtToggle::EarlyDrawInputs)) {
            ResolveVertexBuffers(draw_inputs.pipeline, draw_inputs.prepared);
            if (draw_inputs.is_indexed) {
                ResolveIndexBuffer(draw_inputs.index_offset);
            }
            draw_inputs.resolved = true;
        }
        bind_helper.Join();
        needs_barrier |= task.barrier;
        ++(task.completed == task.count ? helper_full
                                        : task.completed ? helper_partial : helper_serial);
        // Stages the helper left (an image needing work that records commands).
        for (u32 i = task.completed; i < task.count; ++i) {
            auto& stage = task.stages[i];
            u32 write_index = stage.write_index;
            BindTextures(*stage.info, stage.prepared, stage.binding, write_index, needs_barrier);
        }
    }

    if (uses_dma) {
        buffer_cache.SynchronizeDmaBuffers();
        fault_process_pending = true;
    }
    ClearComputeFill(pipeline);

    return true;
}

// ps4rt: the PS4 SDK's compute fill (cs e88f28ca305476bb: dst[i] = pattern[i & mask] for
// i < count; buffers: constants {count, mask}, pattern, destination). It still runs; when it
// writes one repeated dword over whole images, TextureCache::ClearFilledImages clears them
// instead of re-uploading them. PS4RT_FILL_CLEAR=0 turns this off.
void Rasterizer::ClearComputeFill(const Pipeline* pipeline) {
    static const bool enabled = [] {
        const char* value = std::getenv("PS4RT_FILL_CLEAR");
        return !(value && value[0] == '0');
    }();
    if (!enabled || !pipeline->IsCompute()) {
        return;
    }
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    const auto& cs_pgm = CsRegs();
    if (info.pgm_hash != 0xe88f28ca305476bbULL || info.buffers.size() != 3 ||
        !info.images.empty() || cs_pgm.num_thread_x.full != 64 || cs_pgm.dim_y != 1 ||
        cs_pgm.dim_z != 1 || info.buffers[0].is_written || info.buffers[1].is_written ||
        !info.buffers[2].is_written || info.buffers[0].IsSpecial() ||
        info.buffers[1].IsSpecial() || info.buffers[2].IsSpecial()) {
        return;
    }
    const AmdGpu::Buffer constants = info.buffers[0].GetSharp(info);
    const AmdGpu::Buffer pattern = info.buffers[1].GetSharp(info);
    const AmdGpu::Buffer dst = info.buffers[2].GetSharp(info);
    // The constants and the pattern are read from guest memory: they must be CPU written.
    if (!constants.base_address || !pattern.base_address || !dst.base_address ||
        constants.GetSize() < 8 || !memory->IsValidGpuMapping(constants.base_address, 8) ||
        buffer_cache.IsRegionGpuModified(constants.base_address, 8)) {
        return;
    }
    const u32* params = std::bit_cast<const u32*>(constants.base_address);
    const u32 count = params[0], mask = params[1];
    const u64 pattern_bytes = (u64{mask} + 1) * 4;
    if (mask >= 64 || (mask & (mask + 1)) != 0 || pattern.GetSize() < pattern_bytes ||
        !memory->IsValidGpuMapping(pattern.base_address, pattern_bytes) ||
        buffer_cache.IsRegionGpuModified(pattern.base_address, pattern_bytes)) {
        return;
    }
    const u32* words = std::bit_cast<const u32*>(pattern.base_address);
    for (u32 i = 1; i <= mask; ++i) {
        if (words[i] != words[0]) {
            return;
        }
    }
    const u64 threads = u64{cs_pgm.dim_x} * 64;
    const u64 written = std::min<u64>({u64{count}, threads, dst.GetSize() / 4}) * 4;
    texture_cache.ClearFilledImages(dst.base_address, written, words[0]);
}

void Rasterizer::BindVertexBuffers(const GraphicsPipeline* pipeline, const PreparedDraw* prepared) {
    ResolveVertexBuffers(pipeline, prepared);
    EmitVertexBuffers();
}

void Rasterizer::EmitVertexBuffers() {
    // ps4rt: only the used entries go into the recording chunk (the static vectors hold 32 of
    // each: ~3 KiB copied per draw).
    auto& v = vertex_binds;
    const bool dynamic_input = instance.IsVertexInputDynamicState();
    const u32 num_buffers = v.num_buffers;
    scheduler.ReserveRecordData(
        v.bindings.size() * sizeof(v.bindings[0]) + v.attributes.size() * sizeof(v.attributes[0]) +
        num_buffers * (sizeof(vk::Buffer) + 3 * sizeof(vk::DeviceSize)) + 128);
    if (dynamic_input) {
        scheduler.Record([bindings = scheduler.RecordData(std::span<const vk::VertexInputBindingDescription2EXT>{v.bindings.data(), v.bindings.size()}),
                          attributes = scheduler.RecordData(std::span<const vk::VertexInputAttributeDescription2EXT>{
                              v.attributes.data(), v.attributes.size()})](
                             vk::CommandBuffer cmdbuf) {
            cmdbuf.setVertexInputEXT(bindings, attributes);
        });
    }
    if (num_buffers == 0) {
        return;
    }
    const auto buffers = scheduler.RecordData(std::span<const vk::Buffer>{v.host_buffers.data(), num_buffers});
    const auto offsets = scheduler.RecordData(std::span<const vk::DeviceSize>{v.host_offsets.data(), num_buffers});
    if (dynamic_input) {
        scheduler.Record([num_buffers, buffers, offsets](vk::CommandBuffer cmdbuf) {
            cmdbuf.bindVertexBuffers(0, num_buffers, buffers.data(), offsets.data());
        });
        return;
    }
    const auto sizes = scheduler.RecordData(std::span<const vk::DeviceSize>{v.host_sizes.data(), num_buffers});
    const auto strides = scheduler.RecordData(std::span<const vk::DeviceSize>{v.host_strides.data(), num_buffers});
    scheduler.Record([num_buffers, buffers, offsets, sizes, strides](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindVertexBuffers2(0, num_buffers, buffers.data(), offsets.data(), sizes.data(),
                                  strides.data());
    });
}

void Rasterizer::ResolveVertexBuffers(const GraphicsPipeline* pipeline,
                                      const PreparedDraw* prepared) {
    const auto& regs = Regs();
    auto& v = vertex_binds;
    v.num_buffers = 0;
    v.host_buffers.clear();
    v.host_offsets.clear();
    v.host_sizes.clear();
    v.host_strides.clear();
    auto& attributes = v.attributes;
    auto& bindings = v.bindings;
    attributes.clear();
    bindings.clear();
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    VertexInputs<AmdGpu::Buffer> guest_buffers;
    // Inputs a draw-preparation worker read from the same registers and (verified) user data.
    const auto& fetch = pipeline->GetFetchShader();
    const PreparedVertexInputs* ready =
        prepared && prepared->vertex.valid && instance.IsVertexInputDynamicState() && fetch &&
                fetch->attributes.size() == prepared->vertex.count && !false &&
                !Ps4RtToggle::Disabled(Ps4RtToggle::PreparedResources)
            ? &prepared->vertex
            : nullptr;
    if (ready) {
        attributes.assign(ready->attributes, ready->attributes + ready->count);
        bindings.assign(ready->bindings, ready->bindings + ready->count);
        guest_buffers.assign(ready->buffers, ready->buffers + ready->count);
    } else {
        pipeline->GetVertexInputs(attributes, bindings, divisors, guest_buffers,
                                  regs.vgt_instance_step_rate_0, regs.vgt_instance_step_rate_1);
    }

    

    if (bindings.empty()) {
        // If there are no bindings, there is nothing further to do.
        return;
    }
    if (motion_draw && !guest_buffers.empty()) {
        // Include every stream: identical position buffers can be paired with different
        // skinning/instance data. Index topology and base offsets are matched separately.
        motion_geometry = ready ? ready->buffers_hash
                                : XXH3_64bits(guest_buffers.data(),
                                              guest_buffers.size() * sizeof(AmdGpu::Buffer));
    }
    // Object motion research: vertex streams of G-buffer draws.
    

    struct BufferRange {
        VAddr base_address;
        VAddr end_address;
        const VideoCore::Buffer* buffer;
        u64 offset;

        [[nodiscard]] size_t GetSize() const {
            return end_address - base_address;
        }
    };

    // Build list of ranges covering the requested buffers
    VertexInputs<BufferRange> ranges{};
    VertexInputs<BufferRange> ranges_merged{};
    if (ready) {
        for (u32 i = 0; i < ready->num_ranges; ++i) {
            ranges_merged.emplace_back(ready->ranges[i].base, ready->ranges[i].end);
        }
    } else {
        for (const auto& buffer : guest_buffers) {
            if (buffer.base_address != 0 && buffer.GetSize() > 0) {
                ranges.emplace_back(buffer.base_address, buffer.base_address + buffer.GetSize());
            }
        }
    }

    // Merge connecting ranges together
    if (!ranges.empty()) {
        std::ranges::sort(ranges, [](const BufferRange& lhv, const BufferRange& rhv) {
            return lhv.base_address < rhv.base_address;
        });
        ranges_merged.emplace_back(ranges[0]);
        for (auto range : ranges) {
            auto& prev_range = ranges_merged.back();
            if (prev_range.end_address < range.base_address) {
                ranges_merged.emplace_back(range);
            } else {
                prev_range.end_address = std::max(prev_range.end_address, range.end_address);
            }
        }
    }

    // Map buffers for merged ranges
    for (auto& range : ranges_merged) {
        const u64 size = memory->ClampRangeSize(range.base_address, range.GetSize());
        std::tie(range.buffer, range.offset) =
            buffer_cache.ObtainBuffer(range.base_address, size, false);
        needs_barrier |= runtime.IsBufferAccessed(range.buffer, range.offset, size);
    }

    // Bind vertex buffers
    auto& host_buffers = v.host_buffers;
    auto& host_offsets = v.host_offsets;
    auto& host_sizes = v.host_sizes;
    auto& host_strides = v.host_strides;
    u32 stream_with_memory = 0;
    for (const auto& buffer : guest_buffers) {
        if (buffer.base_address != 0 && buffer.GetSize() > 0) {
            const auto host_buffer_info =
                ready ? ranges_merged.begin() + ready->range_index[stream_with_memory++]
                      : std::ranges::find_if(ranges_merged, [&](const BufferRange& range) {
                            return buffer.base_address >= range.base_address &&
                                   buffer.base_address < range.end_address;
                        });
            ASSERT(host_buffer_info != ranges_merged.cend());
            host_buffers.emplace_back(host_buffer_info->buffer->Handle());
            host_offsets.push_back(host_buffer_info->offset + buffer.base_address -
                                   host_buffer_info->base_address);
        } else {
            host_buffers.emplace_back(VK_NULL_HANDLE);
            host_offsets.push_back(0);
        }
        host_sizes.push_back(buffer.GetSize());
        host_strides.push_back(buffer.GetStride());
    }

    v.num_buffers = static_cast<u32>(guest_buffers.size());
}

void Rasterizer::BindIndexBuffer(u32 index_offset) {
    ResolveIndexBuffer(index_offset);
    EmitIndexBuffer();
}

void Rasterizer::EmitIndexBuffer() {
    scheduler.Record([handle = index_bind.handle, offset = index_bind.offset,
                      type = index_bind.type](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindIndexBuffer(handle, offset, type);
    });
}

void Rasterizer::ResolveIndexBuffer(u32 index_offset) {
    const auto& regs = Regs();

    // Figure out index type and size.
    const bool is_index16 = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16;
    const vk::IndexType index_type = is_index16 ? vk::IndexType::eUint16 : vk::IndexType::eUint32;
    const u32 index_size = is_index16 ? sizeof(u16) : sizeof(u32);
    const VAddr index_address =
        regs.index_base_address.Address<VAddr>() + index_offset * index_size;

    // Bind index buffer.
    const u32 index_buffer_size = regs.num_indices * index_size;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(index_address, index_buffer_size, false);
    needs_barrier |= runtime.IsBufferAccessed(buffer, offset, index_buffer_size);
    index_bind = {buffer->Handle(), offset, index_type};
}

void Rasterizer::ResetBindings(bool is_compute) {
    for (auto& image_id : bound_images) {
        texture_cache.GetImage(image_id).binding = {};
    }
    for (const auto [buffer, offset, size, is_written] : bound_buffers) {
        const auto dst_stage = is_compute ? vk::PipelineStageFlagBits2::eComputeShader
                                          : vk::PipelineStageFlagBits2::eAllGraphics;
        const auto write_flag =
            is_written ? vk::AccessFlagBits2::eShaderWrite : vk::AccessFlagBits2::eNone;
        runtime.AccessBuffer(buffer, offset, size, dst_stage,
                             vk::AccessFlagBits2::eShaderRead | write_flag);
    }
    bound_images.clear();
    bound_buffers.clear();
    needs_barrier = false;
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);

    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = CsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return false;
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return false;
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return false;
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return false;
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    runtime.CopyColorAndDepth(&src_image, &dst_image);
    return true;
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = CsRegs();
    const auto& info = pipeline->GetStage(Shader::SwStage::Compute);
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return false;
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return false;
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return false;
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return false;
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return false;
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    runtime.ClearImage(&image1, range, clear);
    return true;
}

void Rasterizer::BindBuffers(const Shader::Info& stage, const PreparedStage* prepared,
                             Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data, u32& write_index) {
    const u64 alignment = instance.StorageMinAlignment();
    for (u32 buffer_index = 0; buffer_index < stage.buffers.size(); ++buffer_index) {
        const auto& desc = stage.buffers[buffer_index];
        const RingBinding* ring = num_ring_stages ? FindRingBinding(stage, buffer_index) : nullptr;
        if (ring) {
            // Copied by the GPU command thread into the constant ring (read-only).
            // ps4rt: dump the ring's own copy, not the guest address. A binding whose bytes came
            // from the host keeps `address` as a label only, and it can be zero; IsValidGpuMapping
            // merely range checks against the 40 bit GPU limit, so zero passed through and the
            // dump read address zero (crash on the frame capture key).
            
            if (!desc.IsSpecial()) {
                if (stage.sw_stage == Shader::SwStage::Vertex && db_desc.first)
                    ;
                if (gbuffer_draw && Ps4RtSettings::Get().native_scene && ring->size == 192)
                    camera_motion->OnPs4RtConstants(
                        reinterpret_cast<const float*>(constant_ring->Data(ring->offset)),ring->size);
                if (ring->size == 864 && gbuffer_draw &&
                    memory->IsValidGpuMapping(ring->address, 0)) {
                    camera_motion->OnConstants(reinterpret_cast<const float*>(ring->address));
                }
                push_data.AddOffset(binding.buffer, 0);
            }
            buffer_infos.emplace_back(constant_ring->Handle(), ring->offset, ring->size);
        } else if (desc.IsSpecial()) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
                needs_barrier |= runtime.IsBufferAccessed(gds_buf, 0, gds_buf->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetStreamBuffer();
                const u32 ubo_size = stage.FlatUserData().size() * sizeof(u32);
                
                const u64 offset =
                    vk_buffer.Copy(stage.FlatUserData().data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::ClipPlanes) {
                // Permutations compiled without enabled planes never read the buffer, so the
                // declared binding is satisfied with a null descriptor instead of a copy.
                if (Regs().clipper_control.user_clip_plane_enable == 0) {
                    buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                } else {
                    auto& vk_buffer = buffer_cache.GetStreamBuffer();
                    std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                    for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; ++i) {
                        const auto& plane = Regs().clip_user_data[i];
                        planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
                        planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
                        planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
                        planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
                    }
                    const u32 ubo_size = static_cast<u32>(sizeof(planes));
                    const u64 offset = vk_buffer.Copy(planes.data(), ubo_size, alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                }
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetStreamBuffer();
                const auto& cs_program = CsRegs();
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                std::memset(data, 0, lds_size);
                lds_buffer.Commit();
                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else {
                UNREACHABLE_MSG("Unexpected buffer type {}", u32(desc.buffer_type));
            }
        } else {
            const auto vsharp =
                prepared ? prepared->buffer_sharps[buffer_index] : desc.GetSharp(stage);
            if (gbuffer_draw && Ps4RtSettings::Get().native_scene && vsharp.GetSize() == 192 &&
                vsharp.base_address && memory->IsValidGpuMapping(vsharp.base_address,0) &&
                memory->ClampRangeSize(vsharp.base_address,vsharp.GetSize()) == vsharp.GetSize())
                camera_motion->OnPs4RtConstants(reinterpret_cast<const float*>(vsharp.base_address),vsharp.GetSize());
            
            if (vsharp.GetSize() == 864 && gbuffer_draw &&
                memory->IsValidGpuMapping(vsharp.base_address, 0)) {
                camera_motion->OnConstants(reinterpret_cast<const float*>(vsharp.base_address));
            }
            // Object motion research: the vertex shader buffers of G-buffer draws.
            
            
            if (vsharp.base_address == 0 || vsharp.GetSize() == 0) {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            } else {
                const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
                if (size != vsharp.GetSize()) {
                    LOG_ERROR(Render, "Clamped size from {} to {} for stage {:#x}",
                              vsharp.GetSize(), size, stage.pgm_hash);
                }
                const auto [buffer, offset] = buffer_cache.ObtainBuffer(
                    vsharp.base_address, size, desc.is_written, desc.is_formatted);
                const u64 offset_aligned = Common::AlignDown(offset, alignment);
                const u64 adjust = offset - offset_aligned;
                if (adjust % 4 != 0) {
                    LOG_WARNING(Render_Vulkan, "Buffer binding in shader {:#x} isn't dword aligned",
                                stage.pgm_hash);
                }
                push_data.AddOffset(binding.buffer, adjust);
                buffer_infos.emplace_back(buffer->Handle(), offset_aligned, size + adjust);
                bound_buffers.emplace_back(buffer, offset, size, desc.is_written);
                if (desc.is_written) {
                    // Raw storage-buffer writes can also make an aliased cached image stale.
                    texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size,
                                                          stage.pgm_hash);
                }
                needs_barrier |= runtime.IsBufferAccessed(buffer, offset, size, desc.is_written);
            }
        }

        auto& set_write = set_writes[write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        ++binding.buffer;
    }
}

Rasterizer::ImageDescCacheEntry& Rasterizer::CachedImageDescEntry(const AmdGpu::Image& sharp,
                                                                   const Shader::ImageResource& res,
                                                                   u64 hash) {
    std::array<u64, 4> key;
    std::memcpy(key.data(), &sharp, sizeof(key));
    // ps4rt: needs_native too, since FindImage resolves partial height views only for it.
    const u32 flags = u32(res.is_written) | u32(res.is_depth) << 1 | u32(res.is_array) << 2 |
                      u32(res.needs_native) << 3;
    // Two-way set associative: a frame binds a few thousand distinct T#s.
    auto* set = &image_desc_cache[(hash % (image_desc_cache.size() / 2)) * 2];
    const auto matches = [&](const ImageDescCacheEntry& e) {
        return e.flags == flags && e.sharp == key;
    };
    const bool disabled = Ps4RtToggle::Disabled(Ps4RtToggle::ImageDescCache);
    ImageDescCacheEntry* slot = nullptr;
    if (!disabled && matches(set[0])) {
        slot = &set[0];
    } else if (!disabled && matches(set[1])) {
        slot = &set[1];
    } else {
        // Replace the least recently used way that no binding of this call points at.
        slot = set[0].last_use <= set[1].last_use ? &set[0] : &set[1];
        if (slot->pinned == bind_epoch) {
            slot = slot == &set[0] ? &set[1] : &set[0];
        }
        if (slot->pinned == bind_epoch) {
            slot = &image_desc_overflow.emplace_back();
        }
    }
    slot->last_use = ++desc_use_counter;
    auto& entry = *slot;
    if (entry.flags != flags || entry.sharp != key ||
        Ps4RtToggle::Disabled(Ps4RtToggle::ImageDescCache)) {
        entry.desc = VideoCore::TextureCache::ImageDesc{sharp, res};
        entry.sharp = key;
        entry.flags = flags;
        entry.found_generation = ~0ULL;
        entry.view_memo = {};
    }
    return entry;
}

void Rasterizer::BindTextures(const Shader::Info& stage, const PreparedStage* prepared,
                              Shader::Backend::Bindings& binding, u32& write_index,
                              bool& barrier, bool on_helper) {
    const u32 first_image_idx = image_infos.size();
    TextureSet* set_slot = nullptr;
    const bool transition_consumer = stage.sw_stage==Shader::SwStage::Fragment &&
        VideoCore::Ps4RtTransition::Consumer(stage.pgm_hash);
    if (!transition_consumer && !on_helper &&
        BindTexturesFromSet(stage, prepared, first_image_idx, barrier, set_slot)) {
        // Descriptor writes: one per image (no mip arrays in a memoized set).
        for (u32 i = 0; i < stage.images.size(); ++i) {
            auto& set_write = set_writes[write_index++];
            set_write.dstSet = VK_NULL_HANDLE;
            set_write.dstBinding = binding.unified++;
            set_write.dstArrayElement = 0;
            set_write.descriptorCount = 1;
            set_write.descriptorType = vk::DescriptorType::eSampledImage;
            set_write.pImageInfo = &image_infos[first_image_idx + i];
        }
        BindSamplers(stage, prepared, binding, write_index);
        return;
    }
    image_bindings.clear();
    image_binding_entries.clear();
    image_desc_overflow.clear();
    image_desc_storage.clear();
    ++bind_epoch;
    // To emulate storing to explicit mip levels, build a descriptor array with each mip level.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;
    // TextureSetMemo: what this call resolves, to remember when the set qualifies.
    std::array<TextureSetEntry, TextureSet::MaxImages> resolved{};
    bool set_ok = set_slot != nullptr;
    // SampleSceneProxies: bindings that may read a reduced scene proxy instead of the native
    // image (normalized sampling only; see Shader::ImageResource::needs_native).
    boost::container::small_vector<bool, 16> binding_proxy_ok;
    boost::container::small_vector<std::pair<size_t,vk::ImageView>,2> transition_views;
    const bool sample_proxies = !Ps4RtToggle::Disabled(Ps4RtToggle::SampleSceneProxies) &&
                                !on_helper && scene_targets->Reduced();

    for (u32 image_index = 0; image_index < stage.images.size(); ++image_index) {
        const auto& image_desc = stage.images[image_index];
        const auto tsharp =
            prepared ? prepared->image_sharps[image_index] : image_desc.GetSharp(stage);
        
        if (image_desc.CanSkipBinding(tsharp, [&](u32 i) {
                return prepared ? prepared->sampler_sharps[i] : stage.samplers[i].GetSharp(stage);
            })) {
            image_bindings.emplace_back(VideoCore::ImageId{}, &image_desc_storage.emplace_back());
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(false);
            image_descriptor_array_sizes.push_back(1);
            continue;
        }
        // ps4rt: a hash lookup per texture per draw for a diagnostic only.
        static const bool warn_meta = std::getenv("PS4RT_WARN_META_TEXTURE") != nullptr;
        if (warn_meta && texture_cache.IsMeta(tsharp.Address())) {
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
        }

        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid) {
            image_bindings.emplace_back(VideoCore::ImageId{}, &image_desc_storage.emplace_back());
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(false);
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        if (!memory->IsValidGpuMapping(tsharp.Address(), 0) ||
            !IsKnownFormat(data_fmt, num_fmt)) {
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid T# address={:#x}, pitch={}, width={}, "
                        "data_format={}, num_format={}",
                        tsharp.Address(), tsharp.pitch, tsharp.width, static_cast<u32>(data_fmt),
                        static_cast<u32>(num_fmt));
            image_bindings.emplace_back(VideoCore::ImageId{}, &image_desc_storage.emplace_back());
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(false);
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        // Same as image_desc.NumBindings(stage), without fetching the T# again.
        const u32 num_bindings =
            mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex
                ? static_cast<u32>(tsharp.last_level - tsharp.base_level + 1)
                : 1u;
        const bool proxy_candidate = sample_proxies && num_bindings == 1 &&
                                     !image_desc.needs_native && !image_desc.is_written &&
                                     mip_fallback_mode == Shader::MipStorageFallbackMode::None;

        auto& desc_entry =
            prepared ? CachedImageDescEntry(tsharp, image_desc, prepared->image_hashes[image_index])
                     : CachedImageDescEntry(tsharp, image_desc);
        if (transition_consumer && !on_helper && num_bindings==1 &&
            mip_fallback_mode==Shader::MipStorageFallbackMode::None && !image_desc.is_written) {
            if (const auto snapshot=texture_cache.FindTransitionSnapshot(desc_entry.desc)) {
                transition_views.emplace_back(image_bindings.size(),snapshot);
                image_bindings.emplace_back(VideoCore::ImageId{},
                    &image_desc_storage.emplace_back(desc_entry.desc));
                image_binding_entries.push_back(nullptr);
                binding_proxy_ok.push_back(false);
                image_descriptor_array_sizes.push_back(1);
                set_ok=false;
                continue;
            }
        }
        const auto debug_native = [&](const VideoCore::Image& image) {
        };
        for (auto i = 0; i < num_bindings; i++) {
            // ps4rt: a plain binding (no mip override) of the same T# resolves to the same image
            // while no image was registered or unregistered.
            if (mip_fallback_mode == Shader::MipStorageFallbackMode::None &&
                desc_entry.found_generation == texture_cache.RegistryGeneration() &&
                !Ps4RtToggle::Disabled(Ps4RtToggle::TextureBindingMemo)) {
                desc_entry.pinned = bind_epoch;
                auto& [image_id, _] =
                    image_bindings.emplace_back(desc_entry.found_id, &desc_entry.found_desc);
                image_binding_entries.push_back(&desc_entry);
                binding_proxy_ok.push_back(proxy_candidate);
                texture_cache.MarkFound(image_id);
                auto* image = &texture_cache.GetImage(image_id);
                if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                    image_id = depth_image_id;
                    image = &texture_cache.GetImage(image_id);
                }
                debug_native(*image);
                if (image->binding.is_bound) {
                    image->binding.force_general |= image_desc.is_written;
                }
                image->binding.is_bound = 1u;
                continue;
            }
            auto& desc = image_desc_storage.emplace_back(desc_entry.desc);
            auto& [image_id, _] = image_bindings.emplace_back(VideoCore::ImageId{}, &desc);
            image_binding_entries.push_back(nullptr);
            binding_proxy_ok.push_back(proxy_candidate);

            if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                ASSERT(num_bindings == 1);
                desc.view_info.range.base.level += image_desc.constant_mip_index;
                desc.view_info.range.extent.levels = 1;
            } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                desc.view_info.range.base.level += i;
                desc.view_info.range.extent.levels = 1;
            }

            const u64 generation = texture_cache.RegistryGeneration();
            image_id = texture_cache.FindImage(desc);
            if (mip_fallback_mode == Shader::MipStorageFallbackMode::None &&
                generation == texture_cache.RegistryGeneration() &&
                desc_entry.pinned != bind_epoch) {
                desc_entry.found_generation = generation;
                desc_entry.found_id = image_id;
                desc_entry.found_desc = desc;
                desc_entry.view_memo = {};
            }
            auto* image = &texture_cache.GetImage(image_id);
            if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                // If this image has an associated depth image, it's a stencil attachment.
                // Redirect the access to the actual depth-stencil buffer.
                image_id = depth_image_id;
                image = &texture_cache.GetImage(image_id);
            }
            debug_native(*image);
            if (image->binding.is_bound) {
                // The image is already bound. In case if it is about to be used as storage we
                // need to force general layout on it.
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
        }

        image_descriptor_array_sizes.push_back(num_bindings);
    }

    // Second pass to re-bind images that were updated after binding
    for (u32 binding_index = 0; binding_index < image_bindings.size(); ++binding_index) {
        auto& [image_id, desc_ptr] = image_bindings[binding_index];
        auto* memo_entry = image_binding_entries[binding_index];
        const auto& desc = *desc_ptr;
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        const auto snapshot=std::find_if(transition_views.begin(),transition_views.end(),
            [&](const auto& v){return v.first==binding_index;});
        if (snapshot!=transition_views.end()) {
            image_infos.emplace_back(VK_NULL_HANDLE,snapshot->second,vk::ImageLayout::eGeneral);
            continue;
        }
        if (!image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                auto& rebind_desc = image_desc_storage.emplace_back(desc);
                desc_ptr = &rebind_desc;
                image_id = texture_cache.FindImage(rebind_desc);
                memo_entry = nullptr;
                set_ok = false;
            }

            bound_images.emplace_back(image_id);

            auto& image = texture_cache.GetImage(image_id);
            auto& image_view = texture_cache.FindTexture(
                image_id, desc, memo_entry ? &memo_entry->view_memo : nullptr, !on_helper);
            const auto binding = image.binding;

            // Opt-in readback of selected shader inputs during a requested frame capture.
            // Run before the normal binding transition so the transfer layout is restored.

            

            // ps4rt: a proxied scene image sampled with normalized coordinates reads the proxy:
            // no resample to the native size (the native image is not touched).
            if (binding_proxy_ok[binding_index] && !is_storage && !binding.force_general &&
                !binding.is_target &&
                !(upscaler->Enabled() && upscaler->RedirectsSampled(image_id))) {
                if (const auto proxy = scene_targets->SampleProxy(image, desc.view_info)) {
                    image.usage.texture = 1u;
                    image_infos.emplace_back(VK_NULL_HANDLE, proxy->view, proxy->layout);
                    if (set_ok && binding_index < resolved.size()) {
                        resolved[binding_index] = {image_id, proxy->view, image.backing,
                                                   desc.view_info.range, true};
                    }
                    ++proxy_samples;
                    continue;
                }
            }
            // The image is either bound as storage in a separate descriptor or bound as render
            // target in feedback loop. Depth images are excluded because they can't be bound as
            // storage and feedback loop doesn't make sense for them
            if ((binding.force_general || binding.is_target) && !image.info.props.is_depth) {
                if (instance.IsAttachmentFeedbackLoopLayoutSupported() && image.binding.is_target) {
                    barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT,
                        vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead);
                } else {
                    barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
                }
            } else {
                if (is_storage) {
                    barrier |= runtime.Transit(
                        &image, vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                        desc.view_info.range);
                } else {
                    const auto new_layout = image.info.props.is_depth
                                                ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                                : vk::ImageLayout::eShaderReadOnlyOptimal;
                    barrier |= runtime.Transit(
                        &image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                        vk::AccessFlagBits2::eShaderRead, desc.view_info.range);
                }
            }
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;
            

            vk::ImageView view = *image_view.image_view;
            vk::ImageLayout layout = image.backing->state.layout;
            if (upscaler->Enabled()) {
                // ps4rt: the display pass reads the upscaled frame (scaled presets).
                upscaler->RedirectSampled(image_id, image_view.info, view, layout);
            }
            image_infos.emplace_back(VK_NULL_HANDLE, view, layout);
            if (set_ok && binding_index < resolved.size()) {
                resolved[binding_index] = {image_id, *image_view.image_view, image.backing,
                                           desc.view_info.range};
                set_ok = !is_storage && !binding.force_general && !binding.is_target;
            }
        }
    }
    if (set_ok && image_bindings.size() == stage.images.size()) {
        // Remember the set: the same T#s resolve the same way while no image is (un)registered.
        set_slot->generation = texture_cache.RegistryGeneration();
        set_slot->scene_generation = scene_targets->Generation();
        set_slot->count = static_cast<u32>(stage.images.size());
        std::copy_n(resolved.begin(), set_slot->count, set_slot->entries.begin());
    } else if (set_slot) {
        set_slot->key = 0;
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        const auto& desc = *image_bindings[image_binding_idx].second;
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        auto& set_write = set_writes[write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    BindSamplers(stage, prepared, binding, write_index);
}

void Rasterizer::BindSamplers(const Shader::Info& stage, const PreparedStage* prepared,
                              Shader::Backend::Bindings& binding, u32& write_index) {
    for (u32 sampler_index = 0; sampler_index < stage.samplers.size(); ++sampler_index) {
        const auto& sampler = stage.samplers[sampler_index];
        auto ssharp =
            prepared ? prepared->sampler_sharps[sampler_index] : sampler.GetSharp(stage);
        
        const auto vk_sampler = texture_cache.GetSampler(
            ssharp, Regs().ta_bc_base, sampler.is_depth, sampler.is_depth ? 0.0f : sampler_lod_bias);
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        auto& set_write = set_writes[write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
    }
}

Rasterizer::BeginSignature Rasterizer::MakeBeginSignature(const GraphicsPipeline* pipeline) const {
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = Regs();
    BeginSignature sig;
    const u32 num_color = std::bit_width(key.mrt_mask);
    for (u32 cb = 0; cb < num_color && cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
        sig.ids[cb] = cb_descs[cb].first;
        if (cb_descs[cb].first) {
            sig.views[cb] = cb_descs[cb].second.view_info;
        }
        sig.color_samples[cb] = key.color_samples[cb];
    }
    sig.ids[AmdGpu::NUM_COLOR_BUFFERS] = db_desc.first;
    if (db_desc.first) {
        sig.views[AmdGpu::NUM_COLOR_BUFFERS] = db_desc.second.view_info;
    }
    sig.mrt_mask = key.mrt_mask;
    sig.num_samples = key.num_samples;
    sig.motion = key.motion_vectors;
    sig.scene_started = scene_started;
    sig.raster_scaling = upscaler->RasterScaling();
    sig.upscaler_state = upscaler->RedirectState();
    sig.generation = texture_cache.RegistryGeneration();
    std::memcpy(&sig.depth_control, &regs.depth_control, sizeof(u32));
    sig.depth_valid = regs.depth_buffer.DepthValid();
    sig.stencil_valid = regs.depth_buffer.StencilValid();
    return sig;
}

bool Rasterizer::BindTexturesFromSet(const Shader::Info& stage, const PreparedStage* prepared,
                                     u32 first_image_idx, bool& barrier, TextureSet*& slot) {
    const u32 count = static_cast<u32>(stage.images.size());
    if (!prepared || count == 0 || count > TextureSet::MaxImages ||
        Ps4RtToggle::Disabled(Ps4RtToggle::TextureSetMemo) || false) {
        return false;
    }
    for (u32 i = 0; i < count; ++i) {
        const auto& image = stage.images[i];
        // This memo is keyed only by image sharps. A folded sample also depends
        // on sampler wrap state, which can change with identical image sharps.
        // Ordinary texel-dependent images retain their existing fast path.
        if ((!image.requires_image_binding &&
             Shader::ConstantSampleImage(prepared->image_sharps[i], image.constant_sample_components)) ||
            image.is_written ||
            image.mip_fallback_mode != Shader::MipStorageFallbackMode::None) {
            return false;
        }
    }
    u64 key = XXH3_64bits_withSeed(prepared->image_hashes, count * sizeof(u64),
                                   reinterpret_cast<u64>(&stage));
    key |= 1; // 0 marks an empty slot
    auto& set = texture_sets[key % texture_sets.size()];
    slot = &set;
    const u64 generation = texture_cache.RegistryGeneration();
    const bool match = set.key == key && set.stage == &stage && set.count == count &&
                       set.generation == generation &&
                       set.scene_generation == scene_targets->Generation() &&
                       std::equal(set.hashes.begin(), set.hashes.begin() + count,
                                  prepared->image_hashes);
    if (!match) {
        ++texture_set_why[set.key != key ? 0 : set.generation != generation ? 1 : 3];
        ++texture_set_misses;
        set.key = key;
        set.stage = &stage;
        set.count = 0;
        set.generation = ~0ull;
        std::copy_n(prepared->image_hashes, count, set.hashes.begin());
        return false;
    }
    // Every image still has the backing its view belongs to and needs no refresh.
    for (u32 i = 0; i < count; ++i) {
        const auto& entry = set.entries[i];
        if (!entry.id) {
            continue;
        }
        const auto& image = texture_cache.GetImage(entry.id);
        // Proxy entries need a current proxy; native entries of a proxied image go through
        // BindTextures, which may sample the proxy instead.
        const bool proxies_on = !Ps4RtToggle::Disabled(Ps4RtToggle::SampleSceneProxies);
        if (image.backing != entry.backing || image.binding.needs_rebind ||
            image.binding.is_target || !texture_cache.IsUpToDate(entry.id) ||
            (entry.proxy ? !proxies_on ||
                               !scene_targets->ProxyCurrent(image, entry.range.base.level,
                                                            entry.range.extent.levels)
                         : image.scene_proxy && proxies_on) ||
            (upscaler->Enabled() && upscaler->RedirectsSampled(entry.id))) {
            ++texture_set_why[2];
            ++texture_set_misses;
            set.generation = ~0ull;
            return false;
        }
    }
    ++texture_set_hits;
    slot = nullptr;
    for (u32 i = 0; i < count; ++i) {
        const auto& entry = set.entries[i];
        if (!entry.id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            continue;
        }
        texture_cache.MarkFound(entry.id);
        auto& image = texture_cache.GetImage(entry.id);
        image.binding.is_bound = 1u;
        bound_images.emplace_back(entry.id);
        image.usage.texture = 1u;
        if (entry.proxy) {
            image_infos.emplace_back(VK_NULL_HANDLE, entry.view,
                                     scene_targets->PrepareSample(image, entry.range.base.level));
            ++proxy_samples;
            continue;
        }
        const auto new_layout = image.info.props.is_depth
                                    ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                    : vk::ImageLayout::eShaderReadOnlyOptimal;
        barrier |= runtime.Transit(&image, new_layout, vk::PipelineStageFlagBits2::eAllCommands,
                                   vk::AccessFlagBits2::eShaderRead, entry.range);
        image_infos.emplace_back(VK_NULL_HANDLE, entry.view, image.backing->state.layout);
    }
    (void)first_image_idx;
    return true;
}

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    // ps4rt: RenderStateMemo (see BeginMemo).
    const bool memo_on = !Ps4RtToggle::Disabled(Ps4RtToggle::RenderStateMemo) && !false;
    
    BeginSignature signature;
    if (memo_on) {
        signature = MakeBeginSignature(pipeline);
        const auto& regs = Regs();
        bool usable = begin_memo.valid && scheduler.IsRenderingWith(begin_memo.state) &&
                      !regs.depth_render_control.depth_clear_enable &&
                      !regs.depth_render_control.stencil_clear_enable &&
                      signature == begin_memo.signature;
        for (const auto id : signature.ids) {
            if (usable && id) {
                const auto& image = texture_cache.GetImage(id);
                usable = !image.binding.is_bound && !image.binding.needs_rebind;
            }
        }
        if (usable) {
            ++begin_memo_hits;
            attachment_feedback_loop = false;
            push_data.scene_size = begin_memo.scene_size;
            target_scale = begin_memo.target_scale;
            return begin_memo.state;
        }
        ++begin_memo_misses;
    }
    begin_memo.valid = false;
    RenderState state = BeginRenderingFull(pipeline);
    if (memo_on && !attachment_feedback_loop) {
        // (is_clear of the depth attachment shares its bytes with has_depth.)
        bool clears = state.depth_stencil_attachment.depth_clear ||
                      state.depth_stencil_attachment.stencil_clear;
        for (u32 i = 0; i < state.num_color_attachments; ++i) {
            clears |= state.color_attachments[i].is_clear != 0;
        }
        if (!clears) {
            // Recomputed after the full path: it may have started the scene (scene_started).
            begin_memo = {true, MakeBeginSignature(pipeline), state, push_data.scene_size,
                          target_scale};
        }
    }
    return state;
}

RenderState Rasterizer::BeginRenderingFull(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = false;
    // ps4rt: frame analyzer census of this pass (sample counts and path, stencil, clears).


    using VulkanUpscalerTarget = TemporalUpscaler::Target;
    VulkanUpscalerTarget redirect{};
    bool color_redirected = false, depth_redirected = false;
    std::pair<u32, u32> guest_extent{1, 1};
    std::pair<vk::ImageView, vk::ImageLayout> original_color{};
    const auto& regs = Regs();
    const auto& key = pipeline->GetGraphicsKey();
    if (std::popcount(key.mrt_mask & 0x7f) >= 5 && db_desc.first &&
        scene_targets->EligibleScene(texture_cache.GetImage(db_desc.first))) scene_started = true;
    // A proxy attachment cannot represent MSAA or a feedback loop that reads the
    // same image through the guest's native descriptor during this draw.
    bool reduced = scene_started && upscaler->RasterScaling() && key.num_samples == 1;
    // native size (see NoteFrameStart).

    
    // Only this pass's attachments: slots past the mask's width keep earlier passes' targets,
    // often the G-buffer images the lighting passes sample (bound), which kept those native.
    const u32 num_attachments = Ps4RtToggle::Disabled(Ps4RtToggle::SceneAttachmentsOnly)
                                    ? u32(cb_descs.size())
                                    : u32(std::bit_width(key.mrt_mask));
    // All attachments of a reduced pass share one proxy size (scene or half resolution).
    std::optional<SceneResolution::Size> pass_size;
    const auto same_size = [&](const VideoCore::Image& image, u32 level) {
        const auto proxy = scene_targets->ProxySize(image, level);
        if (!pass_size) pass_size = proxy;
        return *pass_size == proxy;
    };
    for (u32 cb = 0; cb < num_attachments; ++cb) {
        const auto& [id, desc] = cb_descs[cb];
        if (id) {
            const auto& image = texture_cache.GetImage(id);
            // Mip levels of eligible chains have their own proxies (SceneTargets::Get).
            if (!scene_targets->Eligible(image) || image.binding.is_bound ||
                image.binding.needs_rebind || desc.view_info.range.base.layer ||
                !same_size(image, desc.view_info.range.base.level)) reduced = false;
            
        }
    }
    if (db_desc.first && (!scene_targets->Eligible(texture_cache.GetImage(db_desc.first)) ||
                          !same_size(texture_cache.GetImage(db_desc.first), 0))) {
        reduced = false;
    }
    
    push_data.scene_size = reduced ? SceneResolution::Pack(scene_targets->Size()) : 0;
    
    RenderState state;
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            last_targets[cb].generation = ~0ULL; // FindImage may rewrite the description
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        texture_cache.UpdateImage(image_id);
        runtime.SetBackingSamples(image, key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        const bool is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);

        if (reduced) {
            // The native image is resolved lazily when a shader/copy actually reads it.
        } else if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            runtime.FlushBarriers();
            needs_barrier |=
                runtime.Transit(image,
                                instance.IsAttachmentFeedbackLoopLayoutSupported()
                                    ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                                    : vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                vk::AccessFlagBits2::eColorAttachmentWrite |
                                    vk::AccessFlagBits2::eColorAttachmentRead);
            attachment_feedback_loop = true;
        } else {
            needs_barrier |= runtime.Transit(image, vk::ImageLayout::eColorAttachmentOptimal,
                                             vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                             vk::AccessFlagBits2::eColorAttachmentWrite |
                                                 vk::AccessFlagBits2::eColorAttachmentRead,
                                             desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        if (reduced) {
            const auto target = scene_targets->Attachment(image_id, desc.view_info);
            attachment.image_view = target.view;
            attachment.image_layout = target.layout;
        }
        image->usage.render_target = 1u;
        if (cb == 0 && upscaler->Enabled()) {
            color_redirected = upscaler->RedirectColor(image_id, desc.view_info, redirect);
            if (color_redirected) {
                guest_extent = {image->info.size.width, image->info.size.height};
                original_color = {attachment.image_view, attachment.image_layout};
                attachment.image_view = redirect.view;
                attachment.image_layout = redirect.layout;
            }
        }
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& image_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);

        const auto slice = image_view.info.range.base.layer;
        const bool is_depth_clear =
            (regs.depth_render_control.depth_clear_enable &&
             regs.depth_control.depth_enable &&
             regs.depth_control.depth_write_enable) ||
            texture_cache.IsMetaCleared(htile_address, slice);
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        // Preserve the selected attachment before either a register or HTILE clear.
        // This also covers indirect draws, which have a separate recording path.
        texture_cache.TouchMeta(htile_address, slice, false);
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const bool has_stencil = image.info.props.has_stencil;
        // Stencil writes can be enabled while depth writes are off.
        const bool stencil_write =
            has_stencil && regs.depth_control.stencil_enable && !desc.view_info.is_storage;
        const auto new_layout = desc.view_info.is_storage
                                    ? has_stencil ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                                  : vk::ImageLayout::eDepthAttachmentOptimal
                                : stencil_write
                                    ? vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal
                                : has_stencil ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                              : vk::ImageLayout::eDepthReadOnlyOptimal;
        
        if (!reduced) needs_barrier |= runtime.Transit(&image, new_layout,
                                         vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                             vk::PipelineStageFlagBits2::eLateFragmentTests,
                                         vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                                             vk::AccessFlagBits2::eDepthStencilAttachmentRead,
                                         desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image.backing->state.layout;
        attachment.clear_value = {};
        attachment.is_clear = 0;

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        if (reduced) {
            const auto target = scene_targets->Attachment(image_id, desc.view_info);
            attachment.image_view = target.view;
            attachment.image_layout = target.layout;
        }
        image.usage.depth_target = true;
        if (upscaler->Enabled()) {
            VulkanUpscalerTarget depth_redirect;
            const bool display_pass = color_redirected && cb_descs[0].first &&
                DisplayRegistry::IsDisplayBuffer(
                    texture_cache.GetImage(cb_descs[0].first).info.guest_address);
            bool no_color = true;
            for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
                no_color &= !state.color_attachments[cb].image_view;
            }
            depth_redirected =
                upscaler->RedirectDepth(image_id, depth_redirect, display_pass, no_color);
            if (depth_redirected) {
                guest_extent = {image.info.size.width, image.info.size.height};
                attachment.image_view = depth_redirect.view;
                attachment.image_layout = depth_redirect.layout;
                redirect = depth_redirect;
            }
        }
    } else {
        state.depth_stencil_attachment = {};
    }

    // ps4rt: a pass drawn into the upscaler's output-size images (UI, display pass): every
    // attachment must be redirected, viewports and scissors are scaled.
    target_scale = {1.0f, 1.0f};
    if (color_redirected || depth_redirected) {
        u32 color_targets = 0;
        for (u32 cb = 0; cb < state.num_color_attachments; ++cb) {
            color_targets += state.color_attachments[cb].image_view ? 1 : 0;
        }
        const bool has_depth = bool(db_desc.first);
        if ((color_targets > 0 && !color_redirected) || (has_depth && !depth_redirected) ||
            color_targets > 1) {
            static u32 warned = 0;
            if (warned < 8) {
                ++warned;
                const auto describe = [&](VideoCore::ImageId id) {
                    if (!id) {
                        return std::string{"-"};
                    }
                    const auto& info = texture_cache.GetImage(id).info;
                    return fmt::format("{:#x} {}x{} {}", info.guest_address, info.size.width,
                                       info.size.height, vk::to_string(info.pixel_format));
                };
                std::printf("Upscaler: pass not redirected (color %s: %s, depth %s: %s, %u "
                            "targets)\n",
                            color_redirected ? "yes" : "no", describe(cb_descs[0].first).c_str(),
                            depth_redirected ? "yes" : "no", describe(db_desc.first).c_str(),
                            state.num_color_attachments);
            }
            if (color_redirected) {
                upscaler->CancelDisplay(cb_descs[0].first);
                state.color_attachments[0].image_view = original_color.first;
                state.color_attachments[0].image_layout = original_color.second;
            }
            if (depth_redirected) {
                auto& depth_image = texture_cache.GetImage(db_desc.first);
                state.depth_stencil_attachment.image_view =
                    *texture_cache.FindDepthTarget(db_desc.first, db_desc.second).image_view;
                state.depth_stencil_attachment.image_layout = depth_image.backing->state.layout;
            }
        } else {
            state.width = redirect.width;
            state.height = redirect.height;
            // The UI movie viewport is now 1920x1080 even when the scene is 960x540.
            // A native viewport (or native window-space UI vertices) must not be doubled
            // again. The final display copy still uses the guest render-size coordinates.
            const auto& viewport = regs.viewports[0];
            const bool native_coordinates = redirect.native_ui &&
                (UiComposition::NativeViewport(viewport.xscale * 2, viewport.yscale * 2) ||
                 (regs.IsClipDisabled() && !regs.viewport_control.xscale_enable &&
                  !regs.viewport_control.yscale_enable));
            target_scale = UiComposition::Scale(guest_extent.first, guest_extent.second,
                                                redirect.width, redirect.height,
                                                native_coordinates);
        }
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    if (reduced) {
        // Half-resolution passes keep the scene's factor (proxy / native = scene / 1920).
        const auto size = scene_targets->Size();
        const auto proxy = pass_size.value_or(size);
        state.width = proxy.width;
        state.height = proxy.height;
        target_scale = {float(size.width) / 1920.0f, float(size.height) / 1080.0f};
    }
    
    // ps4rt: object motion vector attachment of G-buffer pipelines.
    if (key.motion_vectors) {
        object_motion->Attach(state, state.width, state.height);
    }
    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = CbExtent(0);
    const auto& mrt1_hint = CbExtent(1);
    VideoCore::TextureCache::ImageDesc mrt0_desc{Regs().color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{Regs().color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                                 Regs().color_buffers[0].Address(),
                                 Regs().color_buffers[1].Address()));
    runtime.ResolveImage(&mrt0_image, &mrt1_image, mrt0_desc.view_info.range,
                         mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = Regs();

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), DbExtent(), false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), DbExtent(), true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    auto& write_image = texture_cache.GetImage(texture_cache.FindImage(write_desc));

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = Regs().depth_view.slice_start;
    sub_range.extent.layers = Regs().depth_view.NumSlices() - sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    runtime.CopyDepthStencil(&read_image, &write_image, sub_range);

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    DrainDrawPipe();
    ASSERT_MSG(address % 4 == 0 && num_bytes % 4 == 0,
               "FillBuffer address and size must be a multiple of 4 bytes");
    if (!is_gds) {
        texture_cache.ClearMeta(address);
        if (!buffer_cache.IsRegionGpuModified(address, num_bytes)) {
            u32* buffer = std::bit_cast<u32*>(address);
            std::fill(buffer, buffer + (num_bytes / sizeof(u32)), value);
            return;
        }
    }
    const auto [buffer, offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (is_gds) {
            return {buffer_cache.GetGdsBuffer(), address};
        }
        return buffer_cache.ObtainBuffer(address, num_bytes, true);
    }();
    runtime.FillBuffer(buffer, offset, num_bytes, value);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    DrainDrawPipe();
    if (!dst_gds && !buffer_cache.IsRegionGpuModified(dst, num_bytes)) {
        if (!src_gds && !buffer_cache.IsRegionGpuModified(src, num_bytes) &&
            !texture_cache.FindImageFromRange(src, num_bytes)) {
            // Both buffers were not transferred to GPU yet. Can safely copy in host memory.
            std::memcpy(std::bit_cast<void*>(dst), std::bit_cast<void*>(src), num_bytes);
            return;
        }
    }
    texture_cache.InvalidateMemoryFromGPU(dst, num_bytes);
    const auto* gds_buffer = buffer_cache.GetGdsBuffer();
    const auto [src_buffer, src_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (src_gds) {
            return {gds_buffer, src};
        }
        return buffer_cache.ObtainBuffer(src, num_bytes, false, true);
    }();
    const auto [dst_buffer, dst_offset] = [&] -> std::pair<const VideoCore::Buffer*, u64> {
        if (dst_gds) {
            return {gds_buffer, dst};
        }
        return buffer_cache.ObtainBuffer(dst, num_bytes, true, true);
    }();
    const vk::BufferCopy copy = {
        .srcOffset = src_offset,
        .dstOffset = dst_offset,
        .size = num_bytes,
    };
    runtime.CopyBuffer(src_buffer, dst_buffer, std::span{&copy, 1});
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    DrainDrawPipe();
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size, bool assume_locks) {
    DrainDrawPipe();
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    texture_cache.ResolveReadbacks(addr, size, assume_locks, VideoCore::ReadbackSource::CpuWrite);
    if (Ps4RtGpuWorkarounds::Current().preserve_rgba8_cpu_writes) {
        texture_cache.PreserveGpuImagesForCpuWrite(addr, size, assume_locks);
    }
    buffer_cache.InvalidateMemory(addr, size, assume_locks);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

bool Rasterizer::OnWriteFault(VAddr addr, bool assume_locks) {
    DrainDrawPipe();
    if (!InvalidateMemory(addr, 8, assume_locks)) {
        return false;
    }
    buffer_cache.ExtendWriteFault(addr);
    return true;
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size, bool assume_locks) {
    DrainDrawPipe();
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    const bool image_readback =
        texture_cache.ResolveReadbacks(addr, size, assume_locks, VideoCore::ReadbackSource::CpuRead);
    if (!image_readback || buffer_cache.IsRegionGpuModified(addr, size)) {
        buffer_cache.ReadMemory(addr, size, false, assume_locks);
    }
    return true;
}

void Rasterizer::ProcessDownloadImages() {
    DrainDrawPipe();
    texture_cache.ProcessDownloadImages();
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::RegisterMemory(VAddr addr, u64 size) {
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    DrainDrawPipe();
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.UnmapMemory(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    UpdateViewportScissorState();
    UpdateDepthStencilState();
    UpdatePrimitiveState(is_indexed);
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.CommitWith(instance.IsDepthBoundsSupported(),
                             instance.IsDynamicColorWriteMaskSupported(),
                             instance.IsAttachmentFeedbackLoopLayoutSupported(),
                             [&](auto&& command) { scheduler.Record(std::move(command)); });
}

void Rasterizer::UpdateViewportScissorState() const {
    const auto& regs = Regs();

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            // ps4rt: sub-pixel jitter of scene geometry for the temporal upscaler; the same
            // shift as jittering the projection.
            // The ultrawide shift and the high-res HUD terms are guest display pixels: scaled
            // by target_scale like the viewport when FSR draws the HUD at its output size.
            const float tx = target_scale[0], ty = target_scale[1];
            viewport.x = (xoffset - xscale * g_uw_stretch + float(g_uw_shift)) * tx + draw_jitter[0];
            viewport.y = (yoffset - yscale) * ty + draw_jitter[1];
            viewport.width = xscale * g_uw_stretch * 2.0f * tx;
            viewport.height = yscale * 2.0f * ty;
            if (g_hud_scale != 1.0f) {
                // High-res display: scale the HUD about the display centre (canvas
                // elements) or keep the edge at the display origin (top-left pixel quads).
                const float k = g_hud_scale;
                if (g_hud_anchor) {
                    const float ax = (xoffset + g_hud_ax) * tx, ay = (yoffset - g_hud_ay) * ty;
                    viewport.x = ax + k * (viewport.x - ax);
                    viewport.y = ay + k * (viewport.y - ay);
                } else if (g_hud_pixel) {
                    viewport.x = (xoffset - xscale * g_uw_stretch + k * float(g_uw_shift)) * tx;
                    viewport.y = (yoffset + yscale - 2.0f * k * yscale) * ty;
                } else {
                    viewport.x = (xoffset - k * xscale * g_uw_stretch + k * float(g_uw_shift)) * tx;
                    viewport.y = (yoffset - k * yscale) * ty;
                }
                viewport.width *= k;
                viewport.height *= k;
            }
        }

        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        if (g_uw_canvas_x1) { // ps4rt (ultrawide): HUD canvas clip
            const s32 cx0 = s32(std::lround(float(g_uw_canvas_x0) * g_hud_scale));
            const s32 cx1 = s32(std::lround(float(g_uw_canvas_x1) * g_hud_scale));
            vp_scsr.top_left_x = s16(std::max<s32>(vp_scsr.top_left_x, cx0));
            vp_scsr.bottom_right_x = s16(std::min<s32>(vp_scsr.bottom_right_x, cx1));
            if (vp_scsr.bottom_right_x < vp_scsr.top_left_x) vp_scsr.bottom_right_x = vp_scsr.top_left_x;
        }
        if (target_scale[0] != 1.0f || target_scale[1] != 1.0f) {
            const auto scale = [](s32 v, float f) { return s32(std::lround(float(v) * f)); };
            const s32 x0 = scale(vp_scsr.top_left_x, target_scale[0]);
            const s32 y0 = scale(vp_scsr.top_left_y, target_scale[1]);
            const s32 x1 = scale(vp_scsr.top_left_x + s32(vp_scsr.GetWidth()), target_scale[0]);
            const s32 y1 = scale(vp_scsr.top_left_y + s32(vp_scsr.GetHeight()), target_scale[1]);
            scissors.push_back({
                .offset = {x0, y0},
                .extent = {u32(std::max(x1 - x0, 0)), u32(std::max(y1 - y0, 0))},
            });
            if (i == 0) g_uw_movie_x0 = x0, g_uw_movie_x1 = x1;
        } else {
            scissors.push_back({
                .offset = {vp_scsr.top_left_x, vp_scsr.top_left_y},
                .extent = {vp_scsr.GetWidth(), vp_scsr.GetHeight()},
            });
            if (i == 0) g_uw_movie_x0 = vp_scsr.top_left_x, g_uw_movie_x1 = vp_scsr.bottom_right_x;
        }
    }

    if (viewports.empty()) {
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

vk::Pipeline Rasterizer::SquarePass(const GraphicsPipeline& pipeline) const {
    const vk::Pipeline square = pipeline.SquarePassHandle();
    static const bool enabled = [] {
        const char* env = std::getenv("PS4RT_SQUARE_PASS");
        return !env || env[0] != '0';
    }();
    if (!square || !enabled) [[likely]] {
        return {};
    }
    // MIN(s * s, d * d) = MIN(s, d)^2: the second draw needs the first one's coverage, which a
    // depth or stencil write would change.
    const auto& regs = Regs();
    const bool depth_write = regs.depth_control.depth_enable &&
                             regs.depth_control.depth_write_enable && regs.depth_buffer.DepthValid();
    if (depth_write || regs.depth_control.stencil_enable) {
        static bool warned = false;
        if (!std::exchange(warned, true)) {
            LOG_WARNING(Render_Vulkan, "Scaled MIN/MAX blend not squared: the draw writes depth "
                                       "or stencil");
        }
        return {};
    }
    return square;
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_stencil = GetEffectiveDepthStencilState(regs);
    const bool depth_test_enabled = depth_stencil.depth_test_enable;
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    dynamic_state.SetDepthWriteEnabled(depth_stencil.depth_write_enable &&
                                       !regs.depth_render_control.depth_clear_enable);
    if (depth_test_enabled) {
        auto compare = LiverpoolToVK::CompareOp(regs.depth_control.depth_func);
#if PS4RT_DIAGNOSTICS
        if (diag.depth_always) compare = vk::CompareOp::eAlways;
#endif
        dynamic_state.SetDepthCompareOp(compare);
    }

    const bool depth_bounds_test_enabled = depth_stencil.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        const float slope = front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias, RenderOptions::Current().bias ? slope : slope / 16.f);
    }

    const bool stencil_test_enabled = depth_stencil.stencil_test_enable;
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        // GCN REPLACE_OP writes DB_STENCILREFMASK.STENCILOPVAL, so a face whose stencil ops
        // include ReplaceOp takes its Vulkan reference from op_val.
        const auto& sc = regs.stencil_control;
        const auto uses_op_val = [](AmdGpu::StencilFunc fail, AmdGpu::StencilFunc zpass,
                                    AmdGpu::StencilFunc zfail) {
            return fail == AmdGpu::StencilFunc::ReplaceOp ||
                   zpass == AmdGpu::StencilFunc::ReplaceOp ||
                   zfail == AmdGpu::StencilFunc::ReplaceOp;
        };
        const bool front_op =
            uses_op_val(sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front);
        const bool back_op =
            regs.depth_control.backface_enable
                ? uses_op_val(sc.stencil_fail_back, sc.stencil_zpass_back, sc.stencil_zfail_back)
                : front_op;
        const auto ref_conflict = [](AmdGpu::CompareFunc func, const AmdGpu::StencilRefMask& ref) {
            return func != AmdGpu::CompareFunc::Always && func != AmdGpu::CompareFunc::Never &&
                   ref.stencil_test_val != ref.stencil_op_val;
        };
        if ((front_op && ref_conflict(regs.depth_control.stencil_ref_func, front)) ||
            (back_op && regs.depth_control.backface_enable &&
             ref_conflict(regs.depth_control.stencil_bf_func, back))) {
            LOG_WARNING(Render_Vulkan, "Stencil test requires test_val while ReplaceOp requires "
                                       "op_val; the stencil test will use op_val");
        }
        dynamic_state.SetStencilReferences(front_op ? front.stencil_op_val : front.stencil_test_val,
                                           back_op ? back.stencil_op_val : back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        // Quad and rect lists are emulated using tessellation.
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               type == AmdGpu::PrimitiveType::QuadList || type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = Regs();
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    // ps4rt diagnostics: a hidden draw writes no colour (the next draw restores the masks).
    dynamic_state.SetColorWriteMasks(diag_hide_color
                                         ? decltype(pipeline->GetGraphicsKey().write_masks){}
                                         : pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    DrainDrawPipe();
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.CommandBuffer();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

std::thread::id Rasterizer::GetGpuCommandProcessorThread() {
    return liverpool->GetGpuCommandProcessorThread();
}

#if defined(__linux__) || defined(_WIN32)
u32 Rasterizer::GetGpuCommandProcessorThreadId() {
    return liverpool->GetGpuCommandProcessorThreadId();
}
#endif

} // namespace Vulkan

namespace Vulkan {





} // namespace Vulkan
