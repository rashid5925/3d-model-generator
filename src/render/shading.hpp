// CPU twin of shaders/mesh.frag. Keep the two in sync.
#pragma once

#include <cmath>

#include "math.hpp"
#include "render/renderer.hpp"

namespace sf {

inline float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
inline float linearToSrgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

inline Vec3 shadeFragment(const FrameUniforms& u, Vec3 worldPos, Vec3 normal, Vec3 color) {
    if (dot(normal, normal) < 1e-4f) return color;  // unlit (grid lines)
    Vec3 n = normalize(normal);
    Vec3 forward{u.forward[0], u.forward[1], u.forward[2]};
    Vec3 eye{u.eye[0], u.eye[1], u.eye[2]};
    Vec3 v = u.eye[3] > 0.5f ? -forward : normalize(eye - worldPos);
    if (dot(n, v) < 0) n = -n;  // two-sided lighting
    Vec3 key{u.keyDir[0], u.keyDir[1], u.keyDir[2]};
    Vec3 fill{u.fillDir[0], u.fillDir[1], u.fillDir[2]};
    Vec3 base{srgbToLinear(color.x), srgbToLinear(color.y), srgbToLinear(color.z)};
    float diffuse = std::max(dot(n, key), 0.0f) * 0.78f + std::max(dot(n, fill), 0.0f) * 0.22f;
    float hemi = 0.5f + 0.5f * n.y;
    float ambient = 0.10f + 0.10f * hemi;
    Vec3 h = normalize(key + v);
    float spec = 0.12f * std::pow(std::max(dot(n, h), 0.0f), 40.0f);
    float facing = 0.78f + 0.22f * std::max(dot(n, v), 0.0f);
    Vec3 lin = base * ((ambient + diffuse) * facing) + Vec3{spec, spec, spec};
    return {linearToSrgb(lin.x), linearToSrgb(lin.y), linearToSrgb(lin.z)};
}

}  // namespace sf
