// Manifold -- things that collide.
#pragma once

#include "physics/world.h"
#include "scene/nodes.h"

namespace mf {

class PhysicsWorld;

// Level geometry. Registered with the physics world once and never
// moved; moving one is legal but rebuilds nothing, so a moving
// platform should be a CharacterBody3D or an AnimatableBody.
class StaticBody3D : public Node3D {
    MF_CLASS(StaticBody3D, Node3D)

public:
    ~StaticBody3D() override;

    // Use the mesh of a MeshInstance3D child, or this node's own if it
    // is one. The commonest case by a distance.
    void build_from_mesh(PhysicsWorld *world, Mesh *mesh, int64_t layer = 1);
    void build_shape(PhysicsWorld *world, const Shape &shape, uint32_t layer = 1);
    void release();

    ColliderId collider() const { return collider_; }

private:
    PhysicsWorld *world_ = nullptr;
    ColliderId collider_;
};

// A CAPSULE THAT WALKS, AND THAT CHANGES SIZE.
//
// Not a rigid body: no torque, no tumbling, no sleeping. A character
// is a capsule that is pushed along a wish direction, slides off what
// it hits, climbs what it can step over, and falls when there is
// nothing under it -- which is what a player and almost every enemy
// actually wants, and none of what a rigid-body solver spends its time
// on.
//
// AND IT RESIZES. Walking through a portal into a larger one multiplies
// `size`, and with it the capsule, the node's scale (so the camera, the
// mesh and everything else parented to it come along), the step height,
// the gravity and the speeds. A two-metre doorway wired to a six-metre
// arch is a machine for making you three times as tall.
class CharacterBody3D : public Node3D {
    MF_CLASS(CharacterBody3D, Node3D)

public:
    CharacterBody3D() = default;
    ~CharacterBody3D() override;

    // --- shape, at size 1 ------------------------------------------------
    float radius = 0.35f;
    // Between the cap centres; the whole capsule is height + 2*radius.
    float height = 1.1f;

    // --- movement ----------------------------------------------------------
    Vec3 velocity;
    // Positive; applied downwards, and scaled by `size` so a shrunken
    // character does not drop like a stone.
    float gravity = 22.0f;
    float step_height = 0.4f;
    float max_slope = deg2rad(48.0f);
    // How hard the body is pushed out of anything it ends up inside.
    // 1 is immediate, which can pop; the default settles over a couple
    // of ticks and looks like nothing happened.
    float depenetration = 0.6f;
    uint32_t collision_mask = 1;

    // --- the size twist -------------------------------------------------------
    float size = 1.0f;
    float min_size = 0.04f;
    float max_size = 24.0f;
    // Sets `size`, the node's scale, and everything derived from them.
    void set_size(float s);
    float get_size() const { return size; }
    // Scaled for the current size, so game code can write `walk_speed`
    // once and have it mean the same thing at any scale.
    float scaled(float v) const { return v * size; }
    float world_radius() const { return radius * size; }
    float world_height() const { return height * size; }
    float eye_height() const { return (height + radius) * size; }

    // --- state --------------------------------------------------------------------
    bool on_floor() const { return on_floor_; }
    bool on_ceiling() const { return on_ceiling_; }
    bool on_wall() const { return on_wall_; }
    Vec3 floor_normal() const { return floor_normal_; }
    // How many portals this body has been through, and the last ratio.
    int portals_traversed() const { return portals_traversed_; }
    float last_portal_scale() const { return last_scale_; }

    // WHERE THE CAPSULE IS, GIVEN WHERE THE NODE IS.
    //
    // A character's origin is AT ITS FEET -- that is where a level
    // designer places one, it is what a camera child's height is
    // measured from, and it is what `global_position` should mean for
    // something that walks. The capsule itself is centred half a body
    // higher, and every query goes through here so that the two can
    // never drift apart.
    Transform3D body_transform() const {
        Transform3D t = global_transform();
        t.basis = t.basis.orthonormalized();
        t.origin += t.basis.y() * (world_height() * 0.5f + world_radius());
        return t;
    }

    // --- the tick ---------------------------------------------------------------------
    // Integrates gravity, moves, slides, steps up, lands, and goes
    // through any portal it crossed. Returns true if it went through
    // one.
    bool move_and_slide(PhysicsWorld *world, float dt);
    // Just the collision shape, for a caller doing its own movement.
    Shape shape() const;

    void set_world(PhysicsWorld *w) { world_ = w; }

private:
    bool cross_portals(PhysicsWorld *world, const Vec3 &before);

    PhysicsWorld *world_ = nullptr;
    bool on_floor_ = false, on_ceiling_ = false, on_wall_ = false;
    Vec3 floor_normal_{0, 1, 0};
    int portals_traversed_ = 0;
    float last_scale_ = 1.0f;
};

}  // namespace mf
