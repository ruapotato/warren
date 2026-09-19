// Warren -- contact manifolds.
//
// WHY THIS IS NOT `contact_one`. The character controller's narrow
// phase answers "how deep, and which way out", which is one point
// and one normal, and that is the right answer for a capsule being
// pushed out of a wall. It is the wrong answer for a box resting on
// a floor: a single point under a rigid body is a pivot, so a crate
// set down flat rocks on one corner, finds another, and jitters
// there for ever while the solver argues with gravity.
//
// A resting box needs its contact to be described as a FACE -- four
// points round a quadrilateral -- so the solver can hold all four
// and the box has nothing to rotate about. That is what a manifold
// is, and generating one is a different algorithm from measuring a
// penetration.
//
// The method is the standard one and is worth naming because the
// literature is easier to follow than any summary: separating axis
// to find the direction of least penetration, then reference-face
// and incident-face clipping (Sutherland-Hodgman) to find where the
// two surfaces actually overlap.
//
// CONTACT IDS MATTER MORE THAN THEY LOOK. Warm starting reuses last
// step's impulse at the same contact, and that is most of what makes
// a stack stand still rather than sag and spring. "The same contact"
// has to survive the manifold being regenerated from scratch every
// step, so each point carries the pair of features that produced it.
#pragma once

#include "core/math/transform.h"
#include "physics/shapes.h"

namespace wr {

// One point of contact.
struct Contact {
    // On the surface of B, in world space.
    Vec3 position;
    // How far the two overlap along the manifold normal. Positive is
    // penetrating; a small negative is a speculative contact, which
    // the solver is allowed to see coming.
    float depth = 0.0f;
    // Which features made this point. Stable across steps while the
    // two shapes stay in roughly the same relative pose, which is
    // exactly when warm starting is worth anything.
    uint32_t id = 0;
    // Carried over by the solver between steps. Not filled in here.
    float normal_impulse = 0.0f;
    float tangent_impulse[2] = {0.0f, 0.0f};
    // HOW FAST IT WAS COMING IN, remembered from before it arrived.
    //
    // Speculative contacts and restitution are in direct conflict:
    // the speculative constraint's whole job is to remove the
    // approach velocity so the body lands exactly on the surface,
    // and a bounce computed at the moment of touching therefore
    // finds nothing left to reflect. A ball dropped from any height
    // lands dead.
    //
    // So the approach is captured while the contact is still a gap
    // and kept until it closes. Nothing else in the solver needs to
    // know, which is why it lives on the contact.
    float approach = 0.0f;
};

// Up to four points and a shared normal. Four because that is what a
// face-face overlap needs and a fifth adds nothing a solver can use:
// a convex polygon's contact region is held by its extreme points.
struct Manifold {
    // World space, pointing from A out of B -- so moving A along it
    // separates them.
    Vec3 normal{0.0f, 1.0f, 0.0f};
    int count = 0;
    Contact points[4];

    void clear() { count = 0; }
    bool add(const Vec3 &p, float depth, uint32_t id);
};

// The pair of them, in world space. Returns false when they are
// apart by more than `margin`.
//
// `margin` is what makes speculative contacts possible: ask for a
// manifold slightly before the shapes touch and the solver can stop
// a fast body at the surface instead of letting it pass through and
// pushing it back out next step. Zero for a plain overlap test.
bool collide(const Shape &a, const Transform3D &ta, const Shape &b,
             const Transform3D &tb, Manifold *out, float margin = 0.0f);

// A shape against one triangle. Separate because the world is a
// triangle soup and asking it for a Shape per triangle would mean
// building one; and because a triangle is one-sided in a way a box
// is not -- see the comment on the implementation.
bool collide_triangle(const Shape &s, const Transform3D &t, const Vec3 tri[3],
                      Manifold *out, float margin = 0.0f);

// The inertia tensor of a shape of this mass, about its centre, in
// local space. Diagonal for all four primitives, which is why this
// can return a Basis and not a full symmetric solve.
Basis inertia_of(const Shape &s, float mass);

}  // namespace wr
