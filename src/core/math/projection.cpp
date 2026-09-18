#include "projection.h"

namespace wr {

// ----------------------------------------------------------- constructors

Projection Projection::frustum(float left, float right, float bottom, float top,
                               float z_near, float z_far) {
    Projection p = Projection::zero();
    float rl = right - left, tb = top - bottom, fn = z_far - z_near;
    if (std::fabs(rl) < EPS || std::fabs(tb) < EPS || std::fabs(fn) < EPS)
        return Projection::identity();
    p.m[0][0] = 2.0f * z_near / rl;
    p.m[1][1] = 2.0f * z_near / tb;
    p.m[2][0] = (right + left) / rl;
    p.m[2][1] = (top + bottom) / tb;
    // REVERSED: near -> 1, far -> 0. The non-reversed form would be
    // z_far/(z_near - z_far) here; swapping the two planes is the whole
    // of the change, and it is worth about two decimal digits of depth.
    p.m[2][2] = z_near / fn;
    p.m[2][3] = -1.0f;
    p.m[3][2] = z_near * z_far / fn;
    return p;
}

Projection Projection::perspective(float fov_y, float aspect, float z_near,
                                   float z_far) {
    float t = z_near * std::tan(fov_y * 0.5f);
    float r = t * aspect;
    return frustum(-r, r, -t, t, z_near, z_far);
}

// The limit of the above as z_far goes to infinity. Nothing degenerates
// -- the depth row simply becomes (0, 0, 0, z_near) -- and the result
// has strictly better precision than any finite far plane, because the
// only thing setting the scale is the near plane.
Projection Projection::perspective_infinite(float fov_y, float aspect,
                                            float z_near) {
    float t = z_near * std::tan(fov_y * 0.5f);
    float r = t * aspect;
    Projection p = Projection::zero();
    p.m[0][0] = z_near / r;
    p.m[1][1] = z_near / t;
    p.m[2][2] = 0.0f;
    p.m[2][3] = -1.0f;
    p.m[3][2] = z_near;
    return p;
}

Projection Projection::perspective_offset(float fov_y, float aspect,
                                          float z_near, float z_far,
                                          const Vec2 &offset) {
    float t = z_near * std::tan(fov_y * 0.5f);
    float r = t * aspect;
    float ox = offset.x * r, oy = offset.y * t;
    return frustum(-r + ox, r + ox, -t + oy, t + oy, z_near, z_far);
}

Projection Projection::orthographic(float left, float right, float bottom,
                                    float top, float z_near, float z_far) {
    Projection p = Projection::zero();
    float rl = right - left, tb = top - bottom, fn = z_far - z_near;
    if (std::fabs(rl) < EPS || std::fabs(tb) < EPS || std::fabs(fn) < EPS)
        return Projection::identity();
    p.m[0][0] = 2.0f / rl;
    p.m[1][1] = 2.0f / tb;
    // Reversed here too, so that one depth test and one clear value
    // serve every camera in the engine, shadow cascades included.
    p.m[2][2] = 1.0f / fn;
    p.m[3][0] = -(right + left) / rl;
    p.m[3][1] = -(top + bottom) / tb;
    p.m[3][2] = z_far / fn;
    p.m[3][3] = 1.0f;
    return p;
}

// ------------------------------------------------------- the oblique near
//
// THE DERIVATION, BECAUSE THE FOUR LINES BELOW LOOK LIKE MAGIC.
//
// The near plane of a projection is exactly the set of points that map
// to z_ndc = -1, and that set is determined by the third row of the
// matrix: a point p is on it when dot(row2, (p,1)) = -dot(row3, (p,1)).
// So replacing row2 replaces the near plane and nothing else -- the x
// and y rows are untouched, so the frustum's shape, its field of view
// and its off-axis offset all survive.
//
// We want the new near plane to be `C`. Setting row2 = C - row3 would do
// it, up to scale, but the scale is not free: the FAR plane is where
// z_ndc = +1, and it moves with any change to row2. Lengyel's insight is
// to choose the scale so that the far plane passes through the one
// corner of the original frustum that the new near plane leaves behind
// -- the corner diagonally opposite C's tilt -- which keeps the depth
// range as wide as it can be given the constraint.
//
// q is that corner, in view space, found by pushing the clip-space point
// (sgn(C.x), sgn(C.y), 1, 1) back through the projection. The scale then
// falls out as 2 / dot(C, q), and the "+1" on m[2][2] is row3's
// contribution (row3 = (0,0,-1,0) for any perspective projection).
//
// The cost is that depth is no longer distributed the way it was: near
// the tilted plane the resolution is worse than a perpendicular near
// plane would give. In exchange the wall behind a portal is gone
// exactly, at every angle, with no bias to tune and nothing traded away.

Projection Projection::with_oblique_near(const Vec4 &clip_plane) const {
    Projection p = *this;
    const Vec4 &C = clip_plane;

    // A degenerate plane would fill the matrix with infinities and the
    // screen with nothing, and nothing in the log would say why.
    if (Vec3(C.x, C.y, C.z).length_sq() < EPS) return p;

    // Row 3 -- the one the perspective divide uses. Read, not assumed,
    // so that an off-axis, an already-oblique or an orthographic
    // projection all work.
    const Vec4 r3(p.m[0][3], p.m[1][3], p.m[2][3], p.m[3][3]);

    // The corner of the ORIGINAL far plane diagonally opposite the way
    // C tilts, in homogeneous view space. Left un-divided on purpose:
    // the constraint below is scale-invariant, and for an infinite far
    // plane this comes back as a direction with w = 0, which is exactly
    // right and would be a division by zero if normalised.
    const Projection inv = p.inverse();
    const Vec4 q = inv.xform(Vec4(sign(C.x), sign(C.y), 0.0f, 1.0f));

    const float den = dot(C, q);
    if (std::fabs(den) < EPS) return p;
    const float k = -dot(r3, q) / den;
    // k must come out negative, or the half-space that survives is the
    // wrong one and the portal would show the wall instead of the room.
    if (!(k < 0.0f) || !std::isfinite(k)) return p;

    const Vec4 r2 = r3 + C * k;
    p.m[0][2] = r2.x;
    p.m[1][2] = r2.y;
    p.m[2][2] = r2.z;
    p.m[3][2] = r2.w;
    return p;
}

// -------------------------------------------------------------- reading

void Projection::get_extents_at(float z, float *left, float *right,
                                float *bottom, float *top) const {
    // SOLVED FROM ROWS 0, 1 AND W -- NOT BY INVERTING THE MATRIX.
    //
    // Inverting and unprojecting an NDC corner is the obvious way and
    // it is wrong here, because the inverse mixes in the third row --
    // the one with_oblique_near replaces. A portal view has the same
    // frustum as the camera it was warped from, with a tilted near
    // clip; asking that projection how wide it is at 40 metres must
    // give the camera's answer, or a shadow cascade fitted to it
    // lurches every time the player's angle to the portal changes.
    //
    // The x row says   ndc_x * w = m00*x + m20*(-z) + m30
    // and the w row    w         = m23*(-z) + m33
    // (the engine never builds a projection with m03 or m13 set), so
    //     x = (ndc_x * w - m30 + m20*z) / m00
    // which reads only rows 0, 1 and w, all of which the oblique cut
    // leaves alone. It covers perspective, off-axis and orthographic
    // without a special case: for an orthographic one m23 is 0, so w
    // is 1 and the answer does not depend on z, which is exactly
    // right.
    const float w = -m[2][3] * z + m[3][3];
    const float mx = std::fabs(m[0][0]) > EPS ? m[0][0] : 1.0f;
    const float my = std::fabs(m[1][1]) > EPS ? m[1][1] : 1.0f;
    auto x_at = [&](float ndc) { return (ndc * w - m[3][0] + m[2][0] * z) / mx; };
    auto y_at = [&](float ndc) { return (ndc * w - m[3][1] + m[2][1] * z) / my; };
    if (left) *left = x_at(-1.0f);
    if (right) *right = x_at(1.0f);
    if (bottom) *bottom = y_at(-1.0f);
    if (top) *top = y_at(1.0f);
}

void Projection::slice_corners(float z_near, float z_far, Vec3 out[8]) const {
    const float z[2] = {z_near, z_far};
    for (int i = 0; i < 2; i++) {
        float l, r, b, t;
        get_extents_at(z[i], &l, &r, &b, &t);
        // View space looks down -Z, so a distance of z is at -z.
        const float depth = -z[i];
        out[i * 4 + 0] = Vec3(l, b, depth);
        out[i * 4 + 1] = Vec3(r, b, depth);
        out[i * 4 + 2] = Vec3(r, t, depth);
        out[i * 4 + 3] = Vec3(l, t, depth);
    }
}

float Projection::get_aspect() const {
    if (std::fabs(m[0][0]) < EPS) return 1.0f;
    return m[1][1] / m[0][0];
}

float Projection::get_fov_y() const {
    if (is_orthographic() || std::fabs(m[1][1]) < EPS) return 0.0f;
    return 2.0f * std::atan(1.0f / m[1][1]);
}

// Reverse-Z: z_ndc is 1 at the near plane and 0 at the far one, so
// these are the inverses of the constructors above, not of the usual
// forms.
float Projection::get_z_near() const {
    if (is_orthographic()) {
        if (std::fabs(m[2][2]) < EPS) return 0.0f;
        return (m[3][2] - 1.0f) / m[2][2];
    }
    float den = 1.0f + m[2][2];
    return std::fabs(den) < EPS ? 0.0f : m[3][2] / den;
}

float Projection::get_z_far() const {
    if (is_orthographic()) {
        if (std::fabs(m[2][2]) < EPS) return INF;
        return m[3][2] / m[2][2];
    }
    // A depth row of zero is the infinite-far projection.
    if (std::fabs(m[2][2]) < EPS) return INF;
    return m[3][2] / m[2][2];
}

bool Projection::is_infinite_far() const {
    return !is_orthographic() && std::fabs(m[2][2]) < EPS;
}

// --------------------------------------------------------------- algebra

Projection Projection::operator*(const Projection &o) const {
    Projection r = Projection::zero();
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++) s += m[k][row] * o.m[c][k];
            r.m[c][row] = s;
        }
    return r;
}

Vec4 Projection::xform(const Vec4 &v) const {
    return {m[0][0] * v.x + m[1][0] * v.y + m[2][0] * v.z + m[3][0] * v.w,
            m[0][1] * v.x + m[1][1] * v.y + m[2][1] * v.z + m[3][1] * v.w,
            m[0][2] * v.x + m[1][2] * v.y + m[2][2] * v.z + m[3][2] * v.w,
            m[0][3] * v.x + m[1][3] * v.y + m[2][3] * v.z + m[3][3] * v.w};
}

Projection Projection::transposed() const {
    Projection r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) r.m[c][row] = m[row][c];
    return r;
}

// Cofactor expansion. A projection is not always invertible in the
// general sense but every one this engine builds is, oblique included.
Projection Projection::inverse() const {
    const float *a = &m[0][0];
    float inv[16];

    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] +
             a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] -
             a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] +
             a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] -
              a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] -
             a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] +
             a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] -
             a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] +
              a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] +
             a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] -
             a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] +
              a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] -
              a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] -
             a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] +
             a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] -
              a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] +
              a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];

    float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (std::fabs(det) < 1e-20f) return Projection::identity();
    det = 1.0f / det;

    Projection r;
    float *o = &r.m[0][0];
    for (int i = 0; i < 16; i++) o[i] = inv[i] * det;
    return r;
}

void Projection::ray_at(const Vec2 &ndc, Vec3 *origin, Vec3 *dir) const {
    Projection inv = inverse();
    Vec3 near_p = inv.xform(Vec4(ndc.x, ndc.y, -1.0f, 1.0f)).homogenized();
    Vec3 far_p = inv.xform(Vec4(ndc.x, ndc.y, 1.0f, 1.0f)).homogenized();
    if (origin) *origin = near_p;
    if (dir) *dir = (far_p - near_p).normalized();
}

// Gribb & Hartmann: the rows of the view-projection matrix ARE the
// frustum planes, because clip space is defined by |x| <= w and so on,
// and each of those inequalities is one row combined with the w row.
// Works for any projection -- oblique included, which is the point,
// since the culling has to agree with what the hardware will clip.
void Projection::frustum_planes(const Transform3D &camera, Plane out[6]) const {
    Projection vp = *this * to_projection(camera.inverse_orthonormal());

    auto row = [&](int r) {
        return Vec4(vp.m[0][r], vp.m[1][r], vp.m[2][r], vp.m[3][r]);
    };
    Vec4 rw = row(3);
    // x and y clip against |x| <= w as usual; DEPTH clips against
    // 0 <= z <= w, not -w <= z <= w, so the near and far rows are
    // row(2) and rw - row(2) rather than a symmetric pair. Getting this
    // wrong culls everything or nothing.
    Vec4 rows[6] = {rw + row(0), rw - row(0), rw + row(1),
                    rw - row(1), row(2), rw - row(2)};
    for (int i = 0; i < 6; i++) {
        Vec3 n(rows[i].x, rows[i].y, rows[i].z);
        float l = n.length();
        if (l < EPS) {
            out[i] = Plane(Vec3::up(), -INF);
            continue;
        }
        // Inward normals, and d on the other side of the equation.
        out[i] = Plane(n / l, -rows[i].w / l);
    }
}

Projection to_projection(const Transform3D &t) {
    Projection p;
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 3; r++) p.m[c][r] = t.basis.col[c][r];
    p.m[0][3] = p.m[1][3] = p.m[2][3] = 0.0f;
    p.m[3][0] = t.origin.x;
    p.m[3][1] = t.origin.y;
    p.m[3][2] = t.origin.z;
    p.m[3][3] = 1.0f;
    return p;
}

}  // namespace wr
