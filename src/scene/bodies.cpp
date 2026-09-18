#include "bodies.h"

#include <algorithm>

#include "core/log.h"
#include "scene/portal.h"

namespace mf {

// ------------------------------------------------------------ StaticBody3D

StaticBody3D::~StaticBody3D() { release(); }

void StaticBody3D::release() {
    if (world_ && collider_.valid()) world_->remove(collider_);
    collider_ = {};
    world_ = nullptr;
}

void StaticBody3D::build_from_mesh(PhysicsWorld *world, Mesh *mesh,
                                   int64_t layer) {
    release();
    if (!world || !mesh) return;
    world_ = world;
    collider_ = world->add_mesh(*mesh, global_transform(), uint32_t(layer), this);
}

void StaticBody3D::build_shape(PhysicsWorld *world, const Shape &shape,
                               uint32_t layer) {
    release();
    if (!world) return;
    world_ = world;
    collider_ = world->add_shape(shape, global_transform(), layer, this, true);
}

// -------------------------------------------------------- CharacterBody3D

CharacterBody3D::~CharacterBody3D() = default;

Shape CharacterBody3D::shape() const {
    return Shape::capsule(world_radius(), world_height());
}

void CharacterBody3D::set_size(float s) {
    s = clampf(s, min_size, max_size);
    if (nearly(s, size, 1e-6f)) return;
    size = s;
    // THE NODE'S SCALE GOES WITH IT, so the camera, the mesh, the held
    // weapon and anything else parented to this body come along
    // without being told. One number, and the whole character is a
    // different size.
    Node3D::set_scale(s);
}

// THE MOVE.
//
// Integrate, then move in up to four slides: sweep to the first
// contact, take that much of the motion, remove the component into
// the surface, and go again with what is left. Four is enough for a
// corner and a step; a fifth is a body wedged somewhere it should be
// pushed out of instead, which is what the depenetration pass at the
// top is for.
bool CharacterBody3D::move_and_slide(PhysicsWorld *world, float dt) {
    if (!world) world = world_;
    if (!world || dt <= 0.0f) return false;
    world_ = world;

    // Everything below works in CAPSULE space -- the node's transform
    // lifted to the capsule's centre -- and the node is put back at
    // the end. Mixing the two is how a character ends up half in the
    // floor.
    Transform3D t = body_transform();
    const Shape body = shape();
    const Vec3 before = t.origin;

    // --- out of anything we are already inside
    {
        Vec3 correction;
        if (world->depenetrate(body, t, collision_mask, this, &correction) > 0)
            t.origin += correction * clampf(depenetration, 0.0f, 1.0f);
    }

    // --- gravity, scaled so a small character is not a lead weight
    if (!on_floor_)
        velocity.y -= gravity * size * dt;
    else if (velocity.y <= 0.0f)
        velocity.y = -0.5f * size;  // a light press, so slopes hold

    Vec3 motion = velocity * dt;
    on_floor_ = on_ceiling_ = on_wall_ = false;
    floor_normal_ = Vec3::up();

    for (int slide = 0; slide < 4; slide++) {
        if (motion.length_sq() < 1e-12f) break;
        SweepHit hit = world->sweep(body, t, motion, collision_mask, this);
        if (!hit.hit) {
            t.origin += motion;
            break;
        }

        // Advance to just short of the contact, so the next sweep does
        // not start already touching.
        float safe = std::max(0.0f, hit.fraction - 1e-3f);
        t.origin += motion * safe;
        Vec3 remaining = motion * (1.0f - safe);

        float slope = dot(hit.normal, Vec3::up());
        if (slope > std::cos(max_slope)) {
            on_floor_ = true;
            floor_normal_ = hit.normal;
        } else if (slope < -0.7f) {
            on_ceiling_ = true;
        } else {
            on_wall_ = true;
            // --- STEPPING UP.
            //
            // A wall that is really a kerb should be climbed, not
            // stopped at. Lift, try the same motion, and drop back
            // down; if the drop lands on something walkable, the step
            // happened.
            if (on_floor_ || slide == 0) {
                Transform3D lifted = t;
                lifted.origin += Vec3::up() * (step_height * size);
                Vec3 flat(remaining.x, 0.0f, remaining.z);
                if (flat.length_sq() > 1e-9f &&
                    !world->sweep(body, lifted, flat, collision_mask, this).hit) {
                    Transform3D stepped = lifted;
                    stepped.origin += flat;
                    SweepHit down = world->sweep(body, stepped,
                                                Vec3::down() * (step_height * size * 1.05f),
                                                collision_mask, this);
                    if (down.hit && dot(down.normal, Vec3::up()) > std::cos(max_slope)) {
                        t.origin = stepped.origin +
                                   Vec3::down() * (step_height * size * 1.05f * down.fraction);
                        on_floor_ = true;
                        floor_normal_ = down.normal;
                        motion = Vec3();
                        continue;
                    }
                }
            }
        }

        // Slide: drop the part of the motion going into the surface,
        // and the same part of the velocity, or the body keeps trying
        // to push through next tick.
        motion = slide_vec(remaining, hit.normal);
        velocity = slide_vec(velocity, hit.normal);
    }

    // --- THE GROUND PROBE.
    //
    // The slide loop no longer reports the floor, because standing on
    // something is not a collision with it. So the floor is looked for
    // explicitly: a short sweep downwards, which also snaps the body
    // to a slope it is walking down instead of letting it launch off
    // every crest.
    if (velocity.y <= 0.01f * size) {
        float probe = (0.06f + step_height * 0.5f) * size;
        SweepHit down = world->sweep(body, t, Vec3::down() * probe,
                                     collision_mask, this);
        if (down.hit && dot(down.normal, Vec3::up()) > std::cos(max_slope)) {
            t.origin += Vec3::down() * (probe * down.fraction);
            on_floor_ = true;
            floor_normal_ = down.normal;
            if (velocity.y < 0.0f) velocity.y = 0.0f;
        }
    }

    // Back down to the feet -- KEEPING THE SCALE.
    //
    // `t` came out of body_transform(), whose basis is orthonormal
    // because the physics wants it that way. Writing that basis back
    // would reset the node's scale to one every tick, and the
    // character would keep its collider's new size while everything
    // parented to it silently shrank back.
    {
        Transform3D node;
        node.basis = t.basis.orthonormalized() * size;
        node.origin = t.origin - t.basis.y().normalized() *
                                     (world_height() * 0.5f + world_radius());
        set_global_transform(node);
    }

    // --- and finally, through anything it crossed
    return cross_portals(world, before);
}

// THE TRAVERSAL, AND THE RESIZE.
bool CharacterBody3D::cross_portals(PhysicsWorld *world, const Vec3 &before) {
    // THE TEST POINT IS THE CAPSULE'S MIDDLE, and `before` is already
    // in capsule space.
    //
    // Testing the feet instead would mean that a portal resting on the
    // floor -- which is where one gets put, every time -- has its
    // lower edge exactly at the height being tested, and whether
    // anything gets through comes down to a centimetre of floor
    // snapping.
    Transform3D t = body_transform();
    float where = 0.0f;
    Portal3D *p = world->crossing(before, t.origin, &where);
    if (!p || !p->link()) return false;

    Portal3D *q = p->link();
    Transform3D warp = Portal3D::warp(p, q);
    float ratio = Portal3D::scale_ratio(p, q);

    // Warp the CAPSULE, then convert back to the node's feet at the
    // new size.
    Transform3D moved = warp * t;
    // The basis is re-orthonormalised and the scale applied through
    // `set_size` instead: a physics body with a scaled transform has a
    // scaled collision shape, and the scale would compound into the
    // next warp besides.
    float new_size = clampf(size * ratio, min_size, max_size);
    set_size(new_size);
    Transform3D node;
    // The warp's rotation, at the new size -- so the node's scale, and
    // with it every child, is the body's new size.
    node.basis = moved.basis.orthonormalized() * new_size;
    node.origin = moved.origin - node.basis.y().normalized() *
                                     (world_height() * 0.5f + world_radius());
    set_global_transform(node);

    // The velocity goes through the warp's basis, which carries the
    // ratio -- so a body shrunk by half arrives moving half as fast in
    // world terms and exactly as fast in its own.
    velocity = warp.basis.xform(velocity);

    // Clear of the exit plane by its NEW width, so it does not arrive
    // inside the wall the exit is mounted on and get pushed back
    // through on the same tick.
    Transform3D out = global_transform();
    out.origin += q->normal() * (world_radius() * 1.05f + 0.01f);
    set_global_transform(out);

    portals_traversed_++;
    last_scale_ = ratio;
    Array args{Variant(p), Variant(double(ratio))};
    emitv("portal_traversed", args);
    return true;
}

// ------------------------------------------------------------ reflection

static void register_body_classes() {
    ClassBuilder<StaticBody3D>()
        .method("build_from_mesh", &StaticBody3D::build_from_mesh,
                {Variant(int64_t(1))})
        .args("world", "mesh", "layer")
        .method("release", &StaticBody3D::release);

    ClassBuilder<CharacterBody3D>()
        .field("radius", &CharacterBody3D::radius)
        .field("height", &CharacterBody3D::height)
        .field("velocity", &CharacterBody3D::velocity)
        .field("gravity", &CharacterBody3D::gravity)
        .field("step_height", &CharacterBody3D::step_height)
        .field("max_slope", &CharacterBody3D::max_slope)
        .field("collision_mask", &CharacterBody3D::collision_mask)
        .field("min_size", &CharacterBody3D::min_size)
        .field("max_size", &CharacterBody3D::max_size)
        .prop("size", &CharacterBody3D::get_size, &CharacterBody3D::set_size)
        .method("move_and_slide", &CharacterBody3D::move_and_slide).args("world", "dt")
        .method("is_on_floor", &CharacterBody3D::on_floor)
        .method("is_on_wall", &CharacterBody3D::on_wall)
        .method("is_on_ceiling", &CharacterBody3D::on_ceiling)
        .method("get_floor_normal", &CharacterBody3D::floor_normal)
        .method("world_radius", &CharacterBody3D::world_radius)
        .method("world_height", &CharacterBody3D::world_height)
        .method("eye_height", &CharacterBody3D::eye_height)
        .method("scaled", &CharacterBody3D::scaled).args("value")
        .method("portals_traversed", &CharacterBody3D::portals_traversed)
        .signal("portal_traversed", {VType::Object, VType::Float});
}
MF_REGISTER(register_body_classes)

}  // namespace mf
