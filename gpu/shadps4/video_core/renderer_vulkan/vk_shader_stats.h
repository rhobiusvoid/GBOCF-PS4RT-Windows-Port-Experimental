// SPDX-License-Identifier: GPL-2.0-or-later
// ps4rt diagnostics: PS4RT_SHADER_STATS=<hex hash> prints the driver's statistics for that
// compute shader (VK_KHR_pipeline_executable_properties: registers, spills, scratch, waves per
// SIMD; "ShaderStats:" lines) and writes its internal representations (ISA) to
// <shader dir>/wave_dumps/stats_<hash>.txt. The extension is only enabled with
// PS4RT_WAVE_STATS=1; without it (or without the device feature) nothing is queried. At most 8
// pipelines are dumped per launch, each file at most 16 MiB (driver buffers come out of the same
// allowance; long representations are cut and marked).
//
// Compute only: graphics pipelines go through the pipeline binary cache, whose own capture
// flags have not been validated together with statistics capture.
//
// Without PS4RT_DIAGNOSTICS (player builds) everything here is an inline no-op.
#pragma once

#include "common/types.h"
#include "ps4rt_diag.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace Vulkan::ShaderStats {

#if PS4RT_DIAGNOSTICS
/// Creation flags for a compute pipeline of shader `hash`: the capture flags when it is the
/// requested shader and pipeline executable info is enabled, else none.
[[nodiscard]] vk::PipelineCreateFlags CaptureFlags(const Instance& instance, u64 hash);
/// After creating that pipeline: prints and writes its statistics (same conditions).
void Dump(const Instance& instance, vk::Pipeline pipeline, u64 hash);
#else
[[nodiscard]] inline vk::PipelineCreateFlags CaptureFlags(const Instance&, u64) {
    return {};
}
inline void Dump(const Instance&, vk::Pipeline, u64) {}
#endif

} // namespace Vulkan::ShaderStats
