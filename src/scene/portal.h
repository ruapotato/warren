// Warren -- a hole in space.
//
// A Portal3D is a rectangular aperture of arbitrary size linked to
// another one. Looking into it you see what the other one sees; walking
// into it you come out of the other one, turned by the difference
// between them and RESIZED by the ratio of their apertures.
//
//     warp = B.global * scale(wB/wA) * flip * A.global^-1
//
// `flip` is a half turn about the aperture's up axis, which is what
// makes it a portal rather than a window: you go IN to A facing it and
// come OUT of B facing away.
//
// SIZE IS PART OF THE TRANSFORM, and that is the interesting part. Most
// portal implementations keep both ends the same size so the warp is a
// rigid motion. Here A may be four metres across and B half a metre,
// and the ratio rides in the warp: a body that walks into the big one
// comes out of the small one correspondingly small, moving at a speed
// that is unchanged in its own terms, and the view through the pair is
// magnified to match. That is one multiply in the right place, and it
// is why an unequal pair reads as a size machine rather than as a
// funnel you cannot fit through.
#pragma once

#include "scene/nodes.h"

namespace wr {

class Portal3D : public Node3D {
    WR_CLASS(Portal3D, Node3D)

public:
    Portal3D();

    // --- the aperture ------------------------------------------------------
    // In local units; the node's own uniform scale multiplies both.
    float width = 2.0f;
    float height = 3.0f;

    float world_width() const { return width * global_transform().basis.uniform_scale(); }
    float world_height() const { return height * global_transform().basis.uniform_scale(); }

    // DOES THIS FIT THROUGH, and it is the other half of what makes
    // an unequal pair a puzzle rather than a curiosity.
    //
    // The size you come out at is the exit's business; whether you
    // get in at all is the entrance's. Without this second rule a
    // size machine has no gate: any body walks into any aperture
    // and the only question is what it is when it lands, so a small
    // portal is never an obstacle and a large one is never a
    // reward. With it, "make yourself big enough to lift that, and
    // small enough to get back" is a thing a level can ask.
    //
    // The rule has to be one a player can hold in their head while
    // looking at a hole, so it is the plain one: your girth against
    // the opening's width, and your height against its height --
    // except through a floor or a ceiling, where you go through end
    // on and your girth is all that matters.
    bool admits(float radius, float height) const;
    // A world point in the aperture's own frame: x across, y up
    // the opening, z out of its face. Callers that need to know
    // WHERE in the opening something is -- rather than just
    // whether it is in it -- want this.
    Vec3 local_point(const Vec3 &world_point) const;
    // How big a body this aperture will take, as the radius of the
    // largest that fits. For a HUD, and for a game that wants to
    // say why it refused.
    float admits_radius() const;

    // --- the link -----------------------------------------------------------
    // Linking is symmetric: linking A to B links B to A, and linking A
    // to something else unlinks whatever B was pointing at. A portal
    // pointing at a portal that points somewhere else is the single
    // most confusing state this system can be in, so it is not
    // representable.
    void link_to(Portal3D *other);
    void unlink();
    Portal3D *link() const { return link_; }
    bool linked() const { return link_ != nullptr; }

    bool active = true;
    // WHAT IS DRAWN THROUGH THIS HOLE. Everything, unless a game
    // says otherwise. It is separate from the camera's own mask
    // on purpose: the usual reason to hide something from the eye
    // is that the eye is inside it, and that is exactly the thing
    // you want to see from the other side.
    uint32_t cull_mask = 0xFFFFFFFFu;
    // 0 uses the renderer's global limit.
    int max_recursion = 0;

    // --- looks ---------------------------------------------------------------
    Color edge_colour = Color::hex(0xFF8C1A);
    float edge_width = 0.04f;
    // While a portal is opening, the surface is pulled towards the
    // middle so it reads as a hole being torn rather than a texture
    // fading in. 0 is closed, 1 is fully open.
    float open = 1.0f;

    // --- geometry -------------------------------------------------------------
    // Front is +Z. A portal is only a hole from the front; from behind
    // it is the back of a hole, which is nothing.
    Vec3 normal() const { return global_transform().basis.z(); }
    Plane plane() const;
    // Counter-clockwise from the bottom-left, seen from the front.
    void corners(Vec3 out[4]) const;
    AABB bounds() const;
    bool faces(const Vec3 &point) const { return plane().distance_to(point) > 0.0f; }

    // --- the transform ---------------------------------------------------------
    // Maps anything on `from`'s side to the corresponding place on
    // `to`'s. Null arguments give the identity.
    static Transform3D warp(const Portal3D *from, const Portal3D *to);
    // How much bigger you come out. 1 between equal apertures, 3 out of
    // one three times the width, 0.25 out of one a quarter of it.
    static float scale_ratio(const Portal3D *from, const Portal3D *to);
    // This portal's own warp to its link.
    Transform3D warp_out() const { return warp(this, link_); }
    float scale_out() const { return scale_ratio(this, link_); }

    // --- traversal ---------------------------------------------------------------
    // Did a segment cross this aperture front-to-back? `t` is where,
    // as a fraction along it. A crossing the wrong way is not a
    // crossing: you cannot back out through a portal's rear face.
    bool crossed(const Vec3 &from, const Vec3 &to, float *t = nullptr) const;
    // Is a point within the rectangle, projected onto the plane?
    bool within_aperture(const Vec3 &world_point, float margin = 0.0f) const;
    // The nearest point on the aperture to a world point, for a soft
    // proximity test.
    Vec3 closest_point(const Vec3 &world_point) const;

    // --- for the renderer ----------------------------------------------------------
    // The portal's rectangle on screen, in [0,1], clipped to the view.
    // False when it is entirely off screen or entirely behind. Used to
    // scissor the recursive pass, which is most of what makes stencil
    // portals affordable.
    bool screen_rect(const Transform3D &view, const Projection &proj,
                     Rect2 *out) const;
    // Is the camera close enough to the plane that it may already be
    // partly through? At that point the near plane can cut into the
    // portal surface and the illusion breaks, so the renderer widens
    // the aperture slightly to cover it.
    bool camera_is_close(const Vec3 &eye, float near_plane) const;

    void on_exit_tree() override;

private:
    Portal3D *link_ = nullptr;
};

}  // namespace wr
