// Small linear-algebra toolkit. Conventions:
//   * Right-handed, Y-up world. "Front" of a model faces +Z.
//   * Mat4 is column-major (m[col * 4 + row]) to match GLSL/Vulkan.
//   * Projection matrices target Vulkan clip space: depth in [0, 1], Y flipped
//     so that NDC y = -1 is the top row of the image.
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace sf {

constexpr float kPi = 3.14159265358979323846f;
inline float radians(float deg) { return deg * kPi / 180.0f; }

struct Vec2 {
    float x = 0, y = 0;
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    float& operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, Vec3 a) { return a * s; }
inline Vec3 operator*(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline Vec3 operator/(Vec3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
inline Vec3& operator+=(Vec3& a, Vec3 b) { a = a + b; return a; }
inline Vec3& operator-=(Vec3& a, Vec3 b) { a = a - b; return a; }
inline Vec3& operator*=(Vec3& a, float s) { a = a * s; return a; }

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) {
    float l = length(a);
    return l > 1e-20f ? a / l : Vec3{0, 0, 0};
}
inline Vec3 vmin(Vec3 a, Vec3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3 vmax(Vec3 a, Vec3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline Vec3 vabs(Vec3 a) { return {std::fabs(a.x), std::fabs(a.y), std::fabs(a.z)}; }
inline Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline float maxComponent(Vec3 a) { return std::max(a.x, std::max(a.y, a.z)); }
inline float minComponent(Vec3 a) { return std::min(a.x, std::min(a.y, a.z)); }

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
};

struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float& at(int row, int col) { return m[col * 4 + row]; }
    float at(int row, int col) const { return m[col * 4 + row]; }
    static Mat4 identity() { return Mat4{}; }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int rr = 0; rr < 4; ++rr) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.at(rr, k) * b.at(k, c);
            r.at(rr, c) = s;
        }
    return r;
}

inline Vec4 operator*(const Mat4& a, const Vec4& v) {
    return {a.m[0] * v.x + a.m[4] * v.y + a.m[8] * v.z + a.m[12] * v.w,
            a.m[1] * v.x + a.m[5] * v.y + a.m[9] * v.z + a.m[13] * v.w,
            a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z + a.m[14] * v.w,
            a.m[3] * v.x + a.m[7] * v.y + a.m[11] * v.z + a.m[15] * v.w};
}

inline Vec3 transformPoint(const Mat4& a, Vec3 p) {
    Vec4 r = a * Vec4{p.x, p.y, p.z, 1.0f};
    return {r.x, r.y, r.z};
}
inline Vec3 transformDir(const Mat4& a, Vec3 d) {
    Vec4 r = a * Vec4{d.x, d.y, d.z, 0.0f};
    return {r.x, r.y, r.z};
}

inline Mat4 translation(Vec3 t) {
    Mat4 r;
    r.at(0, 3) = t.x;
    r.at(1, 3) = t.y;
    r.at(2, 3) = t.z;
    return r;
}
inline Mat4 scaling(Vec3 s) {
    Mat4 r;
    r.at(0, 0) = s.x;
    r.at(1, 1) = s.y;
    r.at(2, 2) = s.z;
    return r;
}
inline Mat4 rotationX(float deg) {
    float c = std::cos(radians(deg)), s = std::sin(radians(deg));
    Mat4 r;
    r.at(1, 1) = c; r.at(1, 2) = -s;
    r.at(2, 1) = s; r.at(2, 2) = c;
    return r;
}
inline Mat4 rotationY(float deg) {
    float c = std::cos(radians(deg)), s = std::sin(radians(deg));
    Mat4 r;
    r.at(0, 0) = c; r.at(0, 2) = s;
    r.at(2, 0) = -s; r.at(2, 2) = c;
    return r;
}
inline Mat4 rotationZ(float deg) {
    float c = std::cos(radians(deg)), s = std::sin(radians(deg));
    Mat4 r;
    r.at(0, 0) = c; r.at(0, 1) = -s;
    r.at(1, 0) = s; r.at(1, 1) = c;
    return r;
}
// Euler angles in degrees, applied X first, then Y, then Z (extrinsic).
inline Mat4 rotationEuler(Vec3 deg) { return rotationZ(deg.z) * rotationY(deg.y) * rotationX(deg.x); }

// Inverse of rotationEuler: Euler degrees (X, then Y, then Z) of a pure rotation.
inline Vec3 eulerFromRotation(const Mat4& r) {
    float sb = std::clamp(-r.at(2, 0), -1.0f, 1.0f);
    float b = std::asin(sb);
    float a, c;
    if (std::fabs(std::cos(b)) > 1e-5f) {
        a = std::atan2(r.at(2, 1), r.at(2, 2));
        c = std::atan2(r.at(1, 0), r.at(0, 0));
    } else {  // gimbal lock: fold Z into X
        c = 0;
        a = sb > 0 ? std::atan2(r.at(0, 1), r.at(1, 1)) : std::atan2(-r.at(0, 1), r.at(1, 1));
    }
    return Vec3{a, b, c} * (180.0f / kPi);
}

inline float determinant3(const Mat4& a) {
    return a.at(0, 0) * (a.at(1, 1) * a.at(2, 2) - a.at(1, 2) * a.at(2, 1)) -
           a.at(0, 1) * (a.at(1, 0) * a.at(2, 2) - a.at(1, 2) * a.at(2, 0)) +
           a.at(0, 2) * (a.at(1, 0) * a.at(2, 1) - a.at(1, 1) * a.at(2, 0));
}

inline Mat4 transpose(const Mat4& a) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.at(i, j) = a.at(j, i);
    return r;
}

// General 4x4 inverse (cofactor expansion). Returns identity if singular.
inline Mat4 inverse(const Mat4& mat) {
    const float* m = mat.m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    Mat4 r;
    if (std::fabs(det) < 1e-30f) return r;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) r.m[i] = inv[i] * det;
    return r;
}

// View matrix for a camera at `eye` looking at `target` (camera looks down -Z).
inline Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 f = normalize(target - eye);
    Vec3 s = normalize(cross(f, up));
    Vec3 u = cross(s, f);
    Mat4 r;
    r.at(0, 0) = s.x; r.at(0, 1) = s.y; r.at(0, 2) = s.z; r.at(0, 3) = -dot(s, eye);
    r.at(1, 0) = u.x; r.at(1, 1) = u.y; r.at(1, 2) = u.z; r.at(1, 3) = -dot(u, eye);
    r.at(2, 0) = -f.x; r.at(2, 1) = -f.y; r.at(2, 2) = -f.z; r.at(2, 3) = dot(f, eye);
    return r;
}

inline Mat4 perspective(float fovyRad, float aspect, float nearZ, float farZ) {
    float f = 1.0f / std::tan(fovyRad * 0.5f);
    Mat4 r;
    for (float& v : r.m) v = 0;
    r.at(0, 0) = f / aspect;
    r.at(1, 1) = -f;
    r.at(2, 2) = farZ / (nearZ - farZ);
    r.at(2, 3) = nearZ * farZ / (nearZ - farZ);
    r.at(3, 2) = -1.0f;
    return r;
}

inline Mat4 orthographic(float l, float r_, float b, float t, float n, float f) {
    Mat4 r;
    r.at(0, 0) = 2.0f / (r_ - l);
    r.at(0, 3) = -(r_ + l) / (r_ - l);
    r.at(1, 1) = -2.0f / (t - b);
    r.at(1, 3) = (t + b) / (t - b);
    r.at(2, 2) = -1.0f / (f - n);
    r.at(2, 3) = -n / (f - n);
    return r;
}

struct AABB {
    Vec3 min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 max{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};

    bool valid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
    void expand(Vec3 p) {
        min = vmin(min, p);
        max = vmax(max, p);
    }
    void expand(const AABB& b) {
        if (!b.valid()) return;
        expand(b.min);
        expand(b.max);
    }
    Vec3 size() const { return valid() ? max - min : Vec3{}; }
    Vec3 center() const { return valid() ? (min + max) * 0.5f : Vec3{}; }
    Vec3 corner(int i) const { return {(i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y, (i & 4) ? max.z : min.z}; }
    AABB transformed(const Mat4& m) const {
        AABB r;
        if (!valid()) return r;
        for (int i = 0; i < 8; ++i) r.expand(transformPoint(m, corner(i)));
        return r;
    }
    AABB padded(float d) const {
        AABB r = *this;
        r.min -= Vec3{d, d, d};
        r.max += Vec3{d, d, d};
        return r;
    }
};

}  // namespace sf
