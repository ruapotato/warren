// Manifold -- 3x3, as three columns.
#pragma once

#include "quat.h"

namespace mf {

// A linear map, stored as the three vectors the unit axes land on. That
// is the useful reading of a basis nine times out of ten -- `col[1]` is
// which way up is -- and it makes `xform` three multiply-adds with no
// index arithmetic.
//
// Non-uniform scale and shear are representable and the engine does not
// forbid them, but a portal warp only ever produces rotation plus
// UNIFORM scale, and the traversal code checks that.
struct Basis {
    Vec3 col[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    constexpr Basis() = default;
    constexpr Basis(const Vec3 &x, const Vec3 &y, const Vec3 &z)
        : col{x, y, z} {}
    explicit Basis(const Quat &q) { *this = q.to_basis(); }

    Vec3 &operator[](int i) { return col[i]; }
    const Vec3 &operator[](int i) const { return col[i]; }

    Vec3 x() const { return col[0]; }
    Vec3 y() const { return col[1]; }
    Vec3 z() const { return col[2]; }
    // A camera's forward is its own -Z.
    Vec3 forward() const { return -col[2]; }

    // Row i, which is what you need to multiply by a row vector or to
    // read the inverse of a rotation.
    Vec3 row(int i) const { return {col[0][i], col[1][i], col[2][i]}; }

    static Basis identity() { return {}; }
    static Basis scaled(const Vec3 &s) {
        return {{s.x, 0, 0}, {0, s.y, 0}, {0, 0, s.z}};
    }
    static Basis uniform(float s) { return scaled(Vec3(s)); }
    static Basis from_axis_angle(const Vec3 &axis, float angle) {
        return Basis(Quat::from_axis_angle(axis, angle));
    }
    static Basis from_euler_yxz(float yaw, float pitch, float roll = 0.0f) {
        return Basis(Quat::from_euler_yxz(yaw, pitch, roll));
    }
    // Look along `dir` with `up` as the reference. The result's -Z is
    // `dir`, which is the convention a camera wants.
    static Basis looking_at(const Vec3 &dir, const Vec3 &up = Vec3::up());

    Vec3 xform(const Vec3 &v) const {
        return col[0] * v.x + col[1] * v.y + col[2] * v.z;
    }
    // The transpose applied, which for an orthonormal basis is the
    // inverse and for anything else is not. Named for what it does.
    Vec3 xform_transposed(const Vec3 &v) const {
        return {dot(col[0], v), dot(col[1], v), dot(col[2], v)};
    }
    Vec3 operator*(const Vec3 &v) const { return xform(v); }

    Basis operator*(const Basis &o) const {
        return {xform(o.col[0]), xform(o.col[1]), xform(o.col[2])};
    }
    Basis operator*(float s) const { return {col[0] * s, col[1] * s, col[2] * s}; }
    Basis operator+(const Basis &o) const {
        return {col[0] + o.col[0], col[1] + o.col[1], col[2] + o.col[2]};
    }

    Basis transposed() const { return {row(0), row(1), row(2)}; }

    float determinant() const { return dot(col[0], cross(col[1], col[2])); }

    Basis inverse() const {
        Vec3 r0 = cross(col[1], col[2]);
        Vec3 r1 = cross(col[2], col[0]);
        Vec3 r2 = cross(col[0], col[1]);
        float d = dot(col[0], r0);
        if (std::fabs(d) < 1e-12f) return Basis();
        float inv = 1.0f / d;
        // Rows of the adjugate become columns of the inverse.
        return Basis{{r0.x * inv, r1.x * inv, r2.x * inv},
                     {r0.y * inv, r1.y * inv, r2.y * inv},
                     {r0.z * inv, r1.z * inv, r2.z * inv}};
    }

    // The length each unit axis came out as.
    Vec3 scale() const {
        float s = determinant() < 0.0f ? -1.0f : 1.0f;
        return {col[0].length() * s, col[1].length(), col[2].length()};
    }
    // The one number a uniform scale is. Averaged rather than taken from
    // one axis so a little drift does not pick a winner.
    float uniform_scale() const {
        return (col[0].length() + col[1].length() + col[2].length()) / 3.0f;
    }
    bool is_uniform(float tol = 1e-3f) const {
        float a = col[0].length(), b = col[1].length(), c = col[2].length();
        float m = (a + b + c) / 3.0f;
        if (m < EPS) return false;
        return std::fabs(a - m) / m < tol && std::fabs(b - m) / m < tol &&
               std::fabs(c - m) / m < tol;
    }

    // Gram-Schmidt. Drops scale and shear and keeps the facing.
    Basis orthonormalized() const {
        Vec3 x = col[0].normalized();
        Vec3 y = (col[1] - x * dot(x, col[1])).normalized();
        Vec3 z = cross(x, y);
        if (determinant() < 0.0f) z = -z;
        return {x, y, z};
    }
    // Keep the facing, set the scale. The engine's portals do exactly
    // this to a body: it turns, and it changes size, and nothing else.
    Basis rescaled(float s) const { return orthonormalized() * s; }

    Quat to_quat() const;
    // Yaw, pitch, roll in the YXZ order the camera uses.
    Vec3 to_euler_yxz() const;
};

inline Basis operator*(float s, const Basis &b) { return b * s; }

inline Basis lerp(const Basis &a, const Basis &b, float t) {
    return {lerp(a.col[0], b.col[0], t), lerp(a.col[1], b.col[1], t),
            lerp(a.col[2], b.col[2], t)};
}

}  // namespace mf
