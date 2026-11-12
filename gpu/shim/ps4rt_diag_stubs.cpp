// SPDX-License-Identifier: GPL-2.0-or-later
// Tiny no-op runtime bridge when the private developer toolkit is unavailable.
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#if !PS4RT_DIAGNOSTICS
namespace Vulkan {
void Rasterizer::RequestDrawList() {}
void Rasterizer::OnGuestFlip() {}
} // namespace Vulkan
#endif
