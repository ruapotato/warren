#include "physics/manifold.h"

#include <algorithm>
#include <cmath>

namespace wr {
namespace {

constexpr float kEps = 1e-6f;

// A polygon being clipped. Eight is the most a box face can become
// after being cut by four planes, with room to spare.
struct Poly {
    Vec3 v[12];
    uint32_t id[12];
    int n = 0;
    void add(const Vec3 &p, uint32_t i) {
        if (n < 12) {
            v[n] = p;
            id[n] = i;
            n++;
        }
    }
};

// Sutherland-Hodgman against one half-space: keep what is behind
// the plane, and cut the edges that cross it.
//
// THE ID OF A CUT POINT IS THE ID OF THE EDGE IT CAME FROM, not a
// fresh number. A point that appears where a face crosses a plane is
// the same contact next step as long as the same edge crosses the
// same plane, and giving it a new id every step would throw away the
// warm start at exactly the contacts that need it -- the ones round
// the rim of an overlap.
void clip(Poly &p, const Vec3 &normal, float offset, uint32_t plane_id) {
    Poly out;
    for (int i = 0; i < p.n; i++) {
        const int j = (i + 1) % p.n;
        const float di = dot(normal, p.v[i]) - offset;
        const float dj = dot(normal, p.v[j]) - offset;
        if (di <= 0.0f) out.add(p.v[i], p.id[i]);
        if ((di < 0.0f) != (dj < 0.0f)) {
            const float t = di / (di - dj);
            // THE PLANE AND THE EDGE, not just the plane. A quad cut
            // by one plane produces two points, and giving both the
            // plane's id makes them indistinguishable -- so the warm
            // start hands the same stored impulse to both and the
            // manifold's own duplicate check can discard a real
            // contact. Two points a step, wrong, round the rim of
            // every partial overlap.
            out.add(p.v[i] + (p.v[j] - p.v[i]) * t,
                    (plane_id << 16) | (p.id[i] & 0xFFFFu));
        }
    }
    p = out;
}

// The eight corners of a box, in world space.
void box_corners(const Shape &s, const Transform3D &t, Vec3 out[8]) {
    const Vec3 h = s.half_extents;
    int k = 0;
    for (int i = 0; i < 8; i++) {
        const Vec3 local(((i & 1) ? h.x : -h.x), ((i & 2) ? h.y : -h.y),
                         ((i & 4) ? h.z : -h.z));
        out[k++] = t.xform(local);
    }
}

// The face of a box most nearly facing `dir`, as a polygon, with an
// id per vertex. `axis` is which of the three the face belongs to
// and `sign` which end, so the ids are stable.
void box_face(const Shape &s, const Transform3D &t, const Vec3 &dir, Poly *out,
              int *out_axis, float *out_sign, uint32_t tag) {
    int best = 0;
    float best_d = -1e30f, best_sign = 1.0f;
    for (int i = 0; i < 3; i++) {
        const Vec3 a = t.basis.col[i].normalized();
        const float d = dot(a, dir);
        if (std::fabs(d) > best_d) {
            best_d = std::fabs(d);
            best = i;
            best_sign = d < 0.0f ? -1.0f : 1.0f;
        }
    }
    *out_axis = best;
    *out_sign = best_sign;

    const int u = (best + 1) % 3, v = (best + 2) % 3;
    const Vec3 h = s.half_extents;
    const float hn = (&h.x)[best], hu = (&h.x)[u], hv = (&h.x)[v];
    const Vec3 an = t.basis.col[best].normalized() * (hn * best_sign);
    const Vec3 au = t.basis.col[u].normalized() * hu;
    const Vec3 av = t.basis.col[v].normalized() * hv;
    const Vec3 c = t.origin + an;

    out->n = 0;
    // Wound consistently, so the clip below sees a convex loop.
    const int corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int i = 0; i < 4; i++) {
        out->add(c + au * float(corner[i][0]) + av * float(corner[i][1]),
                 tag | uint32_t(best << 4) | uint32_t(i));
    }
}

// Project a box onto an axis: the half-width of its shadow.
float box_extent(const Shape &s, const Transform3D &t, const Vec3 &axis) {
    return std::fabs(dot(t.basis.col[0], axis)) * s.half_extents.x +
           std::fabs(dot(t.basis.col[1], axis)) * s.half_extents.y +
           std::fabs(dot(t.basis.col[2], axis)) * s.half_extents.z;
}

// ---------------------------------------------------------- primitives

bool sphere_sphere(const Shape &a, const Transform3D &ta, const Shape &b,
                   const Transform3D &tb, Manifold *out, float margin) {
    Vec3 d = ta.origin - tb.origin;
    const float len = d.length();
    const float sum = a.radius + b.radius;
    if (len > sum + margin) return false;
    out->normal = len > kEps ? d * (1.0f / len) : Vec3::up();
    out->clear();
    out->add(tb.origin + out->normal * b.radius, sum - len, 1u);
    return out->count > 0;
}

// The closest point on a box to `p`, in world space, plus whether p
// was inside it.
Vec3 closest_on_box(const Shape &s, const Transform3D &t, const Vec3 &p,
                    bool *inside) {
    const Transform3D inv = t.inverse();
    Vec3 l = inv.xform(p);
    const Vec3 h = s.half_extents;
    Vec3 c(clampf(l.x, -h.x, h.x), clampf(l.y, -h.y, h.y),
           clampf(l.z, -h.z, h.z));
    *inside = (c - l).length_sq() < kEps * kEps;
    return t.xform(c);
}

bool sphere_box(const Shape &sph, const Transform3D &tsph, const Shape &box,
                const Transform3D &tbox, Manifold *out, float margin,
                bool flip) {
    bool inside = false;
    const Vec3 on = closest_on_box(box, tbox, tsph.origin, &inside);
    Vec3 d = tsph.origin - on;
    float len = d.length();
    Vec3 n;
    float depth;
    if (inside) {
        // Centre inside the box: escape along the shallowest face,
        // which is the only direction that does not push it further
        // in. Measured in local space and rotated back out.
        const Transform3D inv = tbox.inverse();
        const Vec3 l = inv.xform(tsph.origin);
        const Vec3 h = box.half_extents;
        const float dx = h.x - std::fabs(l.x);
        const float dy = h.y - std::fabs(l.y);
        const float dz = h.z - std::fabs(l.z);
        Vec3 ln;
        float best;
        if (dx <= dy && dx <= dz) {
            ln = Vec3(l.x < 0.0f ? -1.0f : 1.0f, 0, 0);
            best = dx;
        } else if (dy <= dz) {
            ln = Vec3(0, l.y < 0.0f ? -1.0f : 1.0f, 0);
            best = dy;
        } else {
            ln = Vec3(0, 0, l.z < 0.0f ? -1.0f : 1.0f);
            best = dz;
        }
        n = tbox.basis.xform(ln).normalized();
        depth = best + sph.radius;
    } else {
        if (len > sph.radius + margin) return false;
        n = len > kEps ? d * (1.0f / len) : Vec3::up();
        depth = sph.radius - len;
    }
    out->clear();
    out->normal = flip ? -n : n;
    out->add(on, depth, 2u);
    return true;
}

// A capsule is a segment with a radius, so every capsule case is the
// sphere case done at the closest point on that segment -- plus a
// second contact when the segment lies along the surface, which is
// what stops a capsule lying on the floor from rolling.
void capsule_segment(const Shape &s, const Transform3D &t, Vec3 *p0, Vec3 *p1) {
    const Vec3 up = t.basis.col[1].normalized();
    const float half = s.height * 0.5f;
    *p0 = t.origin - up * half;
    *p1 = t.origin + up * half;
}

// --------------------------------------------------------- box vs box

bool box_box(const Shape &a, const Transform3D &ta, const Shape &b,
             const Transform3D &tb, Manifold *out, float margin) {
    // Fifteen axes: three faces each, and nine edge-edge crosses.
    Vec3 axes[15];
    int n = 0;
    Vec3 ax[3], bx[3];
    for (int i = 0; i < 3; i++) {
        ax[i] = ta.basis.col[i].normalized();
        bx[i] = tb.basis.col[i].normalized();
    }
    for (int i = 0; i < 3; i++) axes[n++] = ax[i];
    for (int i = 0; i < 3; i++) axes[n++] = bx[i];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            Vec3 c = cross(ax[i], bx[j]);
            // Parallel edges give a zero axis, which is not a
            // separating direction and normalises to a NaN.
            if (c.length_sq() > 1e-8f) axes[n++] = c.normalized();
        }

    const Vec3 between = ta.origin - tb.origin;
    float best_depth = 1e30f, best_score = 1e30f;
    Vec3 best_axis;
    int best_i = -1;
    for (int i = 0; i < n; i++) {
        Vec3 axis = axes[i];
        // Always pointing from B toward A, so the manifold normal
        // separates A from B without a sign hunt later.
        if (dot(axis, between) < 0.0f) axis = -axis;
        const float overlap = box_extent(a, ta, axis) + box_extent(b, tb, axis) -
                              std::fabs(dot(between, axis));
        if (overlap < -margin) return false;   // a separating axis
        // TIES HAVE TO BE BROKEN THE SAME WAY EVERY STEP.
        //
        // Two identical boxes stacked square give EXACTLY the same
        // overlap on A's up-axis and on B's, to the last bit. Which
        // one wins then depends on rounding, and it changes from
        // step to step -- and when it changes, the reference and
        // incident faces swap roles, so every contact id changes
        // with them. Warm starting then finds nothing, the solver
        // restarts from zero at a third of its contacts, and a
        // stack that should be asleep shifts and mutters for ever.
        //
        // The symptom is a contact count that flickers -- twenty,
        // seventeen, twenty -- which is why it is worth counting.
        //
        // So: A's faces win ties, B's faces have to be clearly
        // better, and an edge-edge axis has to be better still. A
        // couple of millimetres is far below anything visible and
        // far above the rounding that was deciding it.
        const float penalty = (i < 3) ? 0.0f : (i < 6 ? 0.002f : 0.004f);
        if (overlap + penalty < best_score) {
            best_score = overlap + penalty;
            best_depth = overlap;
            best_axis = axis;
            best_i = i;
        }
    }
    if (best_i < 0) return false;

    out->clear();
    out->normal = best_axis;

    if (best_i >= 6) {
        // EDGE AGAINST EDGE IS ONE POINT, and trying to clip a face
        // pair here produces a manifold that is flat in the wrong
        // plane -- a box balanced on the corner of another would be
        // held as if it were lying on it.
        const int ia = (best_i - 6) / 3, ib = (best_i - 6) % 3;
        // The extreme edge of each box along the axis.
        auto edge_of = [&](const Shape &s, const Transform3D &t, const Vec3 x[3],
                           int skip, const Vec3 &dir, Vec3 *e0, Vec3 *e1) {
            Vec3 c = t.origin;
            const Vec3 h = s.half_extents;
            for (int k = 0; k < 3; k++) {
                if (k == skip) continue;
                const float sgn = dot(x[k], dir) < 0.0f ? -1.0f : 1.0f;
                c = c + x[k] * ((&h.x)[k] * sgn);
            }
            const float half = (&h.x)[skip];
            *e0 = c - x[skip] * half;
            *e1 = c + x[skip] * half;
        };
        Vec3 a0, a1, b0, b1;
        edge_of(a, ta, ax, ia, -best_axis, &a0, &a1);
        edge_of(b, tb, bx, ib, best_axis, &b0, &b1);
        Vec3 ca, cb;
        closest_points_on_segments(a0, a1, b0, b1, &ca, &cb);
        out->add((ca + cb) * 0.5f, best_depth,
                 0x8000u | uint32_t(ia << 4) | uint32_t(ib));
        return true;
    }

    // FACE AGAINST FACE. The reference face is on whichever box owned
    // the winning axis; the incident face is the most anti-parallel
    // face of the other one.
    const bool ref_is_a = best_i < 3;
    const Shape &rs = ref_is_a ? a : b;
    const Transform3D &rt = ref_is_a ? ta : tb;
    const Shape &is_ = ref_is_a ? b : a;
    const Transform3D &it = ref_is_a ? tb : ta;
    const Vec3 ref_dir = ref_is_a ? -best_axis : best_axis;

    Poly ref_face, inc_face;
    int ref_axis = 0, inc_axis = 0;
    float ref_sign = 1.0f, inc_sign = 1.0f;
    box_face(rs, rt, ref_dir, &ref_face, &ref_axis, &ref_sign, 0x0000u);
    box_face(is_, it, -ref_dir, &inc_face, &inc_axis, &inc_sign, 0x0100u);

    // Clip the incident face against the reference face's four side
    // planes, then drop anything in front of the reference plane.
    const Vec3 rn = rt.basis.col[ref_axis].normalized() * ref_sign;
    for (int k = 1; k <= 2; k++) {
        const int axis = (ref_axis + k) % 3;
        const Vec3 side = rt.basis.col[axis].normalized();
        const float h = (&rs.half_extents.x)[axis];
        clip(inc_face, side, dot(side, rt.origin) + h, 0x0200u | uint32_t(axis));
        clip(inc_face, -side, -(dot(side, rt.origin) - h),
             0x0300u | uint32_t(axis));
    }
    const float plane_d = dot(rn, ref_face.v[0]);
    for (int i = 0; i < inc_face.n; i++) {
        const float sep = dot(rn, inc_face.v[i]) - plane_d;
        if (sep > margin) continue;
        out->add(inc_face.v[i] - rn * sep, -sep, inc_face.id[i]);
    }
    if (out->count == 0) {
        out->add(ta.origin - best_axis * box_extent(a, ta, best_axis),
                 best_depth, 0xFFFFu);
    }
    return true;
}

// --------------------------------------------------- shape vs triangle

// A TRIANGLE HAS NO INSIDE, which is the whole difficulty.
//
// Two boxes are both solid, so "least penetration" always names a
// direction that separates them. A triangle is a sheet: the least
// penetration between a resting crate and the floor it stands on is
// sideways, out of the edge of the triangle, and a solver handed
// that normal slides the crate along the ground for ever.
//
// So the triangle's own normal is preferred, and only rejected when
// the shape is genuinely off the side of it. That is the standard
// answer and it is why box-vs-triangle is not just box-vs-box with
// one box flattened.
bool box_triangle(const Shape &s, const Transform3D &t, const Vec3 tri[3],
                  Manifold *out, float margin) {
    const Vec3 e0 = tri[1] - tri[0], e1 = tri[2] - tri[1], e2 = tri[0] - tri[2];
    Vec3 tn = cross(e0, -e2);
    const float area = tn.length();
    if (area < 1e-9f) return false;       // degenerate
    tn = tn * (1.0f / area);

    Vec3 axes[13];
    int n = 0;
    axes[n++] = tn;
    Vec3 bx[3];
    for (int i = 0; i < 3; i++) {
        bx[i] = t.basis.col[i].normalized();
        axes[n++] = bx[i];
    }
    const Vec3 edges[3] = {e0, e1, e2};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            Vec3 c = cross(bx[i], edges[j]);
            if (c.length_sq() > 1e-8f) axes[n++] = c.normalized();
        }

    float best_depth = 1e30f;
    Vec3 best_axis;
    int best_i = -1;
    for (int i = 0; i < n; i++) {
        Vec3 axis = axes[i];
        // Toward the box, so the normal pushes the box off the
        // triangle.
        if (dot(axis, t.origin - tri[0]) < 0.0f) axis = -axis;
        float tmin = 1e30f, tmax = -1e30f;
        for (int k = 0; k < 3; k++) {
            const float d = dot(tri[k], axis);
            tmin = std::min(tmin, d);
            tmax = std::max(tmax, d);
        }
        const float c = dot(t.origin, axis);
        const float r = box_extent(s, t, axis);
        const float overlap = std::min(tmax - (c - r), (c + r) - tmin);
        if (overlap < -margin) return false;
        // THE TRIANGLE'S OWN NORMAL WINS TIES AND NEAR-TIES. A crate
        // flat on the floor has a face axis of the box that measures
        // the same as the triangle's normal to within rounding, and
        // whichever is picked by a hair decides whether the crate is
        // held up or shoved sideways.
        const float bias = (i == 0) ? 0.02f : 0.0f;
        if (overlap - bias < best_depth) {
            best_depth = overlap - bias;
            best_axis = axis;
            best_i = i;
        }
    }
    if (best_i < 0) return false;
    if (best_i == 0) best_depth += 0.02f;

    out->clear();
    out->normal = best_axis;

    // The box face facing the triangle, clipped to the triangle's
    // three edge planes and then to its plane.
    Poly inc;
    int inc_axis = 0;
    float inc_sign = 1.0f;
    box_face(s, t, -best_axis, &inc, &inc_axis, &inc_sign, 0x0400u);
    for (int k = 0; k < 3; k++) {
        const Vec3 side = cross(edges[k], tn).normalized();
        clip(inc, side, dot(side, tri[k]), 0x0500u | uint32_t(k));
    }
    const float plane_d = dot(best_axis, tri[0]);
    for (int i = 0; i < inc.n; i++) {
        const float sep = dot(best_axis, inc.v[i]) - plane_d;
        if (sep > margin) continue;
        out->add(inc.v[i] - best_axis * sep, -sep, inc.id[i]);
    }
    if (out->count == 0) {
        // Off the face: the deepest single point, which for an edge
        // or corner contact is the honest answer.
        Vec3 corners[8];
        box_corners(s, t, corners);
        int deepest = 0;
        float low = 1e30f;
        for (int i = 0; i < 8; i++) {
            const float d = dot(best_axis, corners[i]);
            if (d < low) {
                low = d;
                deepest = i;
            }
        }
        const float sep = low - plane_d;
        if (sep > margin) return false;
        out->add(corners[deepest], -sep, 0x0600u | uint32_t(deepest));
    }
    return out->count > 0;
}

bool sphere_triangle(float radius, const Vec3 &centre, const Vec3 tri[3],
                     Manifold *out, float margin, uint32_t tag) {
    const Vec3 on = closest_point_on_triangle(centre, tri[0], tri[1], tri[2]);
    Vec3 d = centre - on;
    const float len = d.length();
    if (len > radius + margin) return false;
    Vec3 n;
    if (len > kEps) {
        n = d * (1.0f / len);
    } else {
        n = cross(tri[1] - tri[0], tri[2] - tri[0]).normalized();
    }
    out->normal = n;
    out->add(on, radius - len, tag);
    return true;
}

}  // namespace

// ------------------------------------------------------------- Manifold

bool Manifold::add(const Vec3 &p, float depth, uint32_t id) {
    if (count >= 4) {
        // Keep the deepest four. A fifth point on a convex overlap is
        // inside the hull of the others and holds nothing extra.
        int shallow = 0;
        for (int i = 1; i < 4; i++)
            if (points[i].depth < points[shallow].depth) shallow = i;
        if (points[shallow].depth >= depth) return false;
        points[shallow] = Contact{p, depth, id, 0.0f, {0.0f, 0.0f}};
        return true;
    }
    points[count++] = Contact{p, depth, id, 0.0f, {0.0f, 0.0f}};
    return true;
}

// -------------------------------------------------------------- collide

bool collide(const Shape &a, const Transform3D &ta, const Shape &b,
             const Transform3D &tb, Manifold *out, float margin) {
    out->clear();
    const ShapeType A = a.type, B = b.type;
    if (A == ShapeType::Sphere && B == ShapeType::Sphere)
        return sphere_sphere(a, ta, b, tb, out, margin);
    if (A == ShapeType::Sphere && B == ShapeType::Box)
        return sphere_box(a, ta, b, tb, out, margin, false);
    if (A == ShapeType::Box && B == ShapeType::Sphere) {
        // Same test, and then the normal is flipped so it still
        // points from A out of B.
        Manifold m;
        if (!sphere_box(b, tb, a, ta, &m, margin, true)) return false;
        *out = m;
        return true;
    }
    if (A == ShapeType::Box && B == ShapeType::Box)
        return box_box(a, ta, b, tb, out, margin);

    // A capsule against anything: the sphere case at each end of its
    // segment, which gives the two contacts that stop it rolling.
    if (A == ShapeType::Capsule || B == ShapeType::Capsule) {
        const bool cap_is_a = A == ShapeType::Capsule;
        const Shape &cs = cap_is_a ? a : b;
        const Transform3D &ct = cap_is_a ? ta : tb;
        const Shape &os = cap_is_a ? b : a;
        const Transform3D &ot = cap_is_a ? tb : ta;
        if (os.type == ShapeType::Capsule) {
            Vec3 a0, a1, b0, b1;
            capsule_segment(a, ta, &a0, &a1);
            capsule_segment(b, tb, &b0, &b1);
            Vec3 ca, cb;
            closest_points_on_segments(a0, a1, b0, b1, &ca, &cb);
            Shape s1 = Shape::sphere(a.radius), s2 = Shape::sphere(b.radius);
            Transform3D t1, t2;
            t1.origin = ca;
            t2.origin = cb;
            return sphere_sphere(s1, t1, s2, t2, out, margin);
        }
        Vec3 p0, p1;
        capsule_segment(cs, ct, &p0, &p1);
        bool any = false;
        for (int e = 0; e < 2; e++) {
            const Vec3 p = e ? p1 : p0;
            Shape sph = Shape::sphere(cs.radius);
            Transform3D st;
            st.origin = p;
            Manifold m;
            if (os.type == ShapeType::Box
                    ? sphere_box(sph, st, os, ot, &m, margin, false)
                    : sphere_sphere(sph, st, os, ot, &m, margin)) {
                if (!any) out->normal = m.normal;
                out->add(m.points[0].position, m.points[0].depth,
                         0x0700u | uint32_t(e));
                any = true;
            }
        }
        if (any && !cap_is_a) out->normal = -out->normal;
        if (any && cap_is_a) { /* normal already from capsule out */ }
        return any;
    }
    return false;
}

bool collide_triangle(const Shape &s, const Transform3D &t, const Vec3 tri[3],
                      Manifold *out, float margin) {
    out->clear();
    switch (s.type) {
        case ShapeType::Box:
            return box_triangle(s, t, tri, out, margin);
        case ShapeType::Sphere:
            return sphere_triangle(s.radius, t.origin, tri, out, margin, 0x0800u);
        case ShapeType::Capsule: {
            Vec3 p0, p1;
            capsule_segment(s, t, &p0, &p1);
            // Both ends and the middle: three samples along the
            // segment is enough to hold a capsule flat on a floor
            // without pretending a capsule-triangle clip exists.
            bool any = false;
            const Vec3 pts[3] = {p0, (p0 + p1) * 0.5f, p1};
            for (int i = 0; i < 3; i++) {
                Manifold m;
                if (!sphere_triangle(s.radius, pts[i], tri, &m, margin,
                                     0x0900u | uint32_t(i)))
                    continue;
                if (!any) out->normal = m.normal;
                out->add(m.points[0].position, m.points[0].depth,
                         0x0900u | uint32_t(i));
                any = true;
            }
            return any;
        }
        default:
            return false;
    }
}

// ------------------------------------------------------------- inertia

Basis inertia_of(const Shape &s, float mass) {
    Basis b;
    float ix = 0, iy = 0, iz = 0;
    switch (s.type) {
        case ShapeType::Box: {
            const Vec3 d = s.half_extents * 2.0f;
            const float k = mass / 12.0f;
            ix = k * (d.y * d.y + d.z * d.z);
            iy = k * (d.x * d.x + d.z * d.z);
            iz = k * (d.x * d.x + d.y * d.y);
            break;
        }
        case ShapeType::Sphere: {
            ix = iy = iz = 0.4f * mass * s.radius * s.radius;
            break;
        }
        case ShapeType::Capsule: {
            // Cylinder plus two hemispheres, about the centre. The
            // approximation everyone uses -- the caps' own inertia
            // and the parallel-axis term for their offset.
            const float r = s.radius, h = s.height;
            const float cyl_v = 3.14159265f * r * r * h;
            const float cap_v = 4.0f / 3.0f * 3.14159265f * r * r * r;
            const float total = cyl_v + cap_v;
            const float cm = total > kEps ? mass * cyl_v / total : mass;
            const float sm = mass - cm;
            iy = 0.5f * cm * r * r + 0.4f * sm * r * r;
            ix = cm * (h * h / 12.0f + r * r * 0.25f) +
                 sm * (0.4f * r * r + h * h * 0.25f + 0.375f * r * h);
            iz = ix;
            break;
        }
        default:
            // A mesh is never dynamic here, and an inertia of zero
            // would divide by zero the moment one was. One is a
            // harmless stand-in that behaves like a small solid.
            ix = iy = iz = mass;
            break;
    }
    b.col[0] = Vec3(ix, 0, 0);
    b.col[1] = Vec3(0, iy, 0);
    b.col[2] = Vec3(0, 0, iz);
    return b;
}

}  // namespace wr
