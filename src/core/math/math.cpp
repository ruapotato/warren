#include "basis.h"
#include "transform.h"

namespace mf {

Basis Quat::to_basis() const {
    float xx = x * x, yy = y * y, zz = z * z;
    float xy = x * y, xz = x * z, yz = y * z;
    float wx = w * x, wy = w * y, wz = w * z;
    return {{1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy)},
            {2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx)},
            {2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy)}};
}

// Yaw about +Y, then pitch about +X, then roll about -Z. Applied to a
// vector the rightmost happens first, so the composition is Ry*Rx*Rz:
// roll is in the camera's own frame, pitch is in the yawed frame, and
// yaw is in the world. That is the order that keeps a first-person
// horizon level however far the head is tilted.
Quat Quat::from_euler_yxz(float yaw, float pitch, float roll) {
    return from_axis_angle(Vec3::up(), yaw) *
           from_axis_angle(Vec3::right(), pitch) *
           from_axis_angle(Vec3::back(), roll);
}

Quat Quat::between(const Vec3 &from, const Vec3 &to) {
    Vec3 a = from.normalized(), b = to.normalized();
    float d = dot(a, b);
    if (d >= 1.0f - 1e-6f) return Quat();
    if (d <= -1.0f + 1e-6f) {
        // Opposed: any perpendicular axis is a valid half turn.
        Vec3 axis = any_perpendicular(a);
        return Quat(axis.x, axis.y, axis.z, 0.0f);
    }
    Vec3 c = cross(a, b);
    float s = std::sqrt((1.0f + d) * 2.0f);
    return Quat(c.x / s, c.y / s, c.z / s, s * 0.5f).normalized();
}

// Shepperd's method: pick the largest of the four to divide by, so the
// square root is never taken of something near zero.
Quat Basis::to_quat() const {
    Basis b = orthonormalized();
    float m00 = b.col[0].x, m10 = b.col[0].y, m20 = b.col[0].z;
    float m01 = b.col[1].x, m11 = b.col[1].y, m21 = b.col[1].z;
    float m02 = b.col[2].x, m12 = b.col[2].y, m22 = b.col[2].z;
    float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        float s = std::sqrt(tr + 1.0f) * 2.0f;
        return {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    }
    if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        return {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    }
    if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        return {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    }
    float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
    return {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
}

Vec3 Basis::to_euler_yxz() const {
    Basis b = orthonormalized();
    // The rotated -Z gives yaw and pitch directly; roll is what is left
    // once those two are undone.
    float m12 = b.col[2].y;
    float pitch = std::asin(clampf(-m12, -1.0f, 1.0f));
    float yaw, roll;
    if (std::fabs(m12) < 0.9999f) {
        yaw = std::atan2(b.col[2].x, b.col[2].z);
        roll = std::atan2(b.col[0].y, b.col[1].y);
    } else {
        // Straight up or straight down: yaw and roll are the same turn,
        // so all of it is given to yaw.
        yaw = std::atan2(-b.col[1].x, b.col[0].x);
        roll = 0.0f;
    }
    return {yaw, pitch, roll};
}

Basis Basis::looking_at(const Vec3 &dir, const Vec3 &up) {
    Vec3 f = dir.normalized();
    if (f.length_sq() < EPS) return Basis();
    Vec3 u = up.normalized();
    // Looking straight along `up` leaves the cross product undefined;
    // any perpendicular will do and the choice is never seen, because
    // the roll it picks is about the axis being looked along.
    if (std::fabs(dot(f, u)) > 0.9999f) u = any_perpendicular(f);
    Vec3 z = -f;
    Vec3 x = cross(u, z).normalized();
    Vec3 y = cross(z, x);
    return {x, y, z};
}

}  // namespace mf
