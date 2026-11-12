// SPDX-License-Identifier: GPL-2.0-or-later
// Runtime profiler interface. Capture/aggregation code lives under tools/diagnostics/native.
#pragma once
#ifndef PS4RT_DIAGNOSTICS
#define PS4RT_DIAGNOSTICS 0
#endif
#if PS4RT_DIAGNOSTICS
#include "ps4rt_gpu_profile_core_impl.h"
#else
#include <algorithm>
#include <cstddef>
#include <cstdint>
namespace Ps4RtGpuProfile {
enum Kind : std::uint8_t { Other = 0, Draw = 1, Dispatch = 2, Frame = 3, Submit = 4, Operation = 5 };
enum Op : std::uint8_t {
    CopyBuffer, FillBuffer, InlineData, UploadImage, CopyAliasImage, DownloadImage, CopyImage,
    CopyImageWithBuffer, CopyMip, CopyDepthStencil, ResolveImage, ClearImage, Barrier,
    PassBegin, PassEnd, Detile, Tile, NumOps
};
inline std::uint64_t ImageDetail(std::uint32_t width, std::uint32_t height, std::uint32_t format,
                                 std::size_t regions) {
    return std::uint64_t(width & 0xffff) | (std::uint64_t(height & 0xffff) << 16) |
           (std::uint64_t(format & 0xffff) << 32) |
           (std::uint64_t(std::min<std::size_t>(regions, 15)) << 48);
}
} // namespace Ps4RtGpuProfile
#endif
