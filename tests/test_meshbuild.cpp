// Warren -- building geometry, and standing it on the ground.
//
// The arithmetic in a mesh builder is trivial and the mistakes it
// invites are not. A box is placed by its CENTRE and a wall is
// thought of by its BASE, and the gap between those two habits is
// how a building ends up half a storey in the air -- which looks,
// from inside, exactly like a building.
//
// So the checks here are on bounds and on where surfaces actually
// are, not on vertex counts. And the last one bakes navigation over
// the result, because a floor that is a few centimetres out still
// renders and still stops a ray, and the first thing that notices
// is a body standing in it.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/bind.h"
#include "nav/navmesh.h"
#include "render/mesh_builder.h"

using namespace wr;

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
void check_int(int got, int want, const char *what) {
    g_checks++;
    if (got != want) {
        std::printf("  FAIL  %s: got %d, wanted %d\n", what, got, want);
        g_fail++;
    }
}

// Every triangle of a mesh, flattened, which is what the navigation
// bake takes.
std::vector<Vec3> triangles_of(const Mesh &m) {
    std::vector<Vec3> t;
    t.reserve(m.indices.size());
    for (uint32_t i : m.indices) t.push_back(m.vertices[i].position);
    return t;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ClassDB::register_all();

    // ------------------------------------------------------- a box
    {
        MeshBuilder b;
        b.add_box(Vec3(2, 3, 4), Vec3(2, 2, 2));
        Mesh *m = b.build();
        check(m != nullptr, "a box builds");
        check_int(m->triangle_count(), 12, "as twelve triangles");
        AABB bb = m->bounds();
        check_near(bb.min.y, 2.0f, 1e-4f, "placed by its centre, not a corner");
        check_near(bb.max.y, 4.0f, 1e-4f, "and two metres tall");
        check_near(bb.center().x, 2.0f, 1e-4f, "where it was asked for");
    }

    // ----------------------------------------------- slots and state
    //
    // A town is one mesh with a few materials, not four thousand
    // nodes with one box apiece. Slots are what make that possible,
    // and the order they come out in is what lets a caller line
    // materials up against them.
    {
        MeshBuilder b;
        b.set_slot(3);
        b.add_box(Vec3(), Vec3(1, 1, 1));
        b.set_slot(0);
        b.add_box(Vec3(4, 0, 0), Vec3(1, 1, 1));
        b.set_slot(3);
        b.add_box(Vec3(8, 0, 0), Vec3(1, 1, 1));
        Mesh *m = b.build();
        check_int(int(m->submeshes.size()), 2, "two slots make two submeshes");
        check_int(m->submeshes[0].material_slot, 3,
                  "in the order they were first used");
        check_int(m->submeshes[1].material_slot, 0, "and not sorted behind it");
        check_int(m->triangle_count(), 36, "with everything still in there");

        Array slots = b.get_slots();
        check(slots.size() == 2 && slots[0].to_int() == 3 &&
                  slots[1].to_int() == 0,
              "and the slot list says which is which");
    }

    // ------------------------------------------ a wall on the ground
    //
    // The whole reason add_wall takes the ends of a base rather than
    // two corners.
    {
        MeshBuilder b;
        b.add_wall(Vec3(0, 0, 0), Vec3(6, 0, 0), 3.0f, 0.3f);
        Mesh *m = b.build();
        AABB bb = m->bounds();
        check_near(bb.min.y, 0.0f, 1e-4f, "a wall stands on the ground");
        check_near(bb.max.y, 3.0f, 1e-4f, "and is as tall as it was told");
        check_near(bb.size().x, 6.0f, 1e-3f, "as long as it was told");
        check_near(bb.size().z, 0.3f, 1e-3f, "and as thick");

        // At an angle, which is the case an axis-aligned box cannot
        // do and the reason a wall is not just a box.
        MeshBuilder d;
        d.add_wall(Vec3(0, 1.0f, 0), Vec3(4, 1.0f, 4), 2.0f, 0.2f);
        Mesh *dm = d.build();
        AABB db = dm->bounds();
        check_near(db.min.y, 1.0f, 1e-4f, "a diagonal wall stands on its base");
        check_near(db.max.y, 3.0f, 1e-4f, "and rises from there");
        check(db.size().x > 3.9f && db.size().z > 3.9f,
              "and actually runs diagonally");
    }

    // ------------------------------------------------ a mirrored box
    //
    // A basis with a negative determinant turns every triangle
    // inside out. The result renders as a box lit from within and
    // bakes as a box with no floor, and neither is obvious.
    {
        MeshBuilder b;
        Basis flip;
        flip.col[0] = Vec3(-1, 0, 0);
        b.set_transform(Transform3D(flip, Vec3()));
        b.add_box(Vec3(), Vec3(2, 2, 2));
        Mesh *m = b.build();

        // The top face's normal must still point up. Summing the
        // area-weighted normals of the triangles whose centroid is
        // on top answers that without caring which triangles they
        // are.
        Vec3 up_sum;
        for (size_t i = 0; i + 2 < m->indices.size(); i += 3) {
            const Vec3 &a = m->vertices[m->indices[i]].position;
            const Vec3 &p = m->vertices[m->indices[i + 1]].position;
            const Vec3 &q = m->vertices[m->indices[i + 2]].position;
            if ((a.y + p.y + q.y) / 3.0f < 0.9f) continue;
            up_sum = up_sum + cross(p - a, q - a);
        }
        check(up_sum.y > 0.0f,
              "a mirrored box still has its top face pointing up");
    }

    // ---------------------------------------------------- a room
    //
    // And then navigation over it, because "the floor is where I
    // said" is a claim about a surface and the thing that measures
    // surfaces is the bake.
    {
        MeshBuilder b;
        // A room ten by eight, floor surface at y = 0, walls 3 m.
        b.add_room(AABB(Vec3(-5, -0.5f, -4), Vec3(5, 0, 4)), 3.0f, 0.3f);
        Mesh *m = b.build();
        AABB bb = m->bounds();
        check_near(bb.max.y, 3.0f, 1e-3f, "the walls reach the room's height");
        check_near(bb.min.y, -0.5f, 1e-3f, "and the slab is under the floor");

        nav::BakeSettings s;
        s.agent.radius = 0.4f;
        s.agent.height = 1.8f;
        s.cell_size = 0.2f;
        nav::NavMesh navmesh;
        nav::BakeStats st;
        check(navmesh.bake(triangles_of(*m), bb.grown(1.0f), s, &st),
              "the room bakes");
        check(st.polys > 0, "and has floor in it");

        // THE FLOOR IS AT ZERO, which is the claim add_room makes
        // and the one that is easy to get half a slab wrong.
        Vec3 on;
        check(navmesh.nearest_point(Vec3(0, 0.5f, 0), Vec3(1, 2, 1), &on),
              "there is somewhere to stand in the middle");
        check_near(on.y, 0.0f, 0.2f, "at the height the plan named");

        // And the walls are walls: the navmesh stops inside them,
        // and nothing outside is reachable from inside.
        check(navmesh.find_poly(Vec3(0, 0, 0), Vec3(0.5f, 1, 0.5f)) !=
                  nav::kNoPoly,
              "the middle of the room is walkable");
        check(navmesh.find_poly(Vec3(8, 0, 0), Vec3(0.5f, 1, 0.5f)) ==
                  nav::kNoPoly,
              "and outside the room is not");
        Vec3 hit;
        check(!navmesh.raycast(Vec3(0, 0, 0), Vec3(0, 0, 6), &hit),
              "a ray from the middle stops at a wall");
    }

    // ---------------------------------------------------- stairs
    //
    // Treads from the ground up rather than floating, because the
    // bake decides what can be climbed from the top of the solid
    // below and a floating tread has a drop beside it.
    {
        MeshBuilder b;
        b.add_box(Vec3(0, -0.25f, -3), Vec3(4, 0.5f, 6));      // bottom landing
        b.add_stairs(AABB(Vec3(-2, 0, 0), Vec3(2, 2.4f, 6)), 8, false);
        b.add_box(Vec3(0, 1.15f, 9), Vec3(4, 2.5f, 6));        // top landing
        Mesh *m = b.build();

        nav::BakeSettings s;
        s.agent.radius = 0.4f;
        s.agent.height = 1.8f;
        s.agent.max_climb = 0.45f;
        s.cell_size = 0.2f;
        nav::NavMesh navmesh;
        check(navmesh.bake(triangles_of(*m), m->bounds().grown(1.0f), s,
                           nullptr),
              "a flight of stairs bakes");

        std::vector<nav::PathPoint> path;
        bool partial = true;
        check(navmesh.find_path(Vec3(0, 0, -4), Vec3(0, 2.4f, 10), &path,
                                &partial),
              "and a route up it is found");
        check(!partial, "that reaches the top landing");
    }

    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
