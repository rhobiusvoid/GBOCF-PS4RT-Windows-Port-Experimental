// SPDX-License-Identifier: GPL-2.0-or-later
// Code Fairy (CUSA28763) camera constants and pass identification.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <span>

namespace Vulkan::Ps4RtCamera {
struct Camera {
    std::array<float, 12> view{}, inverse_view{};
    std::array<float, 4> projection{};
};

/// Code Fairy camera constants: a 48-float buffer read by the geometry stage. Floats 0-15 are the
/// view-projection (clip rows, translation in the fourth column), 16-31 the row-vector view matrix
/// (looks down -z), 32-47 the projection (standard depth: ndc z = 1 + zo / distance, near 0.2,
/// infinite far). The result uses +z forward like the motion shader expects.
inline bool Read(std::span<const float> data, Camera& result) {
    if (data.size() != 48) return false;
    for (float value : data) if (!std::isfinite(value)) return false;
    const float* m = data.data() + 16;
    const float* p = data.data() + 32;
    // Perspective block: diagonal scales, w = -z, z scale/offset; everything else zero.
    const float zs = -p[10], zo = p[14];
    if (!(p[0] > 0.05f && p[0] < 100.f && p[5] > 0.05f && p[5] < 100.f)) return false;
    if (std::abs(p[11] + 1.f) > 1e-4f || !(zs > 0.99f && zs < 1.1f) || !(zo < -0.001f)) return false;
    for (unsigned k : {1u,2u,3u,4u,6u,7u,8u,9u,12u,13u,15u})
        if (std::abs(p[k]) > 1e-5f) return false;
    // Rigid view: orthonormal rotation, last column (0,0,0,1).
    if (std::abs(m[3]) > 1e-5f || std::abs(m[7]) > 1e-5f || std::abs(m[11]) > 1e-5f ||
        std::abs(m[15] - 1.f) > 1e-5f) return false;
    const auto rot = [&](unsigned r, unsigned c) { return double(m[r*4+c]); };
    for (unsigned a = 0; a < 3; ++a)
        for (unsigned b = a; b < 3; ++b) {
            double dot = 0;
            for (unsigned k = 0; k < 3; ++k) dot += rot(a,k)*rot(b,k);
            if (std::abs(dot - (a == b ? 1.0 : 0.0)) > 0.003) return false;
        }
    // The view-projection must agree with view * projection (rejects shadow and other cameras).
    // Clip w row = -view z.
    for (unsigned k = 0; k < 3; ++k)
        if (std::abs(data[12+k] + m[k*4+2]) > 0.01f) return false;
    if (std::abs(data[15] + m[14]) > 0.01f*std::max(1.f, std::abs(m[14]))) return false;
    Camera camera;
    for (unsigned k = 0; k < 3; ++k) {
        camera.view[k]   = m[k*4+0];
        camera.view[4+k] = m[k*4+1];
        camera.view[8+k] = -m[k*4+2];
    }
    camera.view[3] = m[12];
    camera.view[7] = m[13];
    camera.view[11] = -m[14];
    for (unsigned r = 0; r < 3; ++r) {
        for (unsigned c = 0; c < 3; ++c) camera.inverse_view[r*4+c] = camera.view[c*4+r];
        for (unsigned c = 0; c < 3; ++c)
            camera.inverse_view[r*4+3] -= camera.view[c*4+r]*camera.view[c*4+3];
    }
    camera.projection = {p[0], p[5], zs, zo};
    for (float value : camera.view) if (!std::isfinite(value)) return false;
    for (float value : camera.inverse_view) if (!std::isfinite(value)) return false;
    result = camera;
    return true;
}

constexpr bool SceneCopy(unsigned long long vs, unsigned long long ps) {
    return vs == 0x8d0e6514 && ps == 0x0c9956c3;
}
/// The pass that writes the last LDR scene colour (FSR's input), after tone mapping ps 74019ab4:
/// fe020947 normally; d205d67c in views with the distance effect (followed by ps dc21aff9
/// passes into the same target), which otherwise left FSR without input and the jitter showing.
constexpr bool Tonemap(unsigned long long vs, unsigned long long ps) {
    return vs == 0x48a2c099 && (ps == 0xfe020947 || ps == 0xd205d67c);
}
} // namespace Vulkan::Ps4RtCamera
