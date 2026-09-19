// Warren -- does the liquid behave like a liquid.
//
// A particle solver is easy to write and hard to trust. It will
// produce something that moves and sparkles long before it produces
// something that holds its volume, and the difference is not
// visible in a screenshot. So every question here is a number:
//
//   does a block of it fall, land and STOP, without exploding and
//     without sinking through the floor;
//   does it hold its density -- the one measurement that separates
//     a liquid from a cloud of sand;
//   does it stay inside a container;
//   and does it cross a portal WITHOUT TEARING.
//
// The last is the one this engine exists to answer. Warping a
// particle at the plane is easy and on its own it looks wrong: a
// particle just short of the hole has neighbours on its own side
// only, reads as under-dense, and is pushed away from the opening.
// The stream splits and sprays. The fix is ghost particles -- the
// liquid on the far side, warped into place so the neighbourhood
// is continuous -- and the test measures exactly that, by running
// the same pour with the ghosts suppressed and comparing.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "physics/fluid.h"
#include "physics/shapes.h"
#include "physics/world.h"
#include "render/mesh.h"
#include "scene/portal.h"

using namespace wr;

namespace {

int g_fail = 0;

void check(bool ok, const char *what) {
    std::printf("    %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

void check_lt(float got, float limit, const char *what) {
    const bool ok = got < limit;
    std::printf("    %s %s (%.4f < %.3f)\n", ok ? "ok  " : "FAIL", what, got,
                limit);
    if (!ok) g_fail++;
}

void check_gt(float got, float limit, const char *what) {
    const bool ok = got > limit;
    std::printf("    %s %s (%.4f > %.3f)\n", ok ? "ok  " : "FAIL", what, got,
                limit);
    if (!ok) g_fail++;
}

// An open-topped box out of six slabs, which is what a fluid test
// needs and what a level is made of.
void tank(PhysicsWorld &w, std::vector<Ref<Mesh>> &keep, const AABB &inner,
          float wall = 0.2f, bool lid = false) {
    const Vec3 c = (inner.min + inner.max) * 0.5f;
    const Vec3 s = inner.max - inner.min;
    struct Slab {
        Vec3 centre, size;
    };
    std::vector<Slab> slabs = {
        {Vec3(c.x, inner.min.y - wall * 0.5f, c.z),
         Vec3(s.x + wall * 2, wall, s.z + wall * 2)},
        {Vec3(inner.min.x - wall * 0.5f, c.y, c.z), Vec3(wall, s.y, s.z)},
        {Vec3(inner.max.x + wall * 0.5f, c.y, c.z), Vec3(wall, s.y, s.z)},
        {Vec3(c.x, c.y, inner.min.z - wall * 0.5f), Vec3(s.x, s.y, wall)},
        {Vec3(c.x, c.y, inner.max.z + wall * 0.5f), Vec3(s.x, s.y, wall)},
    };
    if (lid)
        slabs.push_back({Vec3(c.x, inner.max.y + wall * 0.5f, c.z),
                         Vec3(s.x + wall * 2, wall, s.z + wall * 2)});
    for (const Slab &sl : slabs) {
        Ref<Mesh> m = Mesh::box(sl.size);
        keep.push_back(m);
        w.add_mesh(*m, Transform3D(sl.centre), 1);
    }
}

void run(Fluid &f, float seconds, float dt = 1.0f / 60.0f) {
    const int n = int(seconds / dt);
    for (int i = 0; i < n; i++) f.step(dt);
}

float lowest(const Fluid &f) {
    float lo = 1e30f;
    for (const Vec3 &p : f.positions()) lo = std::min(lo, p.y);
    return lo;
}
float highest(const Fluid &f) {
    float hi = -1e30f;
    for (const Vec3 &p : f.positions()) hi = std::max(hi, p.y);
    return hi;
}

}  // namespace

int main() {
    log_set_level(LogLevel::Warn);
    std::printf("fluid\n");

    // ------------------------------------------- it falls and it stops
    {
        PhysicsWorld w;
        std::vector<Ref<Mesh>> keep;
        tank(w, keep, AABB(Vec3(-0.5f, 0, -0.5f), Vec3(0.5f, 2.0f, 0.5f)));
        Fluid f(&w);
        f.particle_radius = 0.035f;
        f.material.smoothing_radius = 0.105f;
        const int n = f.fill(AABB(Vec3(-0.4f, 0.6f, -0.4f), Vec3(0.4f, 1.2f, 0.4f)));
        check(n > 300, "a block of liquid fills with a useful number of particles");
        run(f, 3.0f);
        check(f.count() == size_t(n), "none of it leaked out of the tank");
        check_gt(lowest(f), -0.05f, "and none of it fell through the floor");
        // Settled: it should be a puddle in the bottom, not a
        // column still standing where it started.
        check_lt(highest(f), 1.0f, "it has settled into the bottom");
        // THE AVERAGE, NOT THE MAXIMUM. A tank of a thousand
        // particles always has one at the surface being flicked
        // about by its neighbours, and a test on the maximum is a
        // test on that one particle. What "settled" means is that
        // the body of the liquid is not moving.
        float total = 0.0f, worst = 0.0f;
        for (const Vec3 &v : f.velocities()) {
            total += v.length();
            worst = std::max(worst, v.length());
        }
        const float mean = total / float(std::max<size_t>(1, f.count()));
        std::printf("      settled: mean speed %.3f, fastest %.3f\n", mean,
                    worst);
        // A POSITION-BASED FLUID AT REST SIMMERS, and the number
        // it simmers at is about one step of gravity -- 0.16 m/s
        // at 60 Hz. That is not a defect to be tuned out: the
        // constraint is resolved in POSITION every step, so each
        // particle falls a little and is caught a little, and the
        // velocity that comes back out of (x* - x)/dt never
        // reaches zero. It is three millimetres of motion a frame
        // and invisible behind a surface.
        //
        // Checked against vorticity and viscosity both off: the
        // number does not move, so it is the method and not a
        // term that could be turned down.
        check_lt(mean, 0.35f, "and the body of it has stopped moving");
        check_lt(worst, 3.2f, "with nothing flying about");
    }

    // ------------------------------------------------ it holds its volume
    {
        PhysicsWorld w;
        std::vector<Ref<Mesh>> keep;
        tank(w, keep, AABB(Vec3(-0.5f, 0, -0.5f), Vec3(0.5f, 2.0f, 0.5f)));
        Fluid f(&w);
        f.particle_radius = 0.035f;
        f.material.smoothing_radius = 0.105f;
        if (const char *e = getenv("WR_IT")) f.solver_iterations = atoi(e);
        f.fill(AABB(Vec3(-0.45f, 0.05f, -0.45f), Vec3(0.45f, 0.8f, 0.45f)));
        if (!getenv("WR_IT")) f.solver_iterations = 16;
        run(f, 4.0f);
        // THE MEASUREMENT THAT SEPARATES A LIQUID FROM SAND, and
        // it is the density and not the height.
        //
        // Height was the first thing tried and it is the wrong
        // question: how tall a settled column is depends on how
        // the lattice was seeded, and a block laid out on a cubic
        // grid is not at the packing the kernel calls rest. It
        // will settle a little however good the solver is, and
        // measuring that says nothing.
        std::printf("      interior density %.3f of rest over %u particles, "
                    "worst compression %.3f\n",
                    f.stats.interior_density, f.stats.interior_count,
                    f.stats.worst_compression);
        check_gt(f.stats.interior_count, 50.0f,
                 "the settled liquid has an interior to measure");
        // At sixteen iterations, on a column about twenty
        // particles deep. See Fluid::solver_iterations -- this
        // number is a statement about how many iterations were
        // paid for, not about whether the solver is right.
        check_lt(std::fabs(f.stats.interior_density - 1.0f), 0.06f,
                 "and it is at the rest density -- it is not being crushed");
        // THE WORST SINGLE PARTICLE IS NOT A MEASUREMENT.
        //
        // A fluid is chaotic, and the maximum over a chaotic
        // field is the least stable statistic there is: the same
        // scene, run with numerically equivalent code -- a
        // divide replaced by a multiply against its reciprocal
        // -- gave 0.11, 0.15, 0.28 and 0.40 on four builds that
        // are all correct. Asserting on it means the test fails
        // when somebody makes the solver faster.
        //
        // The interior density above IS stable: 1.027 to 1.028
        // across every one of those builds. That is the
        // incompressibility measurement; this one is reported
        // because a big jump is still worth a look, with a
        // bound only wide enough to catch a real collapse.
        check_lt(f.stats.worst_compression, 1.0f,
                 "and nothing has collapsed");
    }

    // -------------------------------------------------- through a portal
    //
    // Two apertures: one in the floor of a raised box, one in the
    // ceiling of a lower one, so the liquid pours in at the top and
    // falls out below. The measurement is what happens AT the
    // mouth.
    float tear[2] = {0.0f, 0.0f};
    for (int ghosts = 0; ghosts < 2; ghosts++) {
        PhysicsWorld w;
        std::vector<Ref<Mesh>> keep;
        // A floor with a hole is not needed: the aperture makes the
        // slab non-solid where it is.
        Ref<Mesh> floor = Mesh::box(Vec3(4, 0.2f, 4));
        keep.push_back(floor);
        w.add_mesh(*floor, Transform3D(Vec3(0, -0.1f, 0)), 1);
        Ref<Mesh> low = Mesh::box(Vec3(4, 0.2f, 4));
        keep.push_back(low);
        w.add_mesh(*low, Transform3D(Vec3(0, -5.1f, 0)), 1);

        // Facing up out of the floor, and up out of the lower floor.
        Portal3D in, out;
        in.width = in.height = 0.9f;
        in.active = true;
        in.open = 1.0f;
        in.set_position(Vec3(0, 0.001f, 0));
        // +Z is a portal's normal; turn it to face straight up.
        in.set_euler(Vec3(0.0f, -1.5707963f, 0.0f));
        out.width = out.height = 0.9f;
        out.active = true;
        out.open = 1.0f;
        out.set_position(Vec3(0, -4.999f, 0));
        out.set_euler(Vec3(0.0f, -1.5707963f, 0.0f));
        in.link_to(&out);
        w.add_portal(&in);
        w.add_portal(&out);

        Fluid f(&w);
        f.particle_radius = 0.035f;
        f.material.smoothing_radius = 0.105f;
        f.kill_below_y = -20.0f;
        // Suppressing the ghosts is the control. Pushing the
        // smoothing radius below the distance at which a ghost is
        // collected would change the physics too; instead the
        // portals are simply not registered for the control run,
        // and the crossing is done by hand.
        // THE SAME SCENE BOTH WAYS. The portals stay registered
        // in both runs, so the liquid crosses in both and the only
        // difference is whether a particle at the mouth can see
        // the liquid on the other side of it.
        f.portal_ghosts = ghosts != 0;
        f.fill(AABB(Vec3(-0.3f, 0.30f, -0.3f), Vec3(0.3f, 0.95f, 0.3f)));
        const size_t started = f.count();

        // HOW BADLY IT TEARS, measured where the tearing is: the
        // spread of the stream in the plane of the aperture, a
        // moment after the front of it has gone through. A
        // neighbourhood that stops at the hole pushes particles
        // sideways out of the opening, so the stream fans; one
        // that sees through does not.
        float worst = 0.0f, spread = 0.0f;
        const int n = int(1.2f / (1.0f / 60.0f));
        for (int i = 0; i < n; i++) {
            f.step(1.0f / 60.0f);
            worst = std::max(worst, f.stats.worst_compression);
            // Particles within a hand's width of the entry plane.
            float far = 0.0f;
            int seen = 0;
            for (const Vec3 &p : f.positions()) {
                if (std::fabs(p.y) > 0.08f) continue;
                far += std::sqrt(p.x * p.x + p.z * p.z);
                seen++;
            }
            if (seen > 20) spread = std::max(spread, far / float(seen));
        }
        tear[ghosts] = spread;
        if (ghosts) {
            std::printf("      with ghosts:    %u crossed, %u ghosts, "
                        "worst compression %.3f, spread at the mouth %.3f\n",
                        f.stats.portal_crossings, f.stats.ghosts, worst,
                        tear[1]);
            check(f.stats.portal_crossings > 0, "liquid poured through the portal");
            check(f.stats.ghosts > 0,
                  "and the far side was visible to it as ghost particles");
            check(f.count() > started / 2,
                  "and most of it survived the crossing");
            // It should be BELOW, having come out of the lower
            // aperture, not piled on the upper floor.
            check_lt(lowest(f), -1.0f, "and it came out the other end");
            // THE CLAIM, MEASURED. Same scene, same crossing, the
            // only difference being whether the neighbourhood
            // reaches through the hole.
            check(tear[1] < tear[0] * 0.9f,
                  "and the stream is measurably narrower at the mouth "
                  "than without them -- it does not tear");
        } else {
            std::printf("      without ghosts: worst compression %.3f, "
                        "spread at the mouth %.3f\n",
                        worst, tear[0]);
        }
    }

    // ------------------------------------------- what the surface costs
    //
    // The liquid is drawn as an isosurface and contouring it is by
    // far the most expensive thing here, so the number belongs in
    // the test rather than in somebody's frame budget by surprise.
    //
    // And it is measured on a SPREAD-OUT body of liquid, because
    // that is the case that decides whether it is affordable. A
    // bucketful is cheap however it is meshed; the same liquid
    // thrown down a corridor has a bounding box twenty times the
    // volume and almost all of it empty, and meshing the box costs
    // the cube of the side for a picture of nothing.
    {
        PhysicsWorld w;
        std::vector<Ref<Mesh>> keep;
        Ref<Mesh> floor = Mesh::box(Vec3(12, 0.4f, 3));
        keep.push_back(floor);
        w.add_mesh(*floor, Transform3D(Vec3(0, -0.2f, 0)), 1);
        Fluid f(&w);
        f.particle_radius = 0.045f;
        f.material.smoothing_radius = 0.135f;
        // A long thin stream along the corridor.
        for (int i = 0; i < 1400; i++) {
            const float t = float(i) / 1400.0f;
            f.emit(Vec3(-5.0f + t * 10.0f, 0.10f + 0.05f * float(i % 3),
                        0.05f * float((i / 3) % 3)));
        }
        run(f, 0.5f);

        const AABB b = f.bounds();
        const Vec3 sz = b.max - b.min;
        const float cell = 0.09f;
        const int chunk = 8;
        const float span = cell * float(chunk);
        const int nx = std::max(1, int(std::ceil(sz.x / span)));
        const int ny = std::max(1, int(std::ceil(sz.y / span)));
        const int nz = std::max(1, int(std::ceil(sz.z / span)));
        int occupied = 0;
        for (int x = 0; x < nx; x++)
            for (int y = 0; y < ny; y++)
                for (int z = 0; z < nz; z++) {
                    const Vec3 o =
                        b.min + Vec3(float(x), float(y), float(z)) * span;
                    if (f.occupied(AABB(o, o + Vec3(span, span, span))))
                        occupied++;
                }
        const int all = nx * ny * nz;
        std::printf("      spread liquid: %zu particles over %.1f x %.1f x "
                    "%.1f m, %d of %d chunks have any in them\n",
                    f.count(), sz.x, sz.y, sz.z, occupied, all);
        check(occupied < all,
              "a spread-out body of liquid does not fill its own bounding box");

        // Time the sampling, which is the whole cost, both ways.
        auto sample_chunks = [&](bool skip) {
            const auto t0 = std::chrono::steady_clock::now();
            for (int x = 0; x < nx; x++)
                for (int y = 0; y < ny; y++)
                    for (int z = 0; z < nz; z++) {
                        const Vec3 o =
                            b.min + Vec3(float(x), float(y), float(z)) * span;
                        if (skip &&
                            !f.occupied(AABB(o, o + Vec3(span, span, span))))
                            continue;
                        for (int i = 0; i <= chunk; i++)
                            for (int j = 0; j <= chunk; j++)
                                for (int k = 0; k <= chunk; k++)
                                    (void)f.density_at(
                                        o + Vec3(float(i), float(j),
                                                 float(k)) * cell);
                    }
            const auto t1 = std::chrono::steady_clock::now();
            return std::chrono::duration<double, std::milli>(t1 - t0).count();
        };
        // THE BEST OF THREE, INTERLEAVED. A single timing is a
        // sample of whatever else the machine was doing, and this
        // test failed under `ctest -j4` on a commit it passed
        // alone -- not because the ratio moved but because one of
        // the two runs was descheduled. Taking the minimum of a
        // few is the standard answer: the fastest run is the one
        // with the least interference in it, and interleaving
        // them means neither side gets a quiet patch the other
        // did not.
        double dense = 1e30, sparse = 1e30;
        for (int i = 0; i < 3; i++) {
            dense = std::min(dense, sample_chunks(false));
            sparse = std::min(sparse, sample_chunks(true));
        }
        std::printf("      whole box %.1f ms, occupied chunks only %.1f ms\n",
                    dense, sparse);
        // A THIRD OFF, ON THE WORST CASE FOR IT. This stream has
        // splashed over half a second, so it genuinely occupies
        // two thirds of the chunks its bounding box contains --
        // the skip cannot do better than that and should not
        // claim to. A puddle in the corner of a room, which is
        // the common case, saves far more.
        check(sparse < dense * 0.8,
              "and skipping the empty chunks is measurably cheaper");
        // THE RATIO IS ASSERTED, THE TIME IS ONLY REPORTED.
        //
        // A wall-clock threshold in a test is a test that fails on
        // a loaded machine: this one passed alone and failed under
        // `ctest -j4` on the same commit, which tells you about
        // the other three tests and nothing about the fluid. The
        // ratio is what the change was for and it is load
        // independent. The bound below is left only wide enough to
        // catch something going quadratic.
        //
        // Single-threaded here; Fluid3D puts the chunks out to the
        // job system, so a game sees this divided by its cores.
        check_lt(float(sparse), 400.0f,
                 "and a rebuild has not gone quadratic");
    }

    // ------------------------------------- what a step actually costs
    //
    // The number that decides how much liquid a game can have,
    // so it belongs in the test rather than in somebody's frame
    // budget by surprise. Reported, not asserted against a wall
    // clock -- see the surfacing note above -- except for a very
    // wide bound that would catch something going quadratic.
    {
        PhysicsWorld w;
        std::vector<Ref<Mesh>> keep;
        tank(w, keep, AABB(Vec3(-1.4f, 0, -1.4f), Vec3(1.4f, 3.0f, 1.4f)));
        Fluid f(&w);
        f.particle_radius = 0.045f;
        f.material.smoothing_radius = 0.135f;
        f.fill(AABB(Vec3(-1.3f, 0.05f, -1.3f), Vec3(1.3f, 1.6f, 1.3f)));
        run(f, 0.4f);           // let it find its packing
        const size_t n = f.count();
        double best = 1e30;
        for (int rep = 0; rep < 3; rep++) {
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 20; i++) f.step(1.0f / 60.0f);
            const auto t1 = std::chrono::steady_clock::now();
            best = std::min(
                best,
                std::chrono::duration<double, std::milli>(t1 - t0).count() / 20.0);
        }
        std::printf("      %zu particles, %.2f ms a step, %u neighbours\n",
                    n, best, f.stats.neighbours);
        std::printf("      grid %.1f  neighbours %.1f  density %.1f  "
                    "collide %.1f  finish %.1f ms\n",
                    f.stats.ms_grid, f.stats.ms_neighbours,
                    f.stats.ms_density, f.stats.ms_collide,
                    f.stats.ms_finish);
        std::printf("      busiest neighbourhood %u, %u truncated\n",
                    f.stats.most_neighbours, f.stats.truncated);
        // A TRUNCATED NEIGHBOURHOOD UNDER-REPORTS ITS DENSITY,
        // so the solver pushes less and the fluid compresses
        // further -- silently, and worse the more it happens.
        check(f.stats.truncated == 0,
              "and no neighbourhood was bigger than the cap allows");
        check(n > 6000, "a serious body of liquid");
        check_lt(float(best), 120.0f, "and a step has not gone quadratic");
    }

    std::printf("  %s\n", g_fail ? "FAILED" : "all good");
    return g_fail ? 1 : 0;
}
