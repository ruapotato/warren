// Warren -- the projection matrix, as a matrix.
//
// THIS TYPE IS WHY THE ENGINE EXISTS.
//
// Most engines model a camera as a field of view, a near plane and a far
// plane, build a matrix from those three numbers behind your back, and
// never let you see it. That is enough for every camera except the one a
// portal needs.
//
// A portal's virtual camera must not draw anything on the near side of
// the destination aperture -- the wall it is mounted on, the furniture
// between it and the viewer, the back of the portal's own frame. The
// only exact way to say that is to put the near plane IN the aperture,
// and an aperture seen at an angle is not perpendicular to the view. A
// fov-and-near-plane camera cannot express it; a matrix can, in four
// assignments. Everything that makes portals hard in other engines --
// the wall creeping into shot, the wedge of view traded away to hide it,
// the resolution ladders and clip biases that manage the compromise --
// is downstream of not being allowed to touch these sixteen floats.
//
// So here they are. `perspective` and `orthographic` are convenience
// constructors, not the type. A camera holds a Projection, and anything
// may hand it one.
#pragma once

#include "transform.h"

namespace wr {

struct Projection {
    // Column-major: m[c][r]. Uploads to GLSL unchanged.
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    constexpr Projection() = default;

    static Projection identity() { return {}; }
    static Projection zero() {
        Projection p;
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++) p.m[c][r] = 0.0f;
        return p;
    }

    // --- the usual ways to get one -------------------------------------

    // Symmetric perspective. `fov_y` in radians. Reverse-Z: the near
    // plane comes out at 1 and the far plane at 0.
    static Projection perspective(float fov_y, float aspect, float z_near,
                                  float z_far);
    // No far plane at all. Reverse-Z makes this not just possible but
    // BETTER than a finite one: precision is set by the near plane
    // alone, so a terrain that runs to the horizon costs nothing and
    // nothing ever needs a far-plane fade. This is the default for an
    // outdoor camera.
    static Projection perspective_infinite(float fov_y, float aspect,
                                           float z_near);
    // Off-axis perspective: the near-plane rectangle, stated directly.
    // A sub-rectangle of a frustum is still a frustum, which is how a
    // shadow cascade or a cropped render gets its camera.
    static Projection frustum(float left, float right, float bottom, float top,
                              float z_near, float z_far);
    static Projection orthographic(float left, float right, float bottom,
                                   float top, float z_near, float z_far);
    // A perspective whose apex is offset from the centre of the near
    // rectangle, in units of half-extent. Stereo and lens shift.
    static Projection perspective_offset(float fov_y, float aspect, float z_near,
                                         float z_far, const Vec2 &offset);

    // --- THE ONE THAT MATTERS ------------------------------------------

    // Rebuild the near plane to lie in `clip_plane`, which is given in
    // VIEW SPACE with the convention that dot(plane, (p, 1)) >= 0 marks
    // the half-space to keep. The far plane, the field of view and the
    // shape of the frustum are untouched; only the near plane moves,
    // and it moves somewhere a perpendicular plane cannot reach.
    //
    // THE DERIVATION, because the published one does not apply here.
    //
    // Lengyel's 2005 form assumes OpenGL's [-1, 1] depth. This engine
    // uses [0, 1] reversed, so it is derived again from the clip
    // condition itself, which is cleaner and turns out to be more
    // general.
    //
    // The hardware keeps 0 <= z_clip <= w_clip. The near plane is
    // therefore exactly the set where z_clip = w_clip, which is the set
    // where (row2 - row3) . p = 0 -- a plane determined by row2 alone,
    // since row3 is fixed by the perspective divide. So to put the near
    // plane on C it is enough to set
    //
    //     row2 = row3 + k C,     k < 0
    //
    // for any negative k: negative so that the kept half-space,
    // C . p >= 0, is the one where z_clip <= w. Rows 0 and 1 never
    // move, which is why the picture through the portal is unchanged
    // and only the near side of it is cut away.
    //
    // k is then spent on the far plane. z_clip = 0 is the far plane, so
    // pinning it through the corner of the original frustum diagonally
    // opposite C's tilt -- clip-space (sgn Cx, sgn Cy, 0, 1), pushed
    // back through the projection -- keeps the depth range as wide as
    // the constraint allows:
    //
    //     k = -(row3 . q) / (C . q)
    //
    // Because row3 is read from the matrix rather than assumed, this
    // works unchanged for an off-axis frustum, for an already-oblique
    // projection, and for an orthographic one -- which is how a shadow
    // cascade gets clipped to a portal for free.
    Projection with_oblique_near(const Vec4 &clip_plane) const;
    Projection with_oblique_near(const Plane &clip_plane_view) const {
        return with_oblique_near(clip_plane_view.as_vec4());
    }

    // --- reading one ----------------------------------------------------

    bool is_orthographic() const { return m[3][3] == 1.0f; }
    // True for perspective_infinite: no far plane, so get_z_far is inf.
    bool is_infinite_far() const;
    float get_aspect() const;
    float get_fov_y() const;
    float get_z_near() const;
    float get_z_far() const;
    // The near rectangle, in view space, at distance `z`.
    void get_extents_at(float z, float *left, float *right, float *bottom,
                        float *top) const;
    // THE EIGHT CORNERS OF A DEPTH SLICE, in view space, near face
    // first, each face ordered (-x,-y), (+x,-y), (+x,+y), (-x,+y).
    //
    // This is what a shadow cascade is fitted to. It reads the x and y
    // extents rather than unprojecting the depth range, which matters
    // because an OBLIQUE near plane changes only the projection's
    // third row: the slice a portal view covers is still the ordinary
    // frustum of the camera it was warped from, and fitting a cascade
    // to it must not depend on where the oblique cut happens to fall.
    void slice_corners(float z_near, float z_far, Vec3 out[8]) const;

    // --- using one ------------------------------------------------------

    Projection operator*(const Projection &o) const;
    Vec4 xform(const Vec4 &v) const;
    // View space to normalised device coordinates, with the divide.
    Vec3 project(const Vec3 &view_point) const {
        return xform(Vec4(view_point, 1.0f)).homogenized();
    }
    // The reverse: a point in NDC back to view space.
    Vec3 unproject(const Vec3 &ndc) const {
        return inverse().xform(Vec4(ndc, 1.0f)).homogenized();
    }
    Projection inverse() const;
    Projection transposed() const;

    // A ray through an NDC point, in view space. Origin and unit
    // direction; for an orthographic projection the origin moves and the
    // direction does not.
    void ray_at(const Vec2 &ndc, Vec3 *origin, Vec3 *dir) const;

    // The six planes of the frustum in WORLD space, given where the
    // camera is. Order: left, right, bottom, top, near, far. Normals
    // point inwards, so `distance_to(p) >= -radius` is the sphere test.
    void frustum_planes(const Transform3D &camera, Plane out[6]) const;

    // Sub-pixel offset, for temporal sampling. In NDC units.
    //
    // A perspective projection shifts by its z column, because that
    // column is divided by -z along with everything else and so moves
    // the whole image rather than skewing it; an orthographic one has
    // no divide and shifts by its translation column instead.
    Projection jittered(const Vec2 &ndc_offset) const {
        Projection p = *this;
        if (is_orthographic()) {
            p.m[3][0] += ndc_offset.x;
            p.m[3][1] += ndc_offset.y;
        } else {
            p.m[2][0] += ndc_offset.x;
            p.m[2][1] += ndc_offset.y;
        }
        return p;
    }

    const float *data() const { return &m[0][0]; }
    bool operator==(const Projection &o) const {
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                if (m[c][r] != o.m[c][r]) return false;
        return true;
    }
    bool operator!=(const Projection &o) const { return !(*this == o); }
};

// A transform used as a 4x4, for the view matrix and for model matrices.
Projection to_projection(const Transform3D &t);

}  // namespace wr
