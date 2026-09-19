// Warren -- does the rigid body solver hold anything up.
//
// A solver that compiles is worth nothing. The questions that decide
// whether one is usable are all questions about what it does after a
// few hundred steps, and every one of them has a number:
//
//   does a dropped box come to rest ON the floor, and stay there;
//   does a stack of five stand, or sag and spring;
//   does friction hold a box on a slope it should hold on, and let
//     it go on one it should not;
//   does a fast body stop at a thin floor or go through it;
//   does a resting body go to sleep, and does landing on it wake it;
//   and does a body through a portal keep its speed and take its
//     size with it.
//
// These are the six, and they are in the order they were written,
// which is also the order they first failed in.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "physics/dynamics.h"
#include "physics/world.h"
#include "render/mesh.h"
#include "render/mesh_builder.h"
#include "scene/portal.h"

using namespace wr;

namespace {

int g_fail = 0;

void check(bool ok, const char *what) {
    std::printf("    %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

void check_near(float got, float want, float tol, const char *what) {
    const bool ok = std::fabs(got - want) <= tol;
    std::printf("    %s %s (%.4f, wanted %.4f +- %.3f)\n", ok ? "ok  " : "FAIL",
                what, got, want, tol);
    if (!ok) g_fail++;
}

void check_lt(float got, float limit, const char *what) {
    const bool ok = got < limit;
    std::printf("    %s %s (%.4f < %.3f)\n", ok ? "ok  " : "FAIL", what, got,
                limit);
    if (!ok) g_fail++;
}

// A floor, as a static triangle mesh, because that is what a level
// is. A box collider would take a different code path and the one
// that matters is the mesh.
Ref<Mesh> slab(const Vec3 &size) { return Mesh::box(size); }

void add_floor(PhysicsWorld &w, std::vector<Ref<Mesh>> &keep,
               const Vec3 &centre = Vec3(0, -0.5f, 0),
               const Vec3 &size = Vec3(40, 1, 40),
               const Basis &rot = Basis::identity()) {
    Ref<Mesh> m = slab(size);
    keep.push_back(m);
    Transform3D t;
    t.basis = rot;
    t.origin = centre;
    w.add_mesh(*m, t, 1);
}

// Run the world forward by `seconds` of simulated time.
void run(DynamicsWorld &d, float seconds) {
    const int n = int(seconds / d.fixed_step);
    for (int i = 0; i < n; i++) d.step(d.fixed_step);
}

}  // namespace

int main() {
    log_set_level(LogLevel::Warn);
    std::printf("dynamics\n");

    // ------------------------------------------------- a box on a floor
    {
        PhysicsWorld w;
        std::vector<Ref<Mesh>> keep;
        add_floor(w, keep);
        DynamicsWorld d(&w);
        BodyId id = d.add(Shape::box(Vec3(0.25f, 0.25f, 0.25f)),
                          Transform3D(Vec3(0, 3, 0)), 10.0f);
        run(d, 3.0f);
        const RigidBody *b = d.get(id);
        // Resting means its centre is one half-extent above the
        // surface, within the solver's slop.
        check_near(b->position.y, 0.25f, 0.02f, "a dropped box rests on the floor");
        check_lt(b->linear_velocity.length(), 0.05f, "and has stopped moving");
        check(b->sleeping, "and has gone to sleep");
        // AND IT HAS NOT WANDERED. A solver whose friction is wrong
        // slides a resting box a little every step, and over a
        // minute it is somewhere else entirely.
        check_lt(std::sqrt(b->position.x * b->position.x +
                           b->position.z * b->position.z),
                 0.02f, "and has not drifted sideways");
    }

    // ------------------------------------------------------- a stack
    {
        PhysicsWorld w;
        std::vector<Ref<Mesh>> keep;
        add_floor(w, keep);
        DynamicsWorld d(&w);
        const float h = 0.25f;
        BodyId ids[5];
        for (int i = 0; i < 5; i++) {
            // Placed just clear of each other, so the stack has to
            // settle rather than starting in the answer.
            ids[i] = d.add(Shape::box(Vec3(h, h, h)),
                           Transform3D(Vec3(0, h + float(i) * (h * 2.0f + 0.02f)
                                                   + 0.01f, 0)),
                           5.0f);
        }
        run(d, 6.0f);
        bool standing = true;
        float worst_lean = 0.0f;
        for (int i = 0; i < 5; i++) {
            const RigidBody *b = d.get(ids[i]);
            const float want = h + float(i) * h * 2.0f;
            if (std::fabs(b->position.y - want) > 0.05f) standing = false;
            // How far the box has tipped: the angle its up axis has
            // turned from vertical.
            const Vec3 up = Basis(b->orientation).y();
            worst_lean = std::max(worst_lean, std::acos(clampf(up.y, -1.0f, 1.0f)));
        }
        check(standing, "five boxes stacked stay stacked, at the right heights");
        check_lt(worst_lean, 0.08f, "and none of them has tipped");
        const RigidBody *top = d.get(ids[4]);
        check_lt(top->linear_velocity.length(), 0.05f,
                 "and the top one is not still being squeezed out");
    }

    // ----------------------------------------------------- friction
    {
        // Twenty degrees. tan(20) is 0.36, so a coefficient of 0.7
        // holds and 0.1 does not -- the two sides of the same test.
        const float angle = 20.0f * 3.14159265f / 180.0f;
        const Basis tilt = Basis::from_axis_angle(Vec3(0, 0, 1), angle);
        for (int slippery = 0; slippery < 2; slippery++) {
            PhysicsWorld w;
            std::vector<Ref<Mesh>> keep;
            add_floor(w, keep, Vec3(0, -0.5f, 0), Vec3(40, 1, 40), tilt);
            DynamicsWorld d(&w);
            BodyId id = d.add(Shape::box(Vec3(0.25f, 0.25f, 0.25f)),
                              Transform3D(tilt, tilt.xform(Vec3(0, 0.30f, 0))),
                              10.0f);
            d.get(id)->friction = slippery ? 0.05f : 0.9f;
            // Along the slope, which is the tilted floor's own x.
            const Vec3 start = d.get(id)->position;
            run(d, 4.0f);
            const float moved = (d.get(id)->position - start).length();
            if (slippery)
                check(moved > 0.8f, "a box on a slope with no grip slides off");
            else
                check_lt(moved, 0.05f, "and one with grip stays put");
        }
    }

    // ------------------------------------------------- no tunnelling
    {
        PhysicsWorld w;
        std::vector<Ref<Mesh>> keep;
        // FIVE CENTIMETRES THICK, which at 60 m/s a body crosses in
        // a fraction of one step. Without speculative contacts this
        // is the test that fails, and it fails silently -- the body
        // is simply somewhere else, still falling.
        add_floor(w, keep, Vec3(0, -0.025f, 0), Vec3(40, 0.05f, 40));
        DynamicsWorld d(&w);
        BodyId id = d.add(Shape::box(Vec3(0.2f, 0.2f, 0.2f)),
                          Transform3D(Vec3(0, 2, 0)), 8.0f);
        d.get(id)->linear_velocity = Vec3(0, -60.0f, 0);
        run(d, 2.0f);
        const RigidBody *b = d.get(id);
        check(b->position.y > 0.0f, "a body at 60 m/s does not go through 5 cm of floor");
        check_near(b->position.y, 0.2f, 0.05f, "it stops on top of it");
    }

    // --------------------------------------------------- restitution
    {
        PhysicsWorld w;
        std::vector<Ref<Mesh>> keep;
        add_floor(w, keep);
        DynamicsWorld d(&w);
        BodyId id = d.add(Shape::sphere(0.2f), Transform3D(Vec3(0, 2.0f, 0)), 2.0f);
        d.get(id)->restitution = 0.6f;
        d.get(id)->linear_damping = 0.0f;
        // Dropped from 1.8 m of fall; a 0.6 bounce returns 0.36 of
        // the height, so about 0.65 m. Generous tolerance -- this
        // is testing that restitution is applied at all and in the
        // right ballpark, not that it is analytically exact.
        float peak = 0.0f;
        bool bounced = false;
        const int n = int(2.5f / d.fixed_step);
        for (int i = 0; i < n; i++) {
            d.step(d.fixed_step);
            const RigidBody *b = d.get(id);
            if (b->linear_velocity.y > 0.1f) bounced = true;
            if (bounced) peak = std::max(peak, b->position.y);
        }
        check(bounced, "a bouncy sphere bounces");
        check(peak > 0.4f && peak < 1.1f, "and comes back to about the right height");
    }

    // -------------------------------------------------- through a portal
    {
        PhysicsWorld w;
        std::vector<Ref<Mesh>> keep;
        add_floor(w, keep);
        DynamicsWorld d(&w);
        d.gravity = Vec3();          // one thing at a time

        // Two apertures facing each other across the room, the far
        // one twice the size. A body through the pair should come
        // out twice as big and twice as fast.
        Portal3D a, b;
        a.width = 1.0f;
        a.height = 2.0f;
        a.active = true;
        a.open = 1.0f;
        a.set_position(Vec3(0, 1, -4));
        b.width = 2.0f;
        b.height = 4.0f;
        b.active = true;
        b.open = 1.0f;
        b.set_position(Vec3(0, 1, 4));
        b.set_euler(Vec3(3.14159265f, 0, 0));
        a.link_to(&b);
        w.add_portal(&a);
        w.add_portal(&b);

        BodyId id = d.add(Shape::box(Vec3(0.2f, 0.2f, 0.2f)),
                          Transform3D(Vec3(0, 1, -1)), 4.0f);
        d.get(id)->linear_velocity = Vec3(0, 0, -5.0f);
        d.get(id)->linear_damping = 0.0f;
        const float speed_in = d.get(id)->linear_velocity.length();
        // STOP AT THE FIRST CROSSING. Two apertures facing each
        // other across a room is a loop -- the cube comes out of
        // the far one heading back at the near one and goes round
        // again, doubling every time. That is the right behaviour
        // and it makes "what happened on the way through" an
        // unanswerable question unless the run is stopped.
        bool crossed = false;
        for (int i = 0; i < 400 && !crossed; i++) {
            d.step(d.fixed_step);
            crossed = d.get(id)->warped;
        }
        const RigidBody *r = d.get(id);
        check(crossed, "a body driven at an aperture goes through it");
        check_near(r->scale, 2.0f, 0.01f,
                   "and comes out the far end at the far end's size");
        // SPEED IS KEPT, NOT SCALED, and that is a deliberate
        // choice about what a resizing portal conserves. See
        // DynamicsWorld::portals_preserve_speed: scaling the speed
        // with the body is self-similar and inert -- nothing
        // changes in your own frame -- while keeping it means a
        // body that came out twice the size is now travelling at
        // half its own lengths per second, and one that came out
        // small is racing.
        check_near(r->linear_velocity.length(), speed_in, 0.2f,
                   "speedy thing goes in, speedy thing comes out");
    }

    // --------------------------------------------------- the skater
    //
    // The one piece of real conservation in a resizing portal.
    // L = I*omega; a body whose mass stays put while its radius
    // shrinks by k has its moment of inertia fall as k^2, so omega
    // rises as 1/k^2. Pulling your arms in.
    {
        PhysicsWorld w;
        std::vector<Ref<Mesh>> keep;
        DynamicsWorld d(&w);
        d.gravity = Vec3();

        // Big end first, small end second: a body through this
        // pair comes out at half the size.
        Portal3D a, b;
        a.width = 2.0f;
        a.height = 4.0f;
        a.active = true;
        a.open = 1.0f;
        a.set_position(Vec3(0, 1, -4));
        b.width = 1.0f;
        b.height = 2.0f;
        b.active = true;
        b.open = 1.0f;
        b.set_position(Vec3(0, 1, 4));
        b.set_euler(Vec3(3.14159265f, 0, 0));
        a.link_to(&b);
        w.add_portal(&a);
        w.add_portal(&b);

        BodyId id = d.add(Shape::sphere(0.25f), Transform3D(Vec3(0, 1, -1)),
                          4.0f);
        RigidBody *body = d.get(id);
        body->linear_damping = 0.0f;
        body->angular_damping = 0.0f;
        body->linear_velocity = Vec3(0, 0, -4.0f);
        // About the axis of travel, so the warp does not reorient
        // it and the magnitude is the only thing under test.
        body->angular_velocity = Vec3(0, 0, 3.0f);
        const float spin_in = body->angular_velocity.length();

        bool crossed = false;
        for (int i = 0; i < 400 && !crossed; i++) {
            d.step(d.fixed_step);
            crossed = d.get(id)->warped;
        }
        const RigidBody *r = d.get(id);
        check(crossed, "a spinning body goes through a shrinking pair");
        check_near(r->scale, 0.5f, 0.01f, "and comes out at half the size");
        // Half the size, so four times the spin.
        check_near(r->angular_velocity.length(), spin_in * 4.0f, 0.3f,
                   "and four times the spin -- a skater pulling their arms in");
    }

    // ------------------------------------ a body you can shoot at
    //
    // Queries walk the PhysicsWorld's colliders and a rigid body
    // lives in the dynamics world, so without a proxy in the
    // other list a dynamic object is INVISIBLE to every query in
    // the engine. You cannot shoot a crate, click one, pick one
    // up, or walk a character controller into one -- all of which
    // read as the object not being there, because as far as
    // anything asking is concerned it is not.
    //
    // Found the hard way: picking up a barrel in a game failed
    // silently, because the ray looking for it went straight
    // through.
    {
        PhysicsWorld w;
        std::vector<Ref<Mesh>> keep;
        add_floor(w, keep);
        DynamicsWorld d(&w);
        BodyId id = d.add(Shape::box(Vec3(0.4f, 0.4f, 0.4f)),
                          Transform3D(Vec3(0, 1.0f, 0)), 10.0f);
        d.step(d.fixed_step);

        RayHit h = w.raycast(Vec3(0, 1.0f, 4.0f), Vec3(0, 1.0f, -4.0f));
        check(h.hit, "a ray finds a rigid body");
        check(h.hit && h.position.z > 0.0f && h.position.z < 1.0f,
              "at its near face");

        // AND IT FOLLOWS THE BODY. A proxy left where the body
        // started is worse than none: the crate is shootable
        // where it used to be.
        d.get(id)->position = Vec3(3.0f, 1.0f, 0.0f);
        d.step(d.fixed_step);
        RayHit miss = w.raycast(Vec3(0, 1.0f, 4.0f), Vec3(0, 1.0f, -4.0f));
        RayHit found = w.raycast(Vec3(3.0f, 1.0f, 4.0f),
                                 Vec3(3.0f, 1.0f, -4.0f));
        check(!miss.hit, "and it is not still where it was");
        check(found.hit, "it is where it is now");

        // A body that has been through a portal is bigger, and
        // the thing you shoot at has to be bigger with it.
        d.get(id)->scale = 2.0f;
        d.step(d.fixed_step);
        RayHit wide = w.raycast(Vec3(3.0f + 0.6f, 1.0f, 4.0f),
                                Vec3(3.0f + 0.6f, 1.0f, -4.0f));
        check(wide.hit, "and a body that grew is bigger to shoot at too");

        d.remove(id);
        RayHit gone = w.raycast(Vec3(3.0f, 1.0f, 4.0f),
                                Vec3(3.0f, 1.0f, -4.0f));
        check(!gone.hit, "and a removed body leaves nothing behind");
    }

    // ------------------------------ a body at a size that is not one
    //
    // The size lives on the shape, via sized(). It must not ALSO
    // live in the transform handed to the narrow phase, or the
    // body is collided at the square of its size -- a three-times
    // crate as a nine-times one. Which does not look like a
    // scaling bug: the crate spawns overlapping whatever is near
    // it and the depenetration throws it across the room at the
    // cap, so it reads as the solver exploding.
    //
    // A box of half-extent 0.4 at three times the size is 2.4 m
    // across and rests with its centre 1.2 m up. At the square it
    // would be 7.2 m across and rest at 3.6.
    {
        PhysicsWorld w;
        std::vector<Ref<Mesh>> keep;
        add_floor(w, keep);
        DynamicsWorld d(&w);
        BodyId id = d.add(Shape::box(Vec3(0.4f, 0.4f, 0.4f)),
                          Transform3D(Vec3(0, 4.0f, 0)), 10.0f);
        d.get(id)->scale = 3.0f;
        d.refresh_mass(id);
        run(d, 4.0f);
        const RigidBody *b = d.get(id);
        check_near(b->position.y, 1.2f, 0.05f,
                   "a 3x box rests at 3x its half-extent, not 9x");
        check_lt(b->linear_velocity.length(), 0.1f, "and it is still");
        check_lt(std::sqrt(b->position.x * b->position.x +
                           b->position.z * b->position.z),
                 0.1f, "and it did not get thrown anywhere");

        // And the thing you can shoot at is the same size as the
        // thing that rests on the floor.
        RayHit edge = w.raycast(Vec3(1.0f, 1.2f, 4.0f), Vec3(1.0f, 1.2f, -4.0f));
        RayHit past = w.raycast(Vec3(1.6f, 1.2f, 4.0f), Vec3(1.6f, 1.2f, -4.0f));
        check(edge.hit, "a ray inside its 2.4 m width finds it");
        check(!past.hit, "and one outside that does not");
    }

    std::printf("  %s\n", g_fail ? "FAILED" : "all good");
    return g_fail ? 1 : 0;
}
