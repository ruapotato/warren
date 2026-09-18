// Warren -- shapes described as formulas.
//
// The procedural tools exist so that most of a game's props can be
// written rather than modelled. That only works if the shapes come
// out RIGHT, and "right" here has a precise meaning that is worth
// stating, because it is what the whole thing rests on.
//
// A signed distance field must be LIPSCHITZ: moving a metre may
// change the distance by at most a metre. The contourer walks the
// field in steps and assumes this; a field that overstates a
// distance makes it step over a thin wall, and the wall vanishes
// with no error anywhere. Every deformation in sdf.cpp -- twist,
// bend, displace -- breaks the property unless it is corrected, so
// the test measures it directly, over every shape, from a great
// many pairs of points.
//
// After that: that the booleans mean what they say, measured against
// an analytic answer rather than against themselves; that the mesh
// is closed, because a mesh with a hole in it is a mesh that will
// leak light and fail a physics query; and that the written form of
// a shape survives a round trip, because that is how an agent will
// send one.
#include <cmath>
#include <cstdio>
#include <array>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/log.h"
#include "procgen/sdf.h"
#include "render/mesh.h"

using namespace wr;
using namespace wr::gen;

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
        std::printf("  FAIL  %s: got %.5f, wanted %.5f +- %.5f\n", what, double(got),
                    double(want), double(tol));
        g_fail++;
    }
}

std::mt19937 g_rng(20260918);
float uniform(float lo, float hi) {
    return lo + (hi - lo) * float(g_rng() & 0xFFFFFF) / float(0xFFFFFF);
}
Vec3 random_point(float r) { return {uniform(-r, r), uniform(-r, r), uniform(-r, r)}; }

// The property the contourer depends on. Returns the worst ratio
// found: anything above 1 means the field overstates a distance
// somewhere, and the mesher may step straight over a feature.
float worst_lipschitz(const Sdf &s, float range, int samples) {
    float worst = 0.0f;
    for (int i = 0; i < samples; i++) {
        const Vec3 a = random_point(range);
        // Both near neighbours and distant ones: a deformer can be
        // well behaved locally and badly behaved across the shape.
        const Vec3 b = i % 2 ? a + random_point(0.05f) : random_point(range);
        const float step = (b - a).length();
        if (step < 1e-5f) continue;
        const float ratio = std::fabs(s.distance(a) - s.distance(b)) / step;
        worst = std::max(worst, ratio);
    }
    return worst;
}

// The volume a mesh encloses, by the divergence theorem: the signed
// volume of the tetrahedra from the origin to each triangle. Only
// correct for a closed, consistently wound surface -- which is
// exactly why it is worth comparing with the analytic answer.
double mesh_volume(const Mesh &m) {
    double v = 0.0;
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const Vec3 &a = m.vertices[m.indices[i]].position;
        const Vec3 &b = m.vertices[m.indices[i + 1]].position;
        const Vec3 &c = m.vertices[m.indices[i + 2]].position;
        v += double(dot(a, cross(b, c))) / 6.0;
    }
    return v;
}

// A closed surface has every edge shared by exactly two triangles.
// Returns how many are not.
//
// KEYED ON THE POSITION, EXACTLY. Computing normals splits a vertex
// in two at a hard edge, so counting by index would report a seam
// wherever the surface has a crease. Quantising to a grid and
// comparing the three integers is exact; hashing them into one
// number is not, and a hash that collides reports a hole in a mesh
// that has none -- which is how the first version of this function
// spent an afternoon accusing the contourer of a bug it did not have.
using PosKey = std::array<int64_t, 3>;

size_t open_edges(const Mesh &m) {
    std::map<std::pair<PosKey, PosKey>, int> edges;
    auto key = [&](uint32_t i) {
        const Vec3 &p = m.vertices[i].position;
        return PosKey{std::lround(double(p.x) * 65536.0),
                      std::lround(double(p.y) * 65536.0),
                      std::lround(double(p.z) * 65536.0)};
    };
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        for (int e = 0; e < 3; e++) {
            const PosKey a = key(m.indices[i + size_t(e)]);
            const PosKey b = key(m.indices[i + size_t((e + 1) % 3)]);
            if (a == b) continue;  // a degenerate triangle has no edge
            edges[{std::min(a, b), std::max(a, b)}]++;
        }
    }
    size_t bad = 0;
    for (const auto &kv : edges)
        if (kv.second != 2) bad++;
    return bad;
}

}  // namespace

int main() {
    log_set_level(LogLevel::Error);
    std::printf("procgen\n");

    // ---------------------------------------------- the primitives
    //
    // Distances at points where the answer is known by hand, so that
    // a wrong formula cannot hide behind a plausible picture.
    {
        const Sdf s = Sdf::sphere(1.0f);
        check_near(s.distance(Vec3()), -1.0f, 1e-5f, "the centre of a sphere is r inside");
        check_near(s.distance(Vec3(2, 0, 0)), 1.0f, 1e-5f, "and a metre out is a metre out");
        check_near(s.distance(Vec3(1, 0, 0)), 0.0f, 1e-5f, "the surface is at zero");

        const Sdf b = Sdf::box(Vec3(2, 2, 2));
        check_near(b.distance(Vec3()), -1.0f, 1e-5f, "the centre of a box is half its size in");
        check_near(b.distance(Vec3(2, 0, 0)), 1.0f, 1e-5f, "a face's distance is exact");
        check_near(b.distance(Vec3(2, 2, 0)), std::sqrt(2.0f), 1e-5f,
                   "and a corner's is the diagonal");

        // A ROUNDED BOX IS THE SIZE YOU ASKED FOR. Growing the shape
        // by the rounding radius is the obvious implementation and
        // it is wrong: nobody asking for a 1m crate with soft edges
        // wants a 1.2m crate.
        const Sdf r = Sdf::box(Vec3::one(), 0.1f);
        check_near(r.distance(Vec3(0.5f, 0, 0)), 0.0f, 1e-5f,
                   "a rounded box still measures a metre across its faces");
        check(r.distance(Vec3(0.5f, 0.5f, 0.5f)) > 0.0f,
              "and its corners are inside the box it would have been");

        const Sdf c = Sdf::cylinder(1.0f, 2.0f);
        check_near(c.distance(Vec3()), -1.0f, 1e-5f, "a cylinder's axis is r inside");
        check_near(c.distance(Vec3(0, 2, 0)), 1.0f, 1e-5f, "and its cap is exact");
        const Sdf t = Sdf::torus(1.0f, 0.25f);
        check_near(t.distance(Vec3(1, 0, 0)), -0.25f, 1e-5f, "a torus's ring is minor in");
        check_near(t.distance(Vec3()), 0.75f, 1e-5f, "and its hole is outside it");
        const Sdf cap = Sdf::capsule(0.5f, 3.0f);
        check_near(cap.distance(Vec3(0, 1.5f, 0)), 0.0f, 1e-5f,
                   "a capsule's height includes its round ends");
    }

    // ------------------------------------------------ the booleans
    //
    // Checked against the answer worked out by hand at each point,
    // not against another run of the same code.
    {
        const Sdf a = Sdf::sphere(1.0f);
        const Sdf b = Sdf::sphere(1.0f).translated(Vec3(1, 0, 0));

        const Sdf together = a.merged(b);
        check(together.distance(Vec3(-0.5f, 0, 0)) < 0, "a union contains the first");
        check(together.distance(Vec3(1.5f, 0, 0)) < 0, "and the second");
        check(together.distance(Vec3(3, 0, 0)) > 0, "and nothing outside both");

        const Sdf overlap = a.intersected(b);
        check(overlap.distance(Vec3(0.5f, 0, 0)) < 0, "an intersection keeps the overlap");
        check(overlap.distance(Vec3(-0.5f, 0, 0)) > 0, "and drops what is only in one");

        const Sdf cut = a.subtracted(b);
        check(cut.distance(Vec3(-0.5f, 0, 0)) < 0, "a difference keeps the first");
        check(cut.distance(Vec3(0.5f, 0, 0)) > 0, "and removes the second from it");

        // A SMOOTH UNION ADDS MATERIAL IN THE CREASE and nowhere
        // else. Both halves of that matter: a blend that moved the
        // surface far from the join would change the size of every
        // shape it was used on.
        const Sdf blended = a.blended(b, 0.4f);
        check(blended.distance(Vec3(0.5f, 0.9f, 0)) <
                  together.distance(Vec3(0.5f, 0.9f, 0)),
              "a blend fills the crease between two shapes");
        check_near(blended.distance(Vec3(-2, 0, 0)), together.distance(Vec3(-2, 0, 0)),
                   1e-4f, "and leaves the far side exactly where it was");

        // An empty operand is not an error: a shape built by adding
        // parts in a loop starts with nothing.
        check(Sdf().merged(a).distance(Vec3()) < 0, "a union with nothing is the shape");
        check(a.merged(Sdf()).distance(Vec3()) < 0, "either way round");
    }

    // ------------------------------------------- THE LIPSCHITZ TEST
    //
    // The one that catches the bugs that do not look like bugs.
    {
        struct Case { const char *name; Sdf shape; };
        const std::vector<Case> cases = {
            {"sphere", Sdf::sphere(1)},
            {"box", Sdf::box(Vec3(1.5f, 1, 0.7f))},
            {"rounded box", Sdf::box(Vec3::one(), 0.2f)},
            {"cylinder", Sdf::cylinder(0.6f, 1.4f)},
            {"capsule", Sdf::capsule(0.3f, 1.5f)},
            {"cone", Sdf::cone(0.7f, 1.2f)},
            {"torus", Sdf::torus(0.8f, 0.25f)},
            {"union", Sdf::sphere(0.8f).merged(Sdf::box(Vec3::one()))},
            {"difference", Sdf::box(Vec3::one()).subtracted(Sdf::sphere(0.65f))},
            {"blend", Sdf::sphere(0.6f).blended(Sdf::sphere(0.6f).translated(Vec3(0.7f, 0, 0)), 0.3f)},
            {"scaled", Sdf::sphere(1).scaled(2.5f)},
            {"rotated", Sdf::box(Vec3(1, 0.4f, 0.4f)).rotated(Vec3::up(), 0.7f)},
            {"shell", Sdf::sphere(1).shelled(0.1f)},
            {"elongated", Sdf::sphere(0.4f).elongated(Vec3(1, 0, 0))},
            {"mirrored", Sdf::sphere(0.4f).translated(Vec3(0.8f, 0, 0)).mirrored(true)},
            // The three that need the correction, and would sail
            // through every other test in this file without it.
            {"twisted", Sdf::box(Vec3(0.6f, 2, 0.6f)).twisted(0.5f)},
            {"bent", Sdf::box(Vec3(2, 0.3f, 0.3f)).bent(0.6f)},
            {"displaced", Sdf::sphere(1).displaced(0.2f, 0.5f, 4)},
        };
        float worst_overall = 0.0f;
        const char *worst_name = "";
        for (const Case &c : cases) {
            const float w = worst_lipschitz(c.shape, 3.0f, 4000);
            if (w > worst_overall) { worst_overall = w; worst_name = c.name; }
            // A little over 1 is tolerable -- the correction factors
            // are bounds, not exact -- but nothing should be wildly
            // over, and a twist without its correction reaches 3.
            if (w > 1.25f)
                std::printf("  FAIL  %s overstates distances by %.2fx\n", c.name, double(w));
            g_checks++;
            if (w > 1.25f) g_fail++;
        }
        std::printf("  worst distance overstatement: %.3fx (%s)\n", double(worst_overall),
                    worst_name);
    }

    // --------------------------------------------------- the bounds
    //
    // A wrong bounding box is how a shape comes out with its edges
    // sliced off, so the test is the one that matters: nothing
    // outside the box may be solid.
    {
        struct Case { const char *name; Sdf shape; };
        const std::vector<Case> cases = {
            {"sphere", Sdf::sphere(1)},
            {"rounded box", Sdf::box(Vec3::one(), 0.25f)},
            {"torus", Sdf::torus(0.8f, 0.3f)},
            {"rotated box", Sdf::box(Vec3(2, 0.3f, 0.3f)).rotated(Vec3::up(), 0.9f)},
            {"translated", Sdf::sphere(0.5f).translated(Vec3(2, 1, -1))},
            {"union", Sdf::sphere(0.6f).merged(Sdf::sphere(0.6f).translated(Vec3(2, 0, 0)))},
            {"blend", Sdf::sphere(0.6f).blended(Sdf::sphere(0.6f).translated(Vec3(1, 0, 0)), 0.5f)},
            {"twisted", Sdf::box(Vec3(1.2f, 2, 0.4f)).twisted(0.4f)},
            {"displaced", Sdf::sphere(1).displaced(0.3f, 0.4f)},
            {"elongated", Sdf::sphere(0.4f).elongated(Vec3(2, 0, 1))},
            {"mirrored", Sdf::box(Vec3::one()).translated(Vec3(1.5f, 0, 0)).mirrored(true)},
        };
        size_t escaped = 0;
        for (const Case &c : cases) {
            const AABB box = c.shape.bounds();
            for (int i = 0; i < 6000; i++) {
                const Vec3 p = random_point(6.0f);
                const bool inside_box = p.x >= box.min.x && p.x <= box.max.x &&
                                        p.y >= box.min.y && p.y <= box.max.y &&
                                        p.z >= box.min.z && p.z <= box.max.z;
                if (!inside_box && c.shape.distance(p) < 0.0f) {
                    if (escaped++ == 0)
                        std::printf("  FAIL  %s is solid outside its own bounds\n", c.name);
                }
            }
        }
        check(escaped == 0, "no shape is solid outside the box it reports");

        check(!Sdf::half_space().bounded(), "a half space knows it is unbounded");
        check(Sdf::half_space().intersected(Sdf::box(Vec3::one())).bounded(),
              "and intersecting one with a box makes it finite again");
        check(!Sdf::sphere(1).repeated(Vec3(2, 0, 0)).bounded(),
              "an unlimited repeat is unbounded");
        check(Sdf::sphere(1).repeated(Vec3(2, 0, 0), Vec3(4, 0, 0)).bounded(),
              "and a counted one is not");
    }

    // ------------------------------------------------- the meshing
    {
        std::string error;
        Sdf::MeshOptions options;
        options.target_cells = 64;

        Ref<Mesh> sphere = Sdf::sphere(1.0f).to_mesh(options, &error);
        check(sphere && error.empty(), "a sphere meshes");
        if (sphere) {
            check(open_edges(*sphere) == 0, "and the mesh is closed");
            // The analytic volume, which no amount of plausible
            // geometry will match by accident.
            const double want = 4.0 / 3.0 * 3.14159265358979 * 1.0;
            check_near(float(mesh_volume(*sphere)), float(want), 0.06f,
                       "and encloses the volume a unit sphere should");
            const AABB b = sphere->bounds();
            check_near(b.max.x - b.min.x, 2.0f, 0.06f, "and is two metres across");
        }

        // The measurement that says the boolean really happened:
        // a box with a sphere cut out of it has the volume of the
        // box minus the volume of the sphere.
        Ref<Mesh> cut = Sdf::box(Vec3(2, 2, 2))
                            .subtracted(Sdf::sphere(1.0f))
                            .to_mesh(options, &error);
        check(cut && error.empty(), "a difference meshes");
        if (cut) {
            check(open_edges(*cut) == 0, "and is closed too");
            const double want = 8.0 - 4.0 / 3.0 * 3.14159265358979;
            check_near(float(mesh_volume(*cut)), float(want), 0.25f,
                       "and has lost exactly the sphere's worth of material");
        }

        // A hollow shell is two surfaces, and both have to be there.
        Ref<Mesh> shell = Sdf::sphere(1.0f).shelled(0.2f).to_mesh(options, &error);
        check(shell && open_edges(*shell) == 0, "a shell is closed");
        if (shell) {
            const double outer = 4.0 / 3.0 * 3.14159265358979 * std::pow(1.1, 3);
            const double inner = 4.0 / 3.0 * 3.14159265358979 * std::pow(0.9, 3);
            check_near(float(mesh_volume(*shell)), float(outer - inner), 0.12f,
                       "and encloses only the wall");
        }

        // Cell size is respected, and a request for an absurd one is
        // coarsened rather than allowed to allocate the world.
        Sdf::MeshOptions fine;
        fine.cell_size = 0.02f;
        fine.max_cells_per_axis = 32;
        Ref<Mesh> clamped = Sdf::sphere(1.0f).to_mesh(fine, &error);
        check(clamped && clamped->vertex_count() < 20000,
              "an impossible cell size is coarsened, not obeyed");

        // The two ways to be un-meshable, both answered rather than
        // crashed.
        check(!Sdf().to_mesh(options, &error) && !error.empty(),
              "an empty shape says so");
        check(!Sdf::half_space().to_mesh(options, &error) &&
                  error.find("unbounded") != std::string::npos,
              "and an unbounded one says which problem it has");
    }

    // ---------------------------------------- the written form
    //
    // An agent sends a shape as JSON, so the JSON has to mean the
    // same thing as the calls -- checked by sampling both.
    {
        struct Case { const char *name; const char *json; Sdf built; };
        std::vector<Case> cases = {
            {"sphere", R"({"shape":"sphere","radius":0.8})", Sdf::sphere(0.8f)},
            {"rounded box", R"({"shape":"box","size":[1,2,3],"round":0.1})",
             Sdf::box(Vec3(1, 2, 3), 0.1f)},
            {"placed", R"({"shape":"sphere","radius":0.5,"at":[1,2,3]})",
             Sdf::sphere(0.5f).translated(Vec3(1, 2, 3))},
            {"rotated", R"({"shape":"box","size":[2,0.5,0.5],"rotate":[0,90,0]})",
             Sdf::box(Vec3(2, 0.5f, 0.5f)).rotated(Vec3::up(), deg2rad(90.0f))},
            {"difference", R"({"op":"difference","of":[{"shape":"box","size":[1,1,1]},
                               {"shape":"sphere","radius":0.6}]})",
             Sdf::box(Vec3::one()).subtracted(Sdf::sphere(0.6f))},
            {"blend", R"({"op":"union","blend":0.3,"of":[{"shape":"sphere","radius":0.6},
                          {"shape":"sphere","radius":0.6,"at":[0.8,0,0]}]})",
             Sdf::sphere(0.6f).blended(Sdf::sphere(0.6f).translated(Vec3(0.8f, 0, 0)), 0.3f)},
            {"twist", R"({"shape":"box","size":[0.5,2,0.5],"twist":0.4})",
             Sdf::box(Vec3(0.5f, 2, 0.5f)).twisted(0.4f)},
            {"shell", R"({"shape":"sphere","radius":1,"shell":0.2})",
             Sdf::sphere(1).shelled(0.2f)},
        };
        size_t disagreed = 0;
        for (Case &c : cases) {
            std::string error;
            const Sdf parsed = Sdf::from_json(Json::parse(c.json, &error), &error);
            if (!error.empty()) {
                std::printf("  FAIL  %s did not parse: %s\n", c.name, error.c_str());
                g_fail++;
                g_checks++;
                continue;
            }
            for (int i = 0; i < 2000; i++) {
                const Vec3 p = random_point(4.0f);
                if (std::fabs(parsed.distance(p) - c.built.distance(p)) > 1e-4f) {
                    if (disagreed++ == 0)
                        std::printf("  FAIL  %s: JSON and calls build different shapes\n",
                                    c.name);
                    break;
                }
            }
        }
        check(disagreed == 0, "every shape means the same written down as called");

        // And back again, which is what an editor saving one needs.
        size_t round_trip_failures = 0;
        for (Case &c : cases) {
            std::string error;
            const Json written = c.built.to_json();
            const Sdf back = Sdf::from_json(written, &error);
            if (!error.empty()) { round_trip_failures++; continue; }
            for (int i = 0; i < 2000; i++) {
                const Vec3 p = random_point(4.0f);
                if (std::fabs(back.distance(p) - c.built.distance(p)) > 1e-3f) {
                    std::printf("  FAIL  %s does not survive to_json: %s\n", c.name,
                                written.to_string().c_str());
                    round_trip_failures++;
                    break;
                }
            }
        }
        check(round_trip_failures == 0, "and survives being written out and read back");

        // A union of five, which the fold has to handle.
        std::string error;
        const Sdf five = Sdf::from_json(
            Json::parse(R"({"op":"union","of":[{"shape":"sphere","radius":0.3,"at":[0,0,0]},
                            {"shape":"sphere","radius":0.3,"at":[1,0,0]},
                            {"shape":"sphere","radius":0.3,"at":[2,0,0]},
                            {"shape":"sphere","radius":0.3,"at":[3,0,0]},
                            {"shape":"sphere","radius":0.3,"at":[4,0,0]}]})",
                       &error),
            &error);
        check(error.empty() && five.distance(Vec3(4, 0, 0)) < 0,
              "an operation takes any number of operands");
        // Five spheres, each with a placement, and four unions.
        check(five.node_count() == 14, "and folds them into one tree");
    }

    // ------------------------------------------- what a wrong shape gets
    {
        std::string error;
        Sdf::from_json(Json::parse(R"({"shape":"cilinder"})", &error), &error);
        check(error.find("cylinder") != std::string::npos,
              "a misspelled primitive is suggested");
        Sdf::from_json(Json::parse(R"({"op":"substract","of":[{"shape":"box"}]})", &error),
                       &error);
        check(error.find("difference") != std::string::npos ||
                  error.find("intersection") != std::string::npos,
              "and a misspelled operation is too");
        Sdf::from_json(Json::parse(R"({"radius":1})", &error), &error);
        check(error.find("shape") != std::string::npos,
              "an object with no shape says what it needs");
        Sdf::from_json(Json::parse(R"({"op":"union","of":[{"shape":"box"},{"shpe":"x"}]})",
                                   &error),
                       &error);
        check(error.find("child 1") != std::string::npos,
              "and a bad child says which child it was");
    }

    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
