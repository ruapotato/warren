// Manifold -- vectors.
#pragma once

#include "mathdefs.h"

namespace mf {

struct Vec2 {
    float x = 0, y = 0;
    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
    explicit constexpr Vec2(float s) : x(s), y(s) {}

    float &operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }

    Vec2 operator-() const { return {-x, -y}; }
    Vec2 operator+(const Vec2 &o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2 &o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
    Vec2 operator/(float s) const { return {x / s, y / s}; }
    Vec2 operator*(const Vec2 &o) const { return {x * o.x, y * o.y}; }
    Vec2 operator/(const Vec2 &o) const { return {x / o.x, y / o.y}; }
    Vec2 &operator+=(const Vec2 &o) { x += o.x; y += o.y; return *this; }
    Vec2 &operator-=(const Vec2 &o) { x -= o.x; y -= o.y; return *this; }
    Vec2 &operator*=(float s) { x *= s; y *= s; return *this; }
    Vec2 &operator/=(float s) { x /= s; y /= s; return *this; }
    bool operator==(const Vec2 &o) const { return x == o.x && y == o.y; }
    bool operator!=(const Vec2 &o) const { return !(*this == o); }

    float length_sq() const { return x * x + y * y; }
    float length() const { return std::sqrt(length_sq()); }
    float aspect() const { return y == 0.0f ? 0.0f : x / y; }
    Vec2 normalized() const {
        float l = length();
        return l > EPS ? *this / l : Vec2();
    }
    Vec2 rotated(float a) const {
        float c = std::cos(a), s = std::sin(a);
        return {x * c - y * s, x * s + y * c};
    }
};

inline Vec2 operator*(float s, const Vec2 &v) { return v * s; }
inline float dot(const Vec2 &a, const Vec2 &b) { return a.x * b.x + a.y * b.y; }
inline float cross(const Vec2 &a, const Vec2 &b) { return a.x * b.y - a.y * b.x; }
inline Vec2 lerp(const Vec2 &a, const Vec2 &b, float t) { return a + (b - a) * t; }
inline Vec2 vmin(const Vec2 &a, const Vec2 &b) {
    return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y};
}
inline Vec2 vmax(const Vec2 &a, const Vec2 &b) {
    return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y};
}

struct Vec3 {
    float x = 0, y = 0, z = 0;
    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    explicit constexpr Vec3(float s) : x(s), y(s), z(s) {}
    constexpr Vec3(const Vec2 &v, float z_) : x(v.x), y(v.y), z(z_) {}

    float &operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
    Vec2 xy() const { return {x, y}; }

    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator+(const Vec3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3 operator*(const Vec3 &o) const { return {x * o.x, y * o.y, z * o.z}; }
    Vec3 operator/(const Vec3 &o) const { return {x / o.x, y / o.y, z / o.z}; }
    Vec3 &operator+=(const Vec3 &o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3 &operator-=(const Vec3 &o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3 &operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    Vec3 &operator*=(const Vec3 &o) { x *= o.x; y *= o.y; z *= o.z; return *this; }
    Vec3 &operator/=(float s) { x /= s; y /= s; z /= s; return *this; }
    bool operator==(const Vec3 &o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Vec3 &o) const { return !(*this == o); }

    float length_sq() const { return x * x + y * y + z * z; }
    float length() const { return std::sqrt(length_sq()); }
    Vec3 normalized() const {
        float l = length();
        return l > EPS ? *this / l : Vec3();
    }
    bool is_finite() const {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }
    Vec3 abs() const { return {std::fabs(x), std::fabs(y), std::fabs(z)}; }
    float max_axis_value() const { return x > y ? (x > z ? x : z) : (y > z ? y : z); }
    int max_axis() const { return x > y ? (x > z ? 0 : 2) : (y > z ? 1 : 2); }
    int min_axis() const { return x < y ? (x < z ? 0 : 2) : (y < z ? 1 : 2); }

    static constexpr Vec3 zero() { return {0, 0, 0}; }
    static constexpr Vec3 one() { return {1, 1, 1}; }
    static constexpr Vec3 up() { return {0, 1, 0}; }
    static constexpr Vec3 down() { return {0, -1, 0}; }
    static constexpr Vec3 right() { return {1, 0, 0}; }
    static constexpr Vec3 left() { return {-1, 0, 0}; }
    static constexpr Vec3 forward() { return {0, 0, -1}; }
    static constexpr Vec3 back() { return {0, 0, 1}; }
};

inline Vec3 operator*(float s, const Vec3 &v) { return v * s; }
inline float dot(const Vec3 &a, const Vec3 &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 cross(const Vec3 &a, const Vec3 &b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec3 lerp(const Vec3 &a, const Vec3 &b, float t) { return a + (b - a) * t; }
inline float distance(const Vec3 &a, const Vec3 &b) { return (a - b).length(); }
inline float distance_sq(const Vec3 &a, const Vec3 &b) { return (a - b).length_sq(); }
inline Vec3 vmin(const Vec3 &a, const Vec3 &b) {
    return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
}
inline Vec3 vmax(const Vec3 &a, const Vec3 &b) {
    return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
}
inline Vec3 reflect(const Vec3 &v, const Vec3 &n) { return v - n * (2.0f * dot(v, n)); }
// The part of `v` that survives sliding along a surface with normal `n`.
inline Vec3 slide(const Vec3 &v, const Vec3 &n) { return v - n * dot(v, n); }
inline Vec3 project(const Vec3 &v, const Vec3 &onto) {
    float l = onto.length_sq();
    return l > EPS ? onto * (dot(v, onto) / l) : Vec3();
}
// Any unit vector at right angles to `v`. Chosen off the smallest
// component so the cross product never collapses.
inline Vec3 any_perpendicular(const Vec3 &v) {
    Vec3 a = v.abs();
    Vec3 axis = a.x < a.y ? (a.x < a.z ? Vec3::right() : Vec3::back())
                          : (a.y < a.z ? Vec3::up() : Vec3::back());
    return cross(v, axis).normalized();
}
inline Vec3 move_toward(const Vec3 &from, const Vec3 &to, float step) {
    Vec3 d = to - from;
    float l = d.length();
    return (l <= step || l < EPS) ? to : from + d / l * step;
}

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    constexpr Vec4() = default;
    constexpr Vec4(float x_, float y_, float z_, float w_)
        : x(x_), y(y_), z(z_), w(w_) {}
    constexpr Vec4(const Vec3 &v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    explicit constexpr Vec4(float s) : x(s), y(s), z(s), w(s) {}

    float &operator[](int i) { return (&x)[i]; }
    float operator[](int i) const { return (&x)[i]; }
    Vec3 xyz() const { return {x, y, z}; }
    Vec2 xy() const { return {x, y}; }

    Vec4 operator-() const { return {-x, -y, -z, -w}; }
    Vec4 operator+(const Vec4 &o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    Vec4 operator-(const Vec4 &o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
    Vec4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
    Vec4 operator/(float s) const { return {x / s, y / s, z / s, w / s}; }
    bool operator==(const Vec4 &o) const {
        return x == o.x && y == o.y && z == o.z && w == o.w;
    }
    // The perspective divide, guarded.
    Vec3 homogenized() const { return std::fabs(w) > EPS ? xyz() / w : xyz(); }
};

inline Vec4 operator*(float s, const Vec4 &v) { return v * s; }
inline float dot(const Vec4 &a, const Vec4 &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}
inline Vec4 lerp(const Vec4 &a, const Vec4 &b, float t) { return a + (b - a) * t; }

// Linear RGBA. Everything in the renderer is linear; sRGB happens once,
// at the very end, and only there.
struct Color {
    float r = 0, g = 0, b = 0, a = 1;
    constexpr Color() = default;
    constexpr Color(float r_, float g_, float b_, float a_ = 1.0f)
        : r(r_), g(g_), b(b_), a(a_) {}

    float &operator[](int i) { return (&r)[i]; }
    float operator[](int i) const { return (&r)[i]; }
    Vec3 rgb() const { return {r, g, b}; }
    Vec4 rgba() const { return {r, g, b, a}; }

    Color operator*(float s) const { return {r * s, g * s, b * s, a}; }
    Color operator*(const Color &o) const {
        return {r * o.r, g * o.g, b * o.b, a * o.a};
    }
    Color operator+(const Color &o) const {
        return {r + o.r, g + o.g, b + o.b, a + o.a};
    }

    static Color from_srgb(float r, float g, float b, float a = 1.0f) {
        auto to_linear = [](float c) {
            return c <= 0.04045f ? c / 12.92f
                                 : std::pow((c + 0.055f) / 1.055f, 2.4f);
        };
        return {to_linear(r), to_linear(g), to_linear(b), a};
    }
    static Color hex(uint32_t rgb, float a = 1.0f) {
        return from_srgb(float((rgb >> 16) & 0xFF) / 255.0f,
                         float((rgb >> 8) & 0xFF) / 255.0f,
                         float(rgb & 0xFF) / 255.0f, a);
    }
    static constexpr Color white() { return {1, 1, 1, 1}; }
    static constexpr Color black() { return {0, 0, 0, 1}; }
    static constexpr Color clear() { return {0, 0, 0, 0}; }
};

inline Color lerp(const Color &a, const Color &b, float t) {
    return {lerp(a.r, b.r, t), lerp(a.g, b.g, t), lerp(a.b, b.b, t),
            lerp(a.a, b.a, t)};
}

struct Vec2i {
    int x = 0, y = 0;
    constexpr Vec2i() = default;
    constexpr Vec2i(int x_, int y_) : x(x_), y(y_) {}
    bool operator==(const Vec2i &o) const { return x == o.x && y == o.y; }
    bool operator!=(const Vec2i &o) const { return !(*this == o); }
    Vec2 to_vec2() const { return {float(x), float(y)}; }
    float aspect() const { return y == 0 ? 0.0f : float(x) / float(y); }
};

}  // namespace mf
