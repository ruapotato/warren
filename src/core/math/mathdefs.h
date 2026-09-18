// Warren -- shared numeric ground.
//
// CONVENTIONS, STATED ONCE, TRUE EVERYWHERE.
//
//   Handedness      right-handed. +X right, +Y up, -Z forward. A camera
//                   looks down its own -Z.
//   Matrices        column-major storage, column-vector convention:
//                   `v' = M * v`, and `m[c][r]` is column c, row r. This
//                   is GLSL's layout, so a matrix uploads as its own
//                   bytes with no transpose anywhere.
//   Clip space      ONE convention for every backend: x and y in
//                   [-1, 1] with +Y UP, and depth in [0, 1] REVERSED --
//                   the near plane maps to 1 and the far plane to 0.
//
//                   Vulkan has [0, 1] depth natively and a Y that points
//                   down, which a negative-height viewport turns back up.
//                   OpenGL is put into the same space with
//                   glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE). So one
//                   projection matrix drives both, with no flips, no
//                   per-backend #ifdef in a shader, and no chance of the
//                   two renderers disagreeing about which way is up.
//
//                   Depth is reversed because floating point has its
//                   precision bunched near zero while a perspective
//                   divide bunches depth near the near plane; putting
//                   the far plane at zero lines those two up and buys
//                   roughly a hundredfold in precision. A kilometre of
//                   terrain stops z-fighting. The cost is one line:
//                   the depth test is GREATER and the clear is 0.
//   Angles          radians.
//   Winding         counter-clockwise is front-facing.
#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace wr {

inline constexpr float PI = 3.14159265358979323846f;
inline constexpr float TAU = PI * 2.0f;
inline constexpr float DEG = PI / 180.0f;
inline constexpr float RAD = 180.0f / PI;
inline constexpr float EPS = 1e-6f;
inline constexpr float INF = std::numeric_limits<float>::infinity();

inline float deg2rad(float d) { return d * DEG; }
inline float rad2deg(float r) { return r * RAD; }

inline float sign(float v) { return v < 0.0f ? -1.0f : 1.0f; }
inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
inline float saturate(float v) { return clampf(v, 0.0f, 1.0f); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float inv_lerp(float a, float b, float v) {
    return (b - a) == 0.0f ? 0.0f : (v - a) / (b - a);
}
inline float smoothstep(float a, float b, float v) {
    float t = saturate(inv_lerp(a, b, v));
    return t * t * (3.0f - 2.0f * t);
}
inline bool nearly(float a, float b, float eps = 1e-5f) {
    return std::fabs(a - b) <= eps;
}
inline float move_toward(float from, float to, float step) {
    float d = to - from;
    return std::fabs(d) <= step ? to : from + sign(d) * step;
}
// The shortest signed angular distance from `from` to `to`.
inline float angle_delta(float from, float to) {
    float d = std::fmod(to - from + PI, TAU);
    if (d < 0.0f) d += TAU;
    return d - PI;
}
inline float lerp_angle(float a, float b, float t) {
    return a + angle_delta(a, b) * t;
}

}  // namespace wr
