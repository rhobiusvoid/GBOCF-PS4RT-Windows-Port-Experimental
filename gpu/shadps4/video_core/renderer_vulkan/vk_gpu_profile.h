// SPDX-License-Identifier: GPL-2.0-or-later
// ps4rt: GPU time per shader and runtime operation (diagnostics, PS4RT_GPU_PROFILE=1). Every
// PS4RT_GPU_PROFILE_EVERY guest flips (default 600) it captures PS4RT_GPU_PROFILE_FRAMES frames
// (default 8, at most the capture budget's frame limit): a timestamp before and after each draw
// and dispatch and after each marked runtime operation. Totals per frame are printed by full
// shader hash, largest first ("GpuProfile:" lines). At most 15 captures per launch
// (PS4RT_GPU_PROFILE_CAPTURES can lower it).
//
// Timestamps are written at the bottom of the pipe, so overlapping work is counted where it
// finishes. The time up to a draw's "before" mark is the work recorded since the previous mark
// ("pre", labelled with that draw's shader); time before a command buffer's first command is
// "gap" (mostly the GPU waiting for the submission).
//
// Results are read only after the GPU completed the capture's submissions, and the query pool is
// reset on the host (needs hostQueryReset; the profiler stays off without it) only after a later
// submission completed. The logic without Vulkan is shim/ps4rt_gpu_profile_core.h.
//
// Without PS4RT_DIAGNOSTICS (player builds) everything here is an inline no-op.
#pragma once

#include <span>
#include "common/types.h"
#include "ps4rt_diag.h"
#include "ps4rt_gpu_profile_core.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace Vulkan::GpuProfile {

using Kind = Ps4RtGpuProfile::Kind;
using Op = Ps4RtGpuProfile::Op;
using enum Ps4RtGpuProfile::Kind;
using enum Ps4RtGpuProfile::Op;
inline constexpr u32 NoSlot = ~0u;

#if PS4RT_DIAGNOSTICS
[[nodiscard]] bool Enabled();
/// Creates the query pool (first call; no-op when disabled or unsupported).
void Init(const Instance& instance);

/// Holds the right to take query slots while marks are recorded: a capture only ends once no
/// Scope is alive, so no recorded command can refer to a slot of a finished capture. Keep it
/// alive from Slot() until the commands using the slots were passed to Scheduler::Record().
class Scope {
public:
    Scope();
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    /// A query index for a mark of `kind` with the full shader `hash`, or NoSlot when no capture
    /// is running. Written by Write().
    [[nodiscard]] u32 Slot(Kind kind, u64 hash = 0, Op op = CopyBuffer, u64 detail = 0);

private:
    bool active;
};

/// In the Record() closure: writes the timestamp for `slot` (nothing for NoSlot).
void Write(vk::CommandBuffer cmdbuf, u32 slot);
/// Once per guest flip, in order with the draws (Rasterizer::OnGuestFlip): starts and ends
/// captures, records the frame mark, reads a completed capture back and prints it.
void OnFrame(Scheduler& scheduler);
/// When `scheduler` begins a command buffer: a "gap" mark first thing in it.
void OnCommandBuffer(const Scheduler& scheduler, vk::CommandBuffer cmdbuf);
/// After recording `op` on `scheduler` (the one passed to OnFrame): a mark, so the time since
/// the previous mark is that operation's. `detail` tells marks of one operation apart.
void Mark(Scheduler& scheduler, Op op, u64 detail = 0);
/// A barrier's detail for Mark(): the image barrier count, the first three layout transitions,
/// and whether it has a memory barrier.
[[nodiscard]] u64 BarrierDetail(std::span<const vk::ImageMemoryBarrier2> images, bool memory);
#else
[[nodiscard]] inline bool Enabled() {
    return false;
}
inline void Init(const Instance&) {}
class Scope {
public:
    [[nodiscard]] u32 Slot(Kind, u64 = 0, Op = CopyBuffer, u64 = 0) {
        return NoSlot;
    }
};
inline void Write(vk::CommandBuffer, u32) {}
inline void OnFrame(Scheduler&) {}
inline void OnCommandBuffer(const Scheduler&, vk::CommandBuffer) {}
inline void Mark(Scheduler&, Op, u64 = 0) {}
[[nodiscard]] inline u64 BarrierDetail(std::span<const vk::ImageMemoryBarrier2>, bool) {
    return 0;
}
#endif

/// An image operation's detail for Mark(): size, format and region count.
[[nodiscard]] inline u64 ImageDetail(u32 width, u32 height, vk::Format format, size_t regions) {
    return Ps4RtGpuProfile::ImageDetail(width, height, u32(format), regions);
}

} // namespace Vulkan::GpuProfile
