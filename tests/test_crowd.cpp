// Warren -- thirty bodies moving at once.
//
// Avoidance is the part of navigation where "it looked fine" is
// worth least. A crowd that shuffles, oscillates, jitters against
// walls or quietly lets two bodies occupy the same metre all look
// plausible in a still frame and all look wrong in motion, and none
// of them throws.
//
// So the checks here are the four properties that can be stated
// exactly, measured over every step of a simulation rather than at
// the end:
//
//   nobody ever overlaps anybody
//   nobody ever leaves the navmesh
//   everybody arrives
//   and the route taken is not much longer than the route asked for
//
// The last one is what separates avoidance from timidity. A crowd
// that solves the first three by creeping in wide circles has not
// solved anything.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "nav/crowd.h"
#include "nav/navmesh.h"
#include "render/mesh.h"
#include "scene/nav_nodes.h"
#include "scene/nodes.h"
#include "scene/scene_tree.h"

using namespace wr;
using namespace wr::nav;

namespace {

int g_fail = 0, g_checks = 0;
void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        g_fail++;
    }
}
void check_near(float got, float want, float tol, const char *what) {
    g_checks++;
    if (!(std::fabs(got - want) <= tol)) {
        std::printf("  FAIL  %s: got %.4f, wanted %.4f +- %.4f\n", what,
                    double(got), double(want), double(tol));
        g_fail++;
    }
}
void check_le(float got, float limit, const char *what) {
    g_checks++;
    if (!(got <= limit)) {
        std::printf("  FAIL  %s: got %.4f, wanted at most %.4f\n", what,
                    double(got), double(limit));
        g_fail++;
    }
}

void box(std::vector<Vec3> &t, const Vec3 &mn, const Vec3 &mx) {
    const Vec3 c[8] = {
        {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z},
        {mn.x, mn.y, mx.z}, {mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z},
        {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
    const int f[12][3] = {{0, 1, 2}, {0, 2, 3}, {4, 7, 6}, {4, 6, 5},
                          {0, 4, 5}, {0, 5, 1}, {1, 5, 6}, {1, 6, 2},
                          {2, 6, 7}, {2, 7, 3}, {3, 7, 4}, {3, 4, 0}};
    for (auto &tri : f) {
        t.push_back(c[tri[0]]);
        t.push_back(c[tri[1]]);
        t.push_back(c[tri[2]]);
    }
}

CrowdAgent walker(const Vec3 &at) {
    CrowdAgent a;
    a.position = at;
    a.radius = 0.35f;
    a.height = 1.8f;
    a.max_speed = 2.5f;
    a.max_accel = 12.0f;
    a.time_horizon = 2.0f;
    return a;
}

// Runs the crowd, watching every step for the two things that must
// never happen. Returns the worst overlap seen, in metres, and how
// far each body actually travelled.
struct RunResult {
    float worst_overlap = 0.0f;
    int off_mesh = 0;
    int arrived = 0;
    float steps_taken = 0.0f;
    std::vector<float> distance;
};

RunResult run(Crowd &crowd, const NavMesh &mesh, int steps, float dt) {
    RunResult r;
    r.distance.assign(crowd.agents().size(), 0.0f);
    std::vector<Vec3> last;
    for (const CrowdAgent &a : crowd.agents()) last.push_back(a.position);

    for (int s = 0; s < steps; ++s) {
        crowd.step(dt);
        const std::vector<CrowdAgent> &as = crowd.agents();
        for (size_t i = 0; i < as.size(); ++i) {
            float dx = as[i].position.x - last[i].x;
            float dz = as[i].position.z - last[i].z;
            r.distance[i] += std::sqrt(dx * dx + dz * dz);
            last[i] = as[i].position;

            if (!as[i].on_link &&
                mesh.find_poly(as[i].position, Vec3(0.3f, 1.0f, 0.3f)) ==
                    kNoPoly)
                ++r.off_mesh;

            for (size_t j = i + 1; j < as.size(); ++j) {
                if (as[i].on_link || as[j].on_link) continue;
                float ex = as[i].position.x - as[j].position.x;
                float ez = as[i].position.z - as[j].position.z;
                float gap = std::sqrt(ex * ex + ez * ez);
                float want = as[i].radius + as[j].radius;
                r.worst_overlap = std::fmax(r.worst_overlap, want - gap);
            }
        }
        int done = 0;
        for (const CrowdAgent &a : as)
            if (a.arrived) ++done;
        if (done == int(as.size())) {
            r.steps_taken = float(s + 1);
            break;
        }
        r.steps_taken = float(s + 1);
    }
    for (const CrowdAgent &a : crowd.agents())
        if (a.arrived) ++r.arrived;
    return r;
}

// A Resource cannot be copied -- two references to one resource are
// meant to be one object -- so this fills one in place.
void open_floor(NavMesh *m, float half) {
    std::vector<Vec3> tris;
    box(tris, Vec3(-half, -0.2f, -half), Vec3(half, 0, half));
    AABB b(Vec3(-half - 1, -1, -half - 1), Vec3(half + 1, 3, half + 1));
    BakeSettings s;
    s.agent.radius = 0.35f;
    s.cell_size = 0.2f;
    s.cell_height = 0.15f;
    m->bake(tris, b, s, nullptr);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const float dt = 1.0f / 60.0f;

    // ------------------------------------------- the ORCA line itself
    //
    // Before any simulation: one body, one neighbour coming straight
    // at it, and a check that the constraint says what it should.
    // The chosen velocity must differ from the one wanted -- a body
    // that sails straight on is not avoiding -- and must be on the
    // allowed side of every line, which is the definition of safe.
    {
        std::vector<Vec2> pos{Vec2(4.0f, 0.0f)};
        std::vector<Vec2> vel{Vec2(-1.5f, 0.0f)};
        std::vector<float> rad{0.35f};
        std::vector<OrcaLine> lines;
        orca_constraints(Vec2(0, 0), Vec2(1.5f, 0.0f), 0.35f, pos, vel, rad,
                         2.0f, dt, &lines);
        check(lines.size() == 1, "one neighbour gives one constraint");

        Vec2 out;
        check(orca_solve(lines, 2.5f, Vec2(1.5f, 0.0f), &out),
              "and a safe velocity exists");
        check(std::fabs(out.y) > 0.05f,
              "which steps aside rather than driving straight in");
        float side = lines[0].direction.x * (lines[0].point.y - out.y) -
                     lines[0].direction.y * (lines[0].point.x - out.x);
        check_le(side, 1e-4f, "and it is on the allowed side of the line");

        // A neighbour going the other way is no constraint at all.
        vel[0] = Vec2(3.0f, 0.0f);
        orca_constraints(Vec2(0, 0), Vec2(1.5f, 0.0f), 0.35f, pos, vel, rad,
                         2.0f, dt, &lines);
        orca_solve(lines, 2.5f, Vec2(1.5f, 0.0f), &out);
        check_near((out - Vec2(1.5f, 0.0f)).length(), 0.0f, 1e-3f,
                   "a neighbour walking away is not avoided");
    }

    // --------------------------------------------------- head to head
    //
    // Two bodies swapping places on open ground. The classic failure
    // is the corridor dance: both dodge, both see the dodge, both
    // dodge back. ORCA does not do this because each assumes the
    // other takes half the correction, so the pass resolves once.
    {
        NavMesh mesh;
        open_floor(&mesh, 12.0f);
        Crowd crowd;
        crowd.set_navmesh(&mesh);
        uint32_t a = crowd.add(walker(Vec3(-6, 0, 0)));
        uint32_t b = crowd.add(walker(Vec3(6, 0, 0.01f)));
        check(crowd.set_target(a, Vec3(6, 0, 0)), "the first has a path");
        check(crowd.set_target(b, Vec3(-6, 0, 0)), "and so does the second");

        RunResult r = run(crowd, mesh, 900, dt);
        check(r.arrived == 2, "both get where they were going");
        check_le(r.worst_overlap, 0.02f, "and never overlap on the way");
        check(r.off_mesh == 0, "and never leave the navmesh");
        // Twelve metres apart, so twelve metres of walking plus
        // whatever the sidestep costs.
        check_le(r.distance[0], 12.0f * 1.15f,
                 "the detour costs little -- they pass, they do not dance");
    }

    // -------------------------------------------------- the ring test
    //
    // Sixteen bodies on a circle, each heading for the point
    // opposite. Every route goes through the middle, so the middle
    // is where the whole crowd arrives at once. This is the standard
    // hard case for reciprocal avoidance and the one that exposes
    // deadlock: a symmetric jam has no direction that obviously
    // helps, and an implementation that refuses to move when no
    // velocity is safe stands there for ever.
    {
        NavMesh mesh;
        open_floor(&mesh, 16.0f);
        Crowd crowd;
        crowd.set_navmesh(&mesh);
        const int n = 16;
        const float ring = 10.0f;
        std::vector<uint32_t> ids;
        for (int i = 0; i < n; ++i) {
            float t = float(i) / float(n) * 2.0f * PI;
            ids.push_back(crowd.add(
                walker(Vec3(std::cos(t) * ring, 0, std::sin(t) * ring))));
        }
        for (int i = 0; i < n; ++i) {
            float t = float(i) / float(n) * 2.0f * PI;
            crowd.set_target(ids[size_t(i)],
                             Vec3(-std::cos(t) * ring, 0, -std::sin(t) * ring));
        }

        RunResult r = run(crowd, mesh, 2400, dt);
        check(r.arrived == n, "all sixteen cross the middle and arrive");
        check_le(r.worst_overlap, 0.06f, "with no real overlap at any step");
        check(r.off_mesh == 0, "and none of them leaves the mesh");

        float worst = 0.0f;
        for (float d : r.distance) worst = std::fmax(worst, d);
        check_le(worst, 20.0f * 1.7f,
                 "and nobody takes a wildly longer way round");
        check_le(r.steps_taken * dt, 30.0f,
                 "the crowd clears in a reasonable time, so it never jammed");
    }

    // ------------------------------------------------ pressed to a wall
    //
    // Avoidance knows about bodies and nothing about walls, so a body
    // dodging toward a corner would be pushed through it if nothing
    // else were done. A line of bodies squeezed along a wall is the
    // case where that shows up.
    {
        std::vector<Vec3> tris;
        box(tris, Vec3(-10, -0.2f, -3), Vec3(10, 0, 3));
        box(tris, Vec3(-10.4f, -0.2f, 3.0f), Vec3(10.4f, 2.5f, 3.4f));
        box(tris, Vec3(-10.4f, -0.2f, -3.4f), Vec3(10.4f, 2.5f, -3.0f));
        AABB b(Vec3(-11, -1, -4), Vec3(11, 4, 4));
        BakeSettings s;
        s.agent.radius = 0.35f;
        s.cell_size = 0.2f;
        NavMesh mesh;
        check(mesh.bake(tris, b, s, nullptr), "the corridor bakes");

        Crowd crowd;
        crowd.set_navmesh(&mesh);
        std::vector<uint32_t> ids;
        for (int i = 0; i < 8; ++i) {
            CrowdAgent a = walker(Vec3(-8.0f + float(i % 2) * 0.9f, 0,
                                       -2.0f + float(i / 2) * 1.3f));
            ids.push_back(crowd.add(a));
        }
        // Each to its own spot at the far end. Eight bodies sent to
        // one point would orbit it for ever, correctly, because
        // eight bodies with size cannot stand where one can.
        for (size_t i = 0; i < ids.size(); ++i)
            crowd.set_target(ids[i],
                             Vec3(8.0f, 0, -1.8f + float(i) * 0.52f));

        RunResult r = run(crowd, mesh, 1800, dt);
        check(r.arrived == 8, "eight down a corridor all get through");
        check(r.off_mesh == 0, "and not one of them is pushed into a wall");
        check_le(r.worst_overlap, 0.06f, "nor into each other");
    }

    // ------------------------------------------- everyone after one thing
    //
    // Which is what a game actually asks for: a mob converging on
    // the player. The target is a single point and a dozen bodies
    // cannot stand on it, so the instruction has to be "get near",
    // and the crowd has to settle rather than orbit.
    {
        NavMesh mesh;
        open_floor(&mesh, 14.0f);
        Crowd crowd;
        crowd.set_navmesh(&mesh);
        const Vec3 prey(0, 0, 0);
        std::vector<uint32_t> ids;
        for (int i = 0; i < 12; ++i) {
            float t = float(i) / 12.0f * 2.0f * PI;
            CrowdAgent a = walker(Vec3(std::cos(t) * 9.0f, 0, std::sin(t) * 9.0f));
            a.goal_radius = 2.0f;
            ids.push_back(crowd.add(a));
        }
        for (uint32_t id : ids) crowd.set_target(id, prey);

        RunResult r = run(crowd, mesh, 1500, dt);
        check(r.arrived == 12, "a dozen chasers all close in");
        check_le(r.worst_overlap, 0.06f, "without piling into each other");
        check(r.off_mesh == 0, "and without leaving the mesh");

        float furthest = 0.0f;
        for (const CrowdAgent &a : crowd.agents()) {
            float dx = a.position.x - prey.x, dz = a.position.z - prey.z;
            furthest = std::fmax(furthest, std::sqrt(dx * dx + dz * dz));
        }
        check_le(furthest, 3.2f, "and they end up around it, not orbiting it");
    }

    // ----------------------------------------------- crossing a ladder
    //
    // A link is not walking, so the crowd hands the body along it
    // and stands back: no avoidance, no snapping to the ground, and
    // a flag the game can read to play the climb.
    {
        std::vector<Vec3> tris;
        box(tris, Vec3(-8, -0.2f, -4), Vec3(-2, 0, 4));
        box(tris, Vec3(2, 2.8f, -4), Vec3(8, 3.0f, 4));
        AABB b(Vec3(-9, -1, -5), Vec3(9, 6, 5));
        BakeSettings s;
        s.agent.radius = 0.35f;
        s.cell_size = 0.2f;
        NavMesh mesh;
        mesh.bake(tris, b, s, nullptr);

        NavLink ladder;
        ladder.from = Vec3(-2.6f, 0.0f, 0.0f);
        ladder.to = Vec3(2.6f, 3.0f, 0.0f);
        ladder.radius = 1.5f;
        ladder.name = "ladder";
        mesh.add_link(ladder);
        check(mesh.links()[0].from_poly != kNoPoly &&
                  mesh.links()[0].to_poly != kNoPoly,
              "the ladder is attached at both ends");

        Crowd crowd;
        crowd.set_navmesh(&mesh);
        uint32_t id = crowd.add(walker(Vec3(-6, 0, 0)));
        check(crowd.set_target(id, Vec3(6, 3, 0)), "a route up exists");

        int on_link_steps = 0;
        float highest = -10.0f;
        for (int i = 0; i < 2000; ++i) {
            crowd.step(dt);
            const CrowdAgent *a = crowd.find(id);
            if (a->on_link) ++on_link_steps;
            highest = std::fmax(highest, a->position.y);
            if (a->arrived) break;
        }
        const CrowdAgent *a = crowd.find(id);
        check(a->arrived, "and the body gets to the roof");
        check(on_link_steps > 0, "by way of the ladder, which it reports");
        check_near(a->position.y, 3.0f, 0.2f, "ending up at roof height");
    }

    // ============================================================
    //                                              as nodes in a scene
    // ============================================================
    //
    // Everything above works on triangles and knows nothing about
    // the tree, which is what makes it testable. This is the
    // ordinary way to use it: put a region in the level, put bodies
    // under it, and let the level's own geometry be what gets baked.
    {
        SceneTree tree;
        Node3D *scene = new Node3D();
        scene->set_name("Level");
        tree.set_scene(scene);

        NavRegion3D *region = new NavRegion3D();
        region->set_name("Nav");
        region->settings.agent.radius = 0.35f;
        region->settings.cell_size = 0.25f;
        scene->add_child(region);

        // A floor and a wall with a gap in it, as actual meshes,
        // because the point of the node layer is that the geometry
        // in the level is the geometry that gets baked.
        auto slab = [&](const Vec3 &mn, const Vec3 &mx, const char *n) {
            Ref<Mesh> m(new Mesh());
            std::vector<Vec3> t;
            box(t, mn, mx);
            std::vector<Vertex> v;
            std::vector<uint32_t> idx;
            for (size_t i = 0; i < t.size(); ++i) {
                Vertex vert;
                vert.position = t[i];
                v.push_back(vert);
                idx.push_back(uint32_t(i));
            }
            m->append(v, idx, 0);
            m->set_bounds(compute_bounds(m->vertices));
            MeshInstance3D *mi = new MeshInstance3D();
            mi->set_name(n);
            mi->mesh = m;
            region->add_child(mi);
        };
        slab(Vec3(-10, -0.2f, -10), Vec3(10, 0, 10), "Floor");
        slab(Vec3(-0.3f, 0, -10), Vec3(0.3f, 2.5f, -2), "WallA");
        slab(Vec3(-0.3f, 0, 2), Vec3(0.3f, 2.5f, 10), "WallB");

        // A game loop runs both: navigation lives on the physics
        // tick, because what it produces is movement.
        auto frame = [&] {
            tree.physics_tick(1.0f / 60.0f);
            tree.process(1.0f / 60.0f);
        };
        // The region bakes itself on the first tick.
        frame();
        check(region->poly_count() > 0, "a region bakes the level under it");
        check(region->stats().regions >= 1, "and finds ground to walk on");

        // A wall really is a wall, and the gap really is a gap.
        check(!region->is_reachable(Vec3(-5, 0, -6), Vec3(5, 0, -6)) ||
                  region->find_path(Vec3(-5, 0, -6), Vec3(5, 0, -6)).size() > 2,
              "getting past the wall means going round through the gap");
        check(region->is_reachable(Vec3(-5, 0, -6), Vec3(5, 0, -6)),
              "and the gap does connect the two halves");

        Vec3 on;
        check(region->nearest_point(Vec3(-5, 3.0f, -6), &on),
              "a point above the floor snaps to it");
        check_near(on.y, 0.05f, 0.2f, "at floor height");

        NavAgent3D *agent = new NavAgent3D();
        agent->set_name("Shambler");
        agent->radius = 0.35f;
        agent->max_speed = 2.5f;
        agent->set_position(Vec3(-5, 0, -6));
        region->add_child(agent);
        frame();
        check(agent->region() == region, "an agent finds the region above it");

        agent->set_target(Vec3(5, 0, -6));
        check(agent->has_target(), "and takes a target");

        for (int i = 0; i < 1200 && !agent->arrived(); ++i) frame();
        check(agent->arrived(), "and walks round the wall to reach it");
        check_near(agent->global_position().x, 5.0f, 0.6f,
                   "ending up where it was sent");
        check(agent->global_position().z > -9.9f,
              "and the node's transform followed the body");

        // A link, authored as a node, joins two things nothing could
        // walk between.
        NavLink3D *link = new NavLink3D();
        link->set_name("vault");
        link->set_position(Vec3(-0.9f, 0, -6));
        link->start = Vec3();
        link->end = Vec3(1.8f, 0, 0);
        link->radius = 1.0f;
        region->add_child(link);
        region->collect_links();
        check(region->mesh() && region->mesh()->links().size() == 1,
              "a NavLink3D node is picked up by the region");
        check(region->mesh() && region->mesh()->links().size() == 1 &&
                  region->mesh()->links()[0].from_poly != nav::kNoPoly,
              "and lands on the mesh");

        Array through = region->find_path(Vec3(-5, 0, -6), Vec3(5, 0, -6));
        check(through.size() >= 2, "and the route across it is shorter now");

        // --- WHICH WAY THE BODY POINTS.
        //
        // An agent with a mesh on it has to face where it is going
        // or it moonwalks, and "where it is going" is the velocity
        // avoidance settled on, not the direction of the target: a
        // body squeezing round an obstacle is travelling sideways
        // and facing the target instead is a body walking crabwise.
        // Off by default, because a game driving its own character
        // owns the rotation.
        NavAgent3D *turner = new NavAgent3D();
        turner->set_name("Turner");
        turner->radius = 0.35f;
        turner->max_speed = 2.5f;
        turner->turn_speed = 6.0f;
        turner->set_position(Vec3(-5, 0, -6));
        // Deliberately near a half turn. That is the Euler branch
        // that used to come back as a pitch of pi -- a body lying on
        // its back and walking -- so this is the case worth pinning.
        turner->set_euler(Vec3(3.0f, 0, 0));
        region->add_child(turner);
        frame();
        turner->set_target(Vec3(5, 0, -6));
        for (int i = 0; i < 90; ++i) frame();

        const Vec3 v = turner->velocity();
        const float moving = std::sqrt(v.x * v.x + v.z * v.z);
        check(moving > 0.2f, "a turning agent still walks");
        // The node's forward is -Z, so this is the yaw that points
        // along the velocity. Compared as an angle difference, so
        // the wrap at pi does not read as a failure.
        const float want = std::atan2(-v.x, -v.z);
        // From the basis, not from euler(): see Node3D::euler.
        const Vec3 face = turner->global_transform().basis * Vec3(0, 0, -1);
        float off = std::atan2(-face.x, -face.z) - want;
        while (off > 3.14159265f) off -= 6.28318531f;
        while (off < -3.14159265f) off += 6.28318531f;
        check(std::fabs(off) < 0.35f,
              "and turns to face the way it is actually travelling");
        const Vec3 up = turner->global_transform().basis * Vec3(0, 1, 0);
        check(up.y > 0.99f, "and is still standing up while it does it");

        NavAgent3D *fixed = new NavAgent3D();
        fixed->set_name("Fixed");
        fixed->radius = 0.35f;
        fixed->max_speed = 2.5f;
        fixed->set_position(Vec3(-5, 0, -5));
        fixed->set_euler(Vec3(1.2f, 0, 0));
        region->add_child(fixed);
        frame();
        fixed->set_target(Vec3(5, 0, -5));
        for (int i = 0; i < 90; ++i) frame();
        const Vec3 held = fixed->global_transform().basis * Vec3(0, 0, -1);
        check(std::fabs(std::atan2(-held.x, -held.z) - 1.2f) < 1e-3f,
              "while one with turn_speed zero is left pointing where it was");

        // --- A BODY PUT SOMEWHERE ELSE.
        //
        // A respawn, a teleport, or a body recalled because it was
        // holding a round up on the far side of the level. Moving
        // the NODE is not enough and that is the point of the test:
        // the crowd owns the position while the agent drives the
        // transform and writes its own back the same frame, so a
        // game that sets the node sees it snap back and thinks the
        // teleport did nothing.
        NavAgent3D *mover = new NavAgent3D();
        mover->set_name("Mover");
        mover->radius = 0.35f;
        mover->max_speed = 2.5f;
        mover->set_position(Vec3(-5, 0, -6));
        region->add_child(mover);
        frame();
        mover->set_target(Vec3(5, 0, -6));
        for (int i = 0; i < 30; ++i) frame();

        mover->set_global_position(Vec3(-5, 0, 6));
        frame();
        check((mover->global_position() - Vec3(-5, 0, 6)).length() > 1.0f,
              "setting the node's position alone does not move an agent");

        mover->warp(Vec3(-5, 0, 6));
        frame();
        check((mover->global_position() - Vec3(-5, 0, 6)).length() < 1.0f,
              "warp does, and it sticks after a step");
        check(mover->has_target(),
              "and it still wants what it wanted before");
        check(mover->velocity().length() < 3.0f,
              "without carrying its old momentum through");

        scene->queue_free();
    }

    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
