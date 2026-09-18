// Manifold -- affine transforms, and the planes and boxes they move.
#pragma once

#include "basis.h"

namespace mf {

// Basis plus translation. This is the type a portal warp is, and almost
// everything the engine does with space is one of these composed with
// another.
struct Transform3D {
    Basis basis;
    Vec3 origin;

    constexpr Transform3D() = default;
    Transform3D(const Basis &b, const Vec3 &o) : basis(b), origin(o) {}
    explicit Transform3D(const Vec3 &o) : origin(o) {}
    Transform3D(const Quat &q, const Vec3 &o) : basis(q.to_basis()), origin(o) {}

    static Transform3D identity() { return {}; }
    static Transform3D translation(const Vec3 &o) { return Transform3D(o); }
    static Transform3D rotation(const Vec3 &axis, float angle) {
        return {Basis::from_axis_angle(axis, angle), Vec3()};
    }
    static Transform3D scaling(float s) { return {Basis::uniform(s), Vec3()}; }
    // Place something at `eye` facing `target`.
    static Transform3D looking_at(const Vec3 &eye, const Vec3 &target,
                                  const Vec3 &up = Vec3::up()) {
        return {Basis::looking_at((target - eye).normalized(), up), eye};
    }

    Vec3 xform(const Vec3 &v) const { return basis.xform(v) + origin; }
    Vec3 operator*(const Vec3 &v) const { return xform(v); }
    // A direction is not moved by the translation, but it IS scaled by
    // the basis -- which is the whole point when a portal resizes what
    // goes through it.
    Vec3 xform_dir(const Vec3 &v) const { return basis.xform(v); }

    Transform3D operator*(const Transform3D &o) const {
        return {basis * o.basis, xform(o.origin)};
    }

    Transform3D inverse() const {
        Basis bi = basis.inverse();
        return {bi, -bi.xform(origin)};
    }
    // Cheaper, and wrong unless the basis really is a rotation.
    Transform3D inverse_orthonormal() const {
        Basis bt = basis.transposed();
        return {bt, -bt.xform(origin)};
    }

    Transform3D orthonormalized() const {
        return {basis.orthonormalized(), origin};
    }
    Transform3D translated(const Vec3 &d) const { return {basis, origin + d}; }
    Transform3D rotated(const Vec3 &axis, float angle) const {
        return Transform3D(Basis::from_axis_angle(axis, angle), Vec3()) * *this;
    }

    Vec3 forward() const { return basis.forward(); }
    Vec3 up() const { return basis.y(); }
    Vec3 right() const { return basis.x(); }
};

inline Transform3D lerp(const Transform3D &a, const Transform3D &b, float t) {
    return {Basis(slerp(a.basis.to_quat(), b.basis.to_quat(), t)) *
                lerp(a.basis.uniform_scale(), b.basis.uniform_scale(), t),
            lerp(a.origin, b.origin, t)};
}

// ax + by + cz = d, with `normal` unit. Positive distance is in front.
struct Plane {
    Vec3 normal{0, 1, 0};
    float d = 0;

    constexpr Plane() = default;
    Plane(const Vec3 &n, float d_) : normal(n), d(d_) {}
    Plane(const Vec3 &n, const Vec3 &point) : normal(n), d(dot(n, point)) {}
    // Counter-clockwise winding gives an outward normal.
    Plane(const Vec3 &a, const Vec3 &b, const Vec3 &c) {
        normal = cross(c - a, b - a).normalized();
        d = dot(normal, a);
    }

    float distance_to(const Vec3 &p) const { return dot(normal, p) - d; }
    bool is_above(const Vec3 &p) const { return distance_to(p) > 0.0f; }
    Vec3 project(const Vec3 &p) const { return p - normal * distance_to(p); }
    Plane flipped() const { return {-normal, -d}; }
    Vec3 center() const { return normal * d; }

    // Where a segment crosses, as a fraction along it, or -1.
    float segment_t(const Vec3 &from, const Vec3 &to) const {
        float a = distance_to(from), b = distance_to(to);
        if ((a > 0.0f) == (b > 0.0f)) return -1.0f;
        float den = a - b;
        return std::fabs(den) < EPS ? -1.0f : a / den;
    }
    bool intersects_ray(const Vec3 &origin, const Vec3 &dir, float *out_t) const {
        float den = dot(normal, dir);
        if (std::fabs(den) < EPS) return false;
        float t = (d - dot(normal, origin)) / den;
        if (out_t) *out_t = t;
        return t >= 0.0f;
    }

    // Move a plane by a transform. A plane transforms by the INVERSE
    // TRANSPOSE, not by the transform: under a non-uniform scale the
    // normal tilts the other way from the surface. Getting this wrong
    // is invisible until something is squashed, and then the oblique
    // clip plane misses the portal by a hand's width.
    Plane transformed(const Transform3D &t) const {
        Vec3 point = t.xform(normal * d);
        Vec3 n = t.basis.inverse().transposed().xform(normal).normalized();
        return {n, point};
    }
    // The same, for a transform known to be rotation + translation.
    Plane transformed_orthonormal(const Transform3D &t) const {
        Vec3 n = t.basis.xform(normal);
        return {n, t.xform(normal * d)};
    }
    // As (a, b, c, d') with the sign convention `dot(plane, (p,1)) >= 0`
    // means visible -- which is what the oblique projection wants.
    Vec4 as_vec4() const { return {normal.x, normal.y, normal.z, -d}; }
};

struct AABB {
    Vec3 min{INF, INF, INF};
    Vec3 max{-INF, -INF, -INF};

    constexpr AABB() = default;
    AABB(const Vec3 &mn, const Vec3 &mx) : min(mn), max(mx) {}

    bool valid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
    Vec3 size() const { return valid() ? max - min : Vec3(); }
    Vec3 center() const { return (min + max) * 0.5f; }
    Vec3 extents() const { return size() * 0.5f; }
    float radius() const { return extents().length(); }
    float longest_axis() const { return size().max_axis_value(); }

    void expand(const Vec3 &p) { min = vmin(min, p); max = vmax(max, p); }
    void expand(const AABB &o) {
        if (!o.valid()) return;
        min = vmin(min, o.min);
        max = vmax(max, o.max);
    }
    AABB grown(float m) const { return {min - Vec3(m), max + Vec3(m)}; }

    bool contains(const Vec3 &p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y &&
               p.z >= min.z && p.z <= max.z;
    }
    bool intersects(const AABB &o) const {
        return min.x <= o.max.x && max.x >= o.min.x && min.y <= o.max.y &&
               max.y >= o.min.y && min.z <= o.max.z && max.z >= o.min.z;
    }

    // The point inside the box nearest to `p` -- p itself when it is
    // already inside. The sphere test everything from light culling to
    // a BVH descent is built on.
    Vec3 closest_point(const Vec3 &p) const {
        return {clampf(p.x, min.x, max.x), clampf(p.y, min.y, max.y),
                clampf(p.z, min.z, max.z)};
    }
    float distance_squared_to(const Vec3 &p) const {
        return (closest_point(p) - p).length_sq();
    }
    bool intersects_sphere(const Vec3 &centre, float radius) const {
        return distance_squared_to(centre) <= radius * radius;
    }

    Vec3 corner(int i) const {
        return {(i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y,
                (i & 4) ? max.z : min.z};
    }
    // The corner furthest along `n`, for plane-versus-box tests.
    Vec3 support(const Vec3 &n) const {
        return {n.x >= 0 ? max.x : min.x, n.y >= 0 ? max.y : min.y,
                n.z >= 0 ? max.z : min.z};
    }

    AABB transformed(const Transform3D &t) const {
        if (!valid()) return {};
        AABB r;
        for (int i = 0; i < 8; i++) r.expand(t.xform(corner(i)));
        return r;
    }

    // Slab test. `dir` need not be normalised; t is in its units.
    bool intersects_ray(const Vec3 &origin, const Vec3 &dir, float *out_tmin) const {
        float t0 = 0.0f, t1 = INF;
        for (int i = 0; i < 3; i++) {
            if (std::fabs(dir[i]) < EPS) {
                if (origin[i] < min[i] || origin[i] > max[i]) return false;
                continue;
            }
            float inv = 1.0f / dir[i];
            float a = (min[i] - origin[i]) * inv;
            float b = (max[i] - origin[i]) * inv;
            if (a > b) { float s = a; a = b; b = s; }
            if (a > t0) t0 = a;
            if (b < t1) t1 = b;
            if (t0 > t1) return false;
        }
        if (out_tmin) *out_tmin = t0;
        return true;
    }
};

struct Rect2 {
    Vec2 position, size;
    constexpr Rect2() = default;
    Rect2(const Vec2 &p, const Vec2 &s) : position(p), size(s) {}
    Rect2(float x, float y, float w, float h) : position(x, y), size(w, h) {}
    Vec2 end() const { return position + size; }
    Vec2 center() const { return position + size * 0.5f; }
    float area() const { return size.x * size.y; }
    bool has_point(const Vec2 &p) const {
        return p.x >= position.x && p.y >= position.y && p.x <= position.x + size.x &&
               p.y <= position.y + size.y;
    }
    bool intersects(const Rect2 &o) const {
        return position.x < o.end().x && end().x > o.position.x &&
               position.y < o.end().y && end().y > o.position.y;
    }
};

}  // namespace mf
