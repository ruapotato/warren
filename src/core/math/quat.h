// Warren -- rotations.
#pragma once

#include "vector.h"

namespace wr {

struct Basis;

// Unit quaternion, xyz + w. Composition is `a * b` = "b then a", the
// same order as matrices, so a rotation reads the same whichever of the
// two you happen to be holding.
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    constexpr Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_)
        : x(x_), y(y_), z(z_), w(w_) {}

    static Quat from_axis_angle(const Vec3 &axis, float angle) {
        Vec3 a = axis.normalized();
        float h = angle * 0.5f, s = std::sin(h);
        return {a.x * s, a.y * s, a.z * s, std::cos(h)};
    }
    // Yaw about +Y, then pitch about +X, then roll about -Z: the order a
    // first-person camera wants, so that pitch never rolls the horizon.
    static Quat from_euler_yxz(float yaw, float pitch, float roll = 0.0f);
    // The shortest rotation taking `from` to `to`, both unit.
    static Quat between(const Vec3 &from, const Vec3 &to);

    Quat operator*(const Quat &o) const {
        return {w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x,
                w * o.z + x * o.y - y * o.x + z * o.w,
                w * o.w - x * o.x - y * o.y - z * o.z};
    }
    Quat operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
    Quat operator+(const Quat &o) const {
        return {x + o.x, y + o.y, z + o.z, w + o.w};
    }
    Quat operator-() const { return {-x, -y, -z, -w}; }

    float length_sq() const { return x * x + y * y + z * z + w * w; }
    float length() const { return std::sqrt(length_sq()); }
    Quat normalized() const {
        float l = length();
        return l > EPS ? Quat{x / l, y / l, z / l, w / l} : Quat();
    }
    Quat conjugate() const { return {-x, -y, -z, w}; }
    // Unit quaternions only, which is all this engine makes.
    Quat inverse() const { return conjugate(); }

    Vec3 xform(const Vec3 &v) const {
        // v + 2w(q x v) + 2(q x (q x v)) -- two crosses, no matrix.
        Vec3 q{x, y, z};
        Vec3 t = cross(q, v) * 2.0f;
        return v + t * w + cross(q, t);
    }
    Vec3 operator*(const Vec3 &v) const { return xform(v); }

    float angle_to(const Quat &o) const {
        float d = x * o.x + y * o.y + z * o.z + w * o.w;
        return std::acos(clampf(2.0f * d * d - 1.0f, -1.0f, 1.0f));
    }
    Basis to_basis() const;
};

inline float dot(const Quat &a, const Quat &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

// Shortest-arc spherical interpolation; falls back to a straight line
// when the two are close enough that the difference cannot be seen.
inline Quat slerp(const Quat &a, Quat b, float t) {
    float d = dot(a, b);
    if (d < 0.0f) { b = -b; d = -d; }
    if (d > 0.9995f) return (a + (b + (-a)) * t).normalized();
    float th = std::acos(clampf(d, -1.0f, 1.0f));
    float st = std::sin(th);
    float wa = std::sin((1.0f - t) * th) / st;
    float wb = std::sin(t * th) / st;
    return (a * wa + b * wb).normalized();
}

}  // namespace wr
