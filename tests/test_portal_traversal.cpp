// Warren -- going through a portal, and coming out a different size.
//
// The claim this file checks is the one the engine is named for: a
// pair of apertures of different sizes is not a funnel you cannot fit
// through, it is a similarity transform. Walk into the two-metre end
// of a 2:8 pair and you come out of the eight-metre end four times the
// size, four times as fast in world terms and unchanged in your own.
//
// Headless: no window, no device, no renderer. Just the physics and
// the transform.
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "physics/world.h"
#include "render/mesh.h"
#include "render/mesh.h"
#include "scene/bodies.h"
#include "scene/portal.h"
#include "scene/scene_tree.h"

using namespace wr;

namespace {

int g_fail = 0;
int g_checks = 0;

void check(bool ok, const char *what, double detail = 0.0) {
    g_checks++;
    if (!ok) {
        std::printf("  FAIL  %s  (%g)\n", what, detail);
        g_fail++;
    }
}
void near_check(float got, float want, float tol, const char *what) {
    g_checks++;
    if (!(std::fabs(got - want) <= tol)) {
        std::printf("  FAIL  %s: got %.6g want %.6g (tol %g)\n", what, got, want, tol);
        g_fail++;
    }
}
void section(const char *s) { std::printf("%s\n", s); }

// A floor and two walls far apart, each with a portal in it. No
// meshes, no materials: boxes are enough to walk on and to be stopped
// by.
struct Fixture {
    SceneTree tree;
    Ref<PhysicsWorld> world{new PhysicsWorld()};
    Portal3D *small = nullptr;
    Portal3D *large = nullptr;
    CharacterBody3D *body = nullptr;

    Fixture(float small_width, float large_width) {
        Node *scene = new Node();
        scene->set_name("Scene");
        tree.set_scene(scene);

        // Two floors, one at each end, and a wall behind each portal
        // so that a body that does NOT go through is stopped.
        world->add_shape(Shape::box({40, 0.5f, 40}), Transform3D(Vec3(0, -0.5f, 0)), 1);
        world->add_shape(Shape::box({40, 0.5f, 40}), Transform3D(Vec3(200, -0.5f, 0)), 1);
        world->add_shape(Shape::box({20, 8, 0.5f}), Transform3D(Vec3(0, 8, -6)), 1);
        world->add_shape(Shape::box({40, 20, 0.5f}), Transform3D(Vec3(200, 20, 6)), 1);

        small = new Portal3D();
        small->set_name("Small");
        small->width = small_width;
        small->height = small_width * 1.7f;
        small->set_position({0, small->height * 0.5f, -6.0f});
        scene->add_child(small);

        large = new Portal3D();
        large->set_name("Large");
        large->width = large_width;
        large->height = large_width * 1.7f;
        large->set_position({200, large->height * 0.5f, 6.0f});
        large->set_rotation(Quat::from_axis_angle(Vec3::up(), PI));
        scene->add_child(large);
        small->link_to(large);

        world->add_portal(small);
        world->add_portal(large);

        body = new CharacterBody3D();
        body->set_name("Body");
        body->radius = 0.3f;
        body->height = 1.0f;
        body->set_world(world.get());
        body->set_global_position({0, 0.05f, 1.5f});
        scene->add_child(body);
    }

    // Walk forwards until something happens or the time runs out.
    int walk(const Vec3 &direction, float seconds, float speed_in_own_units) {
        const float dt = 1.0f / 120.0f;
        int steps = int(seconds / dt);
        int crossings = 0;
        for (int i = 0; i < steps; i++) {
            Vec3 want = direction * body->scaled(speed_in_own_units);
            body->velocity.x = want.x;
            body->velocity.z = want.z;
            if (body->move_and_slide(world.get(), dt)) crossings++;
        }
        return crossings;
    }
};

// -------------------------------------------------------------- the tests

void test_warp_algebra() {
    section("the warp itself");
    Fixture f(2.0f, 8.0f);
    near_check(Portal3D::scale_ratio(f.small, f.large), 4.0f, 1e-4f,
               "small to large is 4x");
    near_check(Portal3D::scale_ratio(f.large, f.small), 0.25f, 1e-4f,
               "large to small is a quarter");
    near_check(Portal3D::scale_ratio(f.small, f.small), 1.0f, 1e-4f,
               "a portal to itself is 1x");

    // The warp must take the aperture's own centre to the other's.
    Transform3D w = Portal3D::warp(f.small, f.large);
    Vec3 there = w.xform(f.small->global_position());
    near_check((there - f.large->global_position()).length(), 0.0f, 1e-3f,
               "the centre maps to the centre");
    near_check(w.basis.uniform_scale(), 4.0f, 1e-3f, "the warp carries the ratio");
    check(w.basis.is_uniform(), "the warp's scale is uniform");

    // A HALF TURN, NOT A MIRROR. Going in facing the front of one must
    // come out facing AWAY from the front of the other, and the world
    // must not be inside out -- a negative determinant here is a
    // reflection, and everything would be back to front through it.
    check(w.basis.determinant() > 0.0f, "the warp does not mirror",
          double(w.basis.determinant()));
    Vec3 into = -f.small->normal();                 // walking into the small one
    Vec3 out = w.basis.xform(into).normalized();
    near_check(dot(out, f.large->normal()), 1.0f, 1e-3f,
               "you come out facing away from the far portal");

    // Round trip: there and back is the identity.
    Transform3D back = Portal3D::warp(f.large, f.small);
    Transform3D loop = back * w;
    Vec3 p(0.3f, 1.1f, -4.0f);
    near_check((loop.xform(p) - p).length(), 0.0f, 1e-2f, "there and back again");
    near_check(loop.basis.uniform_scale(), 1.0f, 1e-3f, "and the scale comes back");
}

void test_walking_through() {
    section("walking through, and changing size");
    Fixture f(2.0f, 8.0f);
    near_check(f.body->get_size(), 1.0f, 1e-5f, "starts at 1x");
    const float start_eye = f.body->eye_height();

    int crossings = f.walk(Vec3(0, 0, -1), 4.0f, 4.0f);
    check(crossings == 1, "went through exactly once", double(crossings));
    near_check(f.body->get_size(), 4.0f, 1e-3f, "and came out four times the size");
    near_check(f.body->eye_height(), start_eye * 4.0f, 1e-2f,
               "so its eye is four times as high");
    near_check(f.body->world_radius(), 0.3f * 4.0f, 1e-3f,
               "and its capsule is four times as wide");

    // It must be on the far side, near the large portal, and clear of
    // the wall that portal is cut into.
    Vec3 p = f.body->global_position();
    check(std::fabs(p.x - 200.0f) < 12.0f, "it arrived at the far room", double(p.x));
    check(p.z < 6.0f, "and on the near side of the far wall", double(p.z));

    // The node's scale went with it, so anything parented to the body
    // -- a camera, a mesh, a held object -- is the right size too.
    near_check(f.body->global_transform().basis.uniform_scale(), 4.0f, 1e-3f,
               "the node's scale follows the body's size");
}

void test_coming_back() {
    section("and back again");
    Fixture f(2.0f, 8.0f);
    f.walk(Vec3(0, 0, -1), 4.0f, 4.0f);
    near_check(f.body->get_size(), 4.0f, 1e-3f, "four times the size out there");

    // Turn round and walk back through the large end. The large
    // portal's front faces +Z after its half turn, so coming back
    // means walking +Z.
    int crossings = f.walk(Vec3(0, 0, 1), 6.0f, 4.0f);
    check(crossings >= 1, "came back through", double(crossings));
    near_check(f.body->get_size(), 1.0f, 2e-2f, "and is its old size again");
    Vec3 p = f.body->global_position();
    check(std::fabs(p.x) < 12.0f, "back in the first room", double(p.x));
}

// WHAT A RESIZING PORTAL DOES TO YOUR SPEED, and it is a choice
// about what is conserved rather than a fact -- changing your
// size is not a physical process.
//
// The rule is that SPEED IS KEPT. It is the plain generalisation
// of what an equal-sized pair does, and it is the only one of the
// three candidates that makes a size machine worth using:
//
//   scaling the speed with the body is self-similar, so nothing
//     changes in your own frame and shrinking has no consequence
//     you can feel;
//   conserving momentum with the mass held constant gives the
//     same answer as this one for velocity, and leaves a shrunk
//     object too heavy to push;
//   conserving momentum with the density held constant sends
//     velocity up as the inverse cube of the size, which at a
//     tenth scale is a thousandfold.
//
// So: the metres per second are unchanged, and what changes is
// what a metre is worth to you. Come out four times the size and
// you are crawling at a quarter of your own lengths per second;
// come out small and you are racing.
void test_speed_is_preserved() {
    section("speed is unchanged through a resizing portal");
    Fixture f(2.0f, 8.0f);
    f.body->set_global_position({0, 0.05f, 1.2f});
    const float dt = 1.0f / 120.0f;
    bool crossed = false;
    for (int i = 0; i < 400 && !crossed; i++) {
        f.body->velocity.x = 0.0f;
        f.body->velocity.z = -6.0f;
        crossed = f.body->move_and_slide(f.world.get(), dt);
    }
    check(crossed, "crossed");
    const float world_speed =
        Vec2(f.body->velocity.x, f.body->velocity.z).length();
    near_check(world_speed, 6.0f, 1.0f, "the same metres per second");
    near_check(f.body->get_size(), 4.0f, 0.1f, "at four times the size");
    // Which is the point: four times as big, same world speed, so
    // a quarter of the pace in the only units the body cares
    // about.
    near_check(world_speed / f.body->get_size(), 1.5f, 0.4f,
               "and therefore a quarter of the pace in its own lengths");
}

void test_ray_through_a_portal() {
    section("tracing through a portal");
    Fixture f(2.0f, 8.0f);

    // A ray fired at the small portal must not stop at the wall: it
    // must come out of the large one and hit the far floor.
    Vec3 from(0.0f, 1.0f, 2.0f);
    Vec3 to(0.0f, 1.0f, -40.0f);
    // Even the plain raycast sees the hole: the doorway is a property
    // of the geometry, and only the TELEPORTING is the portal's own
    // doing. So the straight ray passes through and hits nothing.
    RayHit straight = f.world->raycast(from, to, 0xFFFFFFFF);
    check(!straight.hit, "the doorway is a hole for a plain ray too");

    RayHit through = f.world->trace(from, to, 0xFFFFFFFF);
    check(through.portals_crossed >= 1, "the trace went through the portal",
          double(through.portals_crossed));
    if (through.portals_crossed >= 1) {
        // Whatever it hit must be at the far end of the world.
        check(through.hit ? std::fabs(through.position.x - 200.0f) < 60.0f : true,
              "and came out over there",
              through.hit ? double(through.position.x) : 0.0);
        near_check(through.total_warp.basis.uniform_scale(), 4.0f, 1e-2f,
                   "the reported warp carries the size ratio");
    }

    // A ray that misses the aperture must behave exactly as the plain
    // one does.
    Vec3 wide_from(7.0f, 1.0f, 2.0f);
    Vec3 wide_to(7.0f, 1.0f, -40.0f);
    RayHit a = f.world->raycast(wide_from, wide_to, 0xFFFFFFFF);
    RayHit b = f.world->trace(wide_from, wide_to, 0xFFFFFFFF);
    check(a.hit == b.hit, "a ray that misses the aperture is unaffected");
    if (a.hit && b.hit)
        near_check((a.position - b.position).length(), 0.0f, 1e-3f,
                   "and hits the same place");
}

// WHAT THE ENTRANCE DECIDES.
//
// The exit decides what size you come out at. The entrance decides
// whether you get in at all, and that second rule is what turns an
// unequal pair from a curiosity into a gate: a small hole is an
// obstacle until you are small, and making yourself too big to get
// back is exactly the mistake the mechanic invites.
//
// The half that is easy to forget is the COLLISION. A body refused
// the crossing has to be stopped by a wall; if the aperture is
// still a hole in the geometry for it, it walks into the doorway,
// is refused the teleport, and stands inside the wall.
void test_too_big_does_not_fit() {
    section("a body too big for the hole does not get through it");
    Fixture f(0.5f, 8.0f);
    const float before = f.body->global_position().z;
    f.walk(Vec3(0, 0, -1), 4.0f, 4.0f);
    near_check(f.body->get_size(), 1.0f, 1e-4f, "it is still its own size");
    check(f.body->global_position().x < 50.0f,
          "and it is still on this side of the map",
          double(f.body->global_position().x));
    // Stopped SHORT of the aperture, not standing in it. The wall
    // is at z = -6 and the portal is set into it.
    check(f.body->global_position().z > -6.0f,
          "and it was stopped by the wall rather than left inside it",
          double(f.body->global_position().z));
    (void)before;

    // And the same pair, entered from the big end, lets it through
    // -- so the refusal is about the size of the hole and not
    // about the pair being unequal.
    Fixture g(8.0f, 0.5f);
    g.walk(Vec3(0, 0, -1), 4.0f, 4.0f);
    check(g.body->get_size() < 0.5f,
          "while the same body fits the big end and comes out tiny",
          double(g.body->get_size()));
}

void test_equal_pair_does_not_resize() {
    section("an equal pair changes nothing but where you are");
    Fixture f(2.0f, 2.0f);
    near_check(Portal3D::scale_ratio(f.small, f.large), 1.0f, 1e-5f, "1:1");
    f.walk(Vec3(0, 0, -1), 4.0f, 4.0f);
    near_check(f.body->get_size(), 1.0f, 1e-4f, "still 1x after going through");
    check(std::fabs(f.body->global_position().x - 200.0f) < 12.0f,
          "but somewhere else entirely");
}

void test_size_is_clamped() {
    section("the size cannot run away");
    Fixture f(0.5f, 8.0f);   // 16x each way
    f.body->max_size = 6.0f;
    // SMALL ENOUGH TO GET IN. A half-metre aperture will not take
    // a body six tenths of a metre across, and since the entrance
    // gained a fit rule it does not pretend to -- so growing
    // sixteenfold now starts from something that fits through the
    // small end. Which is the mechanic: you have to be small to
    // use the small hole.
    f.body->set_size(0.5f);
    f.walk(Vec3(0, 0, -1), 4.0f, 4.0f);
    check(f.body->get_size() <= 6.0f + 1e-4f, "clamped at the maximum",
          double(f.body->get_size()));
    check(f.body->get_size() > 0.5f, "but it did grow", double(f.body->get_size()));
}

// TRIANGLE MESHES, WHICH ARE WHAT A LEVEL IS MADE OF.
//
// Every other test here uses primitive colliders, and primitives do
// not go through the BVH. A BVH that returns nothing makes all mesh
// collision silently vanish -- the level is still drawn, and the
// player falls through it -- so it gets its own test.
void test_mesh_collision() {
    section("triangle mesh colliders");
    Ref<PhysicsWorld> world(new PhysicsWorld());
    Ref<Mesh> floor = Mesh::box({10.0f, 0.25f, 10.0f});
    ColliderId id = world->add_mesh(*floor, Transform3D(Vec3(0, 0, 0)), 1, nullptr);
    check(id.valid(), "the mesh collider was made");
    check(world->collider_count() == 1, "and registered");

    // Straight down, from well above. The top face is at y = 0.125.
    RayHit r = world->raycast(Vec3(0, 5, 0), Vec3(0, -5, 0), 0xFFFFFFFF);
    check(r.hit, "a ray finds the mesh");
    if (r.hit) {
        near_check(r.position.y, 0.125f, 1e-2f, "at the top face");
        near_check(dot(r.normal, Vec3::up()), 1.0f, 1e-2f, "with an upward normal");
    }
    // And off the end of it, which must miss.
    check(!world->raycast(Vec3(40, 5, 0), Vec3(40, -5, 0), 0xFFFFFFFF).hit,
          "and misses beside it");

    // A capsule swept down from a gap must stop at the surface, not
    // at zero and not at one.
    Shape cap = Shape::capsule(0.32f, 1.1f);
    Transform3D at(Vec3(0, 0.3f + 1.1f * 0.5f + 0.32f, 0));
    SweepHit s = world->sweep(cap, at, Vec3(0, -0.3f, 0), 0xFFFFFFFF, nullptr);
    check(s.hit, "a swept capsule finds the mesh");
    // 0.175 of the 0.3 closes the gap.
    if (s.hit) near_check(s.fraction, 0.175f / 0.3f, 0.05f, "at the right fraction");

    // And one already inside is pushed back out.
    Transform3D sunk(Vec3(0, 0.0f + 1.1f * 0.5f + 0.32f, 0));
    Vec3 correction;
    int contacts = world->depenetrate(cap, sunk, 0xFFFFFFFF, nullptr, &correction);
    check(contacts > 0, "an overlapping capsule reports a contact");
    near_check(correction.y, 0.125f, 2e-2f, "and is pushed out by the overlap");

    // Every triangle of the box must be reachable, or the BVH is
    // finding only the subtree the root happens to point at.
    int faces_found = 0;
    const Vec3 dirs[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                          {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const Vec3 &d : dirs)
        if (world->raycast(d * 8.0f, -d * 8.0f, 0xFFFFFFFF).hit) faces_found++;
    check(faces_found == 6, "all six faces of the box are reachable",
          double(faces_found));
}

}  // namespace

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    log_set_level(LogLevel::Warn);
    ClassDB::register_all();

    std::printf("portal traversal\n");
    test_warp_algebra();
    test_walking_through();
    test_coming_back();
    test_speed_is_preserved();
    test_ray_through_a_portal();
    test_equal_pair_does_not_resize();
    test_too_big_does_not_fit();
    test_size_is_clamped();
    test_mesh_collision();
    std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
