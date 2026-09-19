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
        check_near(r->linear_velocity.length(), speed_in * 2.0f, 0.2f,
                   "speedy thing goes in, speedy thing comes out -- scaled");
    }

    std::printf("  %s\n", g_fail ? "FAILED" : "all good");
    return g_fail ? 1 : 0;
}
