// Warren -- the navigation bake, checked against worlds small enough
// to know the answer for.
//
// Every stage of a navmesh bake produces something that looks
// plausible when it is wrong. A heightfield with the slope test
// inverted still has spans. A ledge filter that is too eager still
// gives a navmesh, just one that stops a metre short of every wall.
// Regions that interlock still have ids. Nothing throws, nothing
// looks obviously broken, and bodies walk off roofs.
//
// So each world here is one shape with one arithmetic answer: a
// floor whose walkable area can be multiplied out, a wall that must
// cut it in exactly two, a table nothing can reach, a staircase that
// must stay in one piece.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "nav/heightfield.h"
#include "nav/navmesh.h"

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
void check_int(int got, int want, const char *what) {
    g_checks++;
    if (got != want) {
        std::printf("  FAIL  %s: got %d, wanted %d\n", what, got, want);
        g_fail++;
    }
}

// An axis-aligned box as twelve triangles, so that the test worlds
// read as rooms rather than as vertex lists.
void box(std::vector<Vec3> &t, const Vec3 &mn, const Vec3 &mx) {
    const Vec3 c[8] = {
        {mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z},
        {mn.x, mn.y, mx.z}, {mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z},
        {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
    // Wound so every normal points OUT, which is not decoration:
    // the baker takes a surface's normal as the direction a body
    // would stand on it, so an inside-out box is a box with no
    // floor and a walkable ceiling.
    const int f[12][3] = {{0, 1, 2}, {0, 2, 3},   // -y
                          {4, 7, 6}, {4, 6, 5},   // +y
                          {0, 4, 5}, {0, 5, 1},   // -z
                          {1, 5, 6}, {1, 6, 2},   // +x
                          {2, 6, 7}, {2, 7, 3},   // +z
                          {3, 7, 4}, {3, 4, 0}};  // -x
    for (auto &tri : f) {
        t.push_back(c[tri[0]]);
        t.push_back(c[tri[1]]);
        t.push_back(c[tri[2]]);
    }
}

// A ramp rising along +x, as two triangles.
void ramp(std::vector<Vec3> &t, float x0, float x1, float z0, float z1,
          float y0, float y1) {
    Vec3 a(x0, y0, z0), b(x1, y1, z0), c(x1, y1, z1), d(x0, y0, z1);
    t.push_back(a); t.push_back(c); t.push_back(b);
    t.push_back(a); t.push_back(d); t.push_back(c);
}

AgentSpec survivor() {
    AgentSpec a;
    a.radius = 0.4f;
    a.height = 1.8f;
    a.max_climb = 0.45f;
    a.max_slope_degrees = 48.0f;
    return a;
}

// How much walkable ground survived, in square metres.
float walkable_area(const CompactField &f) {
    return float(f.spans().size()) * f.cell_size() * f.cell_size();
}

// The same, counting only ground that made it into a region -- which
// is the ground that will become polygons. Cells dropped as an
// unreachable island are still walkable surfaces; they are just not
// part of the navmesh.
float meshed_area(const CompactField &f) {
    size_t n = 0;
    for (const CompactSpan &s : f.spans())
        if (s.region != kNoRegion) ++n;
    return float(n) * f.cell_size() * f.cell_size();
}

// A VOXEL BAKE IS ACCURATE TO A CELL, and no better. Which side of a
// boundary a surface exactly on that boundary lands is a tie-break,
// so a square of known size comes out within one cell of it. Asking
// for more than that from any voxel method is asking it to lie, so
// the square worlds below are checked on their side length with a
// cell of slack rather than on an area to four figures.
void check_square(float area, float want_side, float cells, float cs,
                  const char *what) {
    float side = std::sqrt(std::fmax(area, 0.0f));
    g_checks++;
    if (!(std::fabs(side - want_side) <= cells * cs)) {
        std::printf("  FAIL  %s: side %.3f m, wanted %.3f +- %.3f\n", what,
                    double(side), double(want_side), double(cells * cs));
        g_fail++;
    }
}

int distinct_regions(const CompactField &f) {
    std::vector<bool> seen(size_t(f.region_count()) + 1, false);
    int n = 0;
    for (const CompactSpan &s : f.spans()) {
        if (s.region == kNoRegion) continue;
        if (!seen[s.region]) {
            seen[s.region] = true;
            ++n;
        }
    }
    return n;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const float cs = 0.25f, ch = 0.15f;

    // ---------------------------------------------------- a bare floor
    //
    // Ten metres square, one slab. Two numbers are known exactly: the
    // grid is 40x40 cells, and after eroding by the body's radius the
    // walkable part is the floor inset by that radius on all four
    // sides -- 9.2 by 9.2, because the navmesh must not let a body
    // stand with half of itself over the edge.
    {
        std::vector<Vec3> tris;
        box(tris, Vec3(0, -0.2f, 0), Vec3(10, 0, 10));
        AABB b(Vec3(-1, -1, -1), Vec3(11, 3, 11));

        Heightfield hf;
        check(hf.build(tris, b, cs, ch, 48.0f), "the floor rasterises");
        check_int(hf.width(), 48, "grid width from the bake volume");
        check(hf.span_count() >= 1600, "at least one span per floor cell");
        check_int(int(hf.column(20, 20).size()), 1,
                  "one slab is one span, not two");

        CompactField cf;
        check(cf.build(hf, survivor()), "the compact field builds");
        // THE INSET IS NOT THE RADIUS, and this is the number to
        // know when the navmesh looks a little small. A floating
        // slab has a drop on all four sides, so the outermost ring
        // of cells is a ledge and goes first -- one cell. Then
        // erosion takes the body's radius, rounded up to whole
        // cells, because a cell is either in or out. At 0.25 m cells
        // and a 0.4 m radius that is one plus two, so the navmesh
        // sits 0.75 m inside the floor rather than 0.4 m.
        //
        // It errs toward not walking off roofs, which is the right
        // way to err, and it is why the cell size matters: halve it
        // and the overshoot halves with it.
        check_square(walkable_area(cf), 9.5f, 1.0f, cs,
                     "before eroding, all but the ledge ring is walkable");

        cf.erode(survivor().radius);
        check_square(walkable_area(cf), 8.5f, 1.0f, cs,
                     "eroding takes the radius, rounded up to whole cells");

        check_int(cf.build_regions(1.5f), 2, "open ground is one region");
        check_int(distinct_regions(cf), 1, "and every cell is in it");
    }

    // ------------------------------------------------- a wall, and a door
    //
    // The same floor with a wall across the middle. With no way
    // through, the two halves must be separate regions and must not
    // be linked -- if they are, a body will path straight through the
    // wall. With a door, they must be one connected surface again.
    {
        auto bake = [&](bool door, CompactField &cf) {
            std::vector<Vec3> tris;
            box(tris, Vec3(0, -0.2f, 0), Vec3(10, 0, 10));
            if (door) {
                box(tris, Vec3(4.9f, 0, 0), Vec3(5.1f, 2.5f, 4.0f));
                box(tris, Vec3(4.9f, 0, 6.0f), Vec3(5.1f, 2.5f, 10.0f));
            } else {
                box(tris, Vec3(4.9f, 0, 0), Vec3(5.1f, 2.5f, 10.0f));
            }
            AABB b(Vec3(-1, -1, -1), Vec3(11, 4, 11));
            Heightfield hf;
            hf.build(tris, b, cs, ch, 48.0f);
            cf.build(hf, survivor());
            cf.erode(survivor().radius);
            cf.build_regions(1.5f);
        };

        // Are these two cells in one connected component of the link
        // graph? This is the question that matters: regions are a
        // partition for the polygon builder's benefit, but
        // reachability is what a path depends on.
        auto connected = [](const CompactField &f, int ax, int az, int bx,
                            int bz) {
            const auto &ca = f.cell(ax, az);
            const auto &cb = f.cell(bx, bz);
            if (ca.count == 0 || cb.count == 0) return false;
            std::vector<bool> seen(f.spans().size(), false);
            std::vector<int32_t> stack{int32_t(ca.start)};
            seen[ca.start] = true;
            while (!stack.empty()) {
                int32_t i = stack.back();
                stack.pop_back();
                if (i == int32_t(cb.start)) return true;
                for (int d = 0; d < 4; ++d) {
                    int32_t a = f.spans()[size_t(i)].neighbour[d];
                    if (a == kNoNeighbour || seen[size_t(a)]) continue;
                    seen[size_t(a)] = true;
                    stack.push_back(a);
                }
            }
            return false;
        };

        CompactField shut, open;
        bake(false, shut);
        bake(true, open);

        check(!connected(shut, 8, 20, 32, 20),
              "a solid wall separates the two halves");
        check(connected(open, 8, 20, 32, 20),
              "a doorway joins them again");
        check(distinct_regions(shut) >= 2,
              "and the shut room is at least two regions");
    }

    // ------------------------------------------------- a table, unreached
    //
    // A table top is a perfectly good walkable surface that nothing
    // can get onto. It must not appear in the navmesh, or a body can
    // be ordered to stand on it and will spend forever failing.
    {
        AABB b(Vec3(-1, -1, -1), Vec3(11, 4, 11));
        auto bake = [&](bool table, CompactField &cf) {
            std::vector<Vec3> tris;
            box(tris, Vec3(0, -0.2f, 0), Vec3(10, 0, 10));
            if (table) box(tris, Vec3(4, 0, 4), Vec3(6, 0.8f, 6));
            Heightfield hf;
            hf.build(tris, b, cs, ch, 48.0f);
            if (table)
                check_int(int(hf.column(20, 20).size()), 1,
                          "the table merges with the floor it stands on");
            cf.build(hf, survivor());
            cf.erode(survivor().radius);
            cf.build_regions(1.5f);
        };
        CompactField bare, cf;
        bake(false, bare);
        bake(true, cf);

        // Nothing at table height should have survived.
        // The table top survives as a surface -- it is flat, it has
        // headroom, and nothing about it is wrong except that no
        // body can get to it. What removes it is the region filter,
        // which drops any piece too small to be worth reaching that
        // touches no other piece. So the check is that no region
        // lives up there, not that no span does.
        float on_table = 0.0f, meshed_on_table = 0.0f;
        for (const CompactSpan &s : cf.spans()) {
            if (b.min.y + float(s.y) * ch <= 0.5f) continue;
            on_table += cs * cs;
            if (s.region != kNoRegion) meshed_on_table += cs * cs;
        }
        check(on_table > 0.0f, "the table top is a walkable surface");
        check_near(meshed_on_table, 0.0f, 0.001f,
                   "but it is an island, so no region is put on it");

        // The floor, with a table-shaped hole. Measured against the
        // same floor without the table, so the comparison is against
        // what this bake actually produces rather than against the
        // ideal square -- the hole is the whole of the difference.
        // It is the 2 m table grown by the ledge ring and the
        // erosion on each side.
        float hole = meshed_area(bare) - meshed_area(cf);
        check_square(hole, 3.5f, 2.0f, cs,
                     "and the floor has a table-shaped hole in it");
    }

    // ------------------------------------------------------- a staircase
    //
    // Steps of 0.2 m, which is under the 0.45 m the body can climb,
    // so the whole flight plus both landings must be one connected
    // surface. A ledge filter that is too eager eats staircases
    // first, and this is the check that catches it.
    {
        std::vector<Vec3> tris;
        box(tris, Vec3(0, -0.2f, 0), Vec3(4, 0, 6));
        for (int i = 0; i < 10; ++i) {
            float y = float(i + 1) * 0.2f;
            float x = 4.0f + float(i) * 0.3f;
            box(tris, Vec3(x, -0.2f, 0), Vec3(x + 0.3f, y, 6));
        }
        box(tris, Vec3(7, -0.2f, 0), Vec3(11, 2.0f, 6));
        AABB b(Vec3(-1, -1, -1), Vec3(12, 6, 7));

        Heightfield hf;
        hf.build(tris, b, cs, ch, 48.0f);
        CompactField cf;
        cf.build(hf, survivor());
        cf.erode(survivor().radius);

        // Walk from the bottom landing to the top one.
        const auto &bottom = cf.cell(8, 12);
        check(bottom.count > 0, "the bottom landing is walkable");
        bool reached_top = false;
        if (bottom.count > 0) {
            std::vector<bool> seen(cf.spans().size(), false);
            std::vector<int32_t> stack{int32_t(bottom.start)};
            seen[bottom.start] = true;
            float best = 0.0f;
            while (!stack.empty()) {
                int32_t i = stack.back();
                stack.pop_back();
                best = std::fmax(best,
                                 b.min.y + float(cf.spans()[size_t(i)].y) * ch);
                for (int d = 0; d < 4; ++d) {
                    int32_t a = cf.spans()[size_t(i)].neighbour[d];
                    if (a == kNoNeighbour || seen[size_t(a)]) continue;
                    seen[size_t(a)] = true;
                    stack.push_back(a);
                }
            }
            reached_top = best > 1.9f;
        }
        check(reached_top, "the stairs climb to the upper landing");
    }

    // ------------------------------------------------------------ a ramp
    //
    // Twenty degrees is walkable and sixty is not, and the difference
    // is one dot product. Both ramps rise the same height over
    // different runs, so a slope test that is measuring the wrong
    // thing gets one of them wrong.
    {
        auto ramp_area = [&](float run) {
            std::vector<Vec3> tris;
            ramp(tris, 0, run, 0, 4, 0, 3.0f);
            AABB b(Vec3(-1, -1, -1), Vec3(run + 1, 5, 5));
            Heightfield hf;
            hf.build(tris, b, cs, ch, 48.0f);
            CompactField cf;
            cf.build(hf, survivor());
            return walkable_area(cf);
        };
        // 3 m up over 8.2 m is 20 degrees; over 1.7 m is 60.
        check(ramp_area(8.2f) > 25.0f, "a shallow ramp is walkable");
        check_near(ramp_area(1.7f), 0.0f, 0.01f, "a steep one is not");
    }

    // ============================================================
    //                                            polygons and paths
    // ============================================================

    // Everything below depends on three properties of the polygon
    // mesh, and all three are cheap to state and miserable to debug
    // from a wrong path: polygons are convex, adjacency is mutual,
    // and there are no cracks. A crack is the nasty one -- two
    // polygons that share part of an edge but were never matched,
    // so the mesh looks continuous and a path cannot cross.
    auto check_mesh = [&](const NavMesh &m, const char *world) {
        int nonconvex = 0, asymmetric = 0, cracks = 0;
        for (size_t i = 0; i < m.polys().size(); ++i) {
            const NavPoly &p = m.polys()[i];
            int sign = 0;
            for (int k = 0; k < p.count; ++k) {
                const Vec3 &a = m.verts()[p.verts[k]];
                const Vec3 &b = m.verts()[p.verts[(k + 1) % p.count]];
                const Vec3 &c = m.verts()[p.verts[(k + 2) % p.count]];
                float cr = (b.x - a.x) * (c.z - a.z) - (c.x - a.x) * (b.z - a.z);
                int s = cr > 1e-6f ? 1 : (cr < -1e-6f ? -1 : 0);
                if (s == 0) continue;
                if (sign == 0) sign = s;
                else if (s != sign) { ++nonconvex; break; }
            }
            for (int k = 0; k < p.count; ++k) {
                uint16_t n = p.neis[k];
                if (n == kNoPoly) {
                    // A WALL EDGE WITH MESH BEHIND IT IS A CRACK,
                    // and the distance probed matters.
                    //
                    // The two ways two polygons fail to be joined
                    // are a T-junction, where one edge meets the
                    // middle of another, and a near-miss, where two
                    // contours run parallel a cell apart because
                    // the two sides disagreed about where their
                    // shared boundary starts. A timid probe walks
                    // into the gap of the second kind and reports
                    // nothing, so this steps a whole cell out --
                    // safe, because a real wall has the body's
                    // radius of clearance behind it and a cell is
                    // much less than that.
                    //
                    // Probed at three points along the edge rather
                    // than at the middle, so a short edge near a
                    // concave corner cannot answer for the whole.
                    const Vec3 &a = m.verts()[p.verts[k]];
                    const Vec3 &b = m.verts()[p.verts[(k + 1) % p.count]];
                    Vec3 d = b - a;
                    float len = std::sqrt(d.x * d.x + d.z * d.z);
                    if (len < 1e-4f) continue;
                    Vec3 outward(-d.z / len, 0.0f, d.x / len);
                    int behind = 0;
                    for (float f : {0.25f, 0.5f, 0.75f}) {
                        Vec3 probe = a + d * f + outward * 0.26f;
                        probe.y = a.y + (b.y - a.y) * f;
                        if (m.find_poly(probe, Vec3(0.05f, 0.4f, 0.05f)) !=
                            kNoPoly)
                            ++behind;
                    }
                    if (behind == 3) ++cracks;
                    continue;
                }
                const NavPoly &q = m.polys()[n];
                bool back = false;
                for (int j = 0; j < q.count; ++j)
                    if (q.neis[j] == uint16_t(i)) back = true;
                if (!back) ++asymmetric;
            }
        }
        char what[128];
        std::snprintf(what, sizeof what, "%s: every polygon is convex", world);
        check_int(nonconvex, 0, what);
        std::snprintf(what, sizeof what, "%s: adjacency is mutual", world);
        check_int(asymmetric, 0, what);
        std::snprintf(what, sizeof what, "%s: no cracks between polygons", world);
        check_int(cracks, 0, what);
    };

    // The length of a path on the ground, ignoring the vertical, so
    // that a route over a ramp compares with one along the flat.
    auto path_length = [](const std::vector<PathPoint> &p) {
        float d = 0.0f;
        for (size_t i = 1; i < p.size(); ++i) {
            float dx = p[i].position.x - p[i - 1].position.x;
            float dz = p[i].position.z - p[i - 1].position.z;
            d += std::sqrt(dx * dx + dz * dz);
        }
        return d;
    };

    // AN INDEPENDENT ANSWER TO CHECK THE FUNNEL AGAINST.
    //
    // The shortest route across a polygon mesh bends only at the
    // mesh's own vertices -- there is nothing else for a taut string
    // to catch on. So the optimum can be found a second way, by
    // brute force: build a graph over the start, the goal and every
    // vertex, join two of them whenever a ray between them stays on
    // the mesh, and run Dijkstra. Slow, obviously wrong for a game,
    // and completely independent of the funnel, which is the point.
    //
    // Visibility is tested by walking the segment in small steps and
    // asking whether each point is on the mesh. Slower than a ray
    // walk and, for this purpose, better: an optimal route runs
    // along shared edges and through vertices, which is exactly
    // where a ray walk has to make arbitrary calls about which side
    // of an edge it is on, and the oracle must not inherit the
    // judgement calls of the thing it is checking.
    auto shortest_by_brute_force = [](const NavMesh &m, const Vec3 &from,
                                      const Vec3 &to) {
        std::vector<Vec3> node;
        node.push_back(from);
        node.push_back(to);
        for (const Vec3 &v : m.verts()) node.push_back(v);

        auto visible = [&](const Vec3 &a, const Vec3 &c) {
            float dx = a.x - c.x, dz = a.z - c.z;
            float len = std::sqrt(dx * dx + dz * dz);
            int steps = std::max(2, int(len / 0.02f));
            for (int i = 0; i <= steps; ++i) {
                float t = float(i) / float(steps);
                Vec3 p = a + (c - a) * t;
                if (m.find_poly(p, Vec3(0.02f, 1.0f, 0.02f)) == kNoPoly)
                    return false;
            }
            return true;
        };

        const size_t n = node.size();
        std::vector<float> best(n, 1e30f);
        std::vector<bool> done(n, false);
        best[0] = 0.0f;
        for (size_t it = 0; it < n; ++it) {
            size_t u = n;
            for (size_t i = 0; i < n; ++i)
                if (!done[i] && (u == n || best[i] < best[u])) u = i;
            if (u == n || best[u] >= 1e29f) break;
            done[u] = true;
            if (u == 1) break;
            for (size_t v = 0; v < n; ++v) {
                if (done[v]) continue;
                if (!visible(node[u], node[v])) continue;
                float dx = node[u].x - node[v].x, dz = node[u].z - node[v].z;
                float d = best[u] + std::sqrt(dx * dx + dz * dz);
                if (d < best[v]) best[v] = d;
            }
        }
        return best[1];
    };

    BakeSettings settings;
    settings.agent = survivor();
    settings.cell_size = cs;
    settings.cell_height = ch;

    // ------------------------------------------------- an open floor
    //
    // Nothing in the way, so the only correct path is the straight
    // line. This is the funnel's simplest possible job and the one
    // it must not get wrong: if crossing twenty polygons in a row
    // produces twenty waypoints, the string is not being pulled.
    {
        std::vector<Vec3> tris;
        box(tris, Vec3(0, -0.2f, 0), Vec3(20, 0, 20));
        AABB b(Vec3(-1, -1, -1), Vec3(21, 4, 21));

        NavMesh mesh;
        BakeStats st;
        check(mesh.bake(tris, b, settings, &st), "an open floor bakes");
        check(st.polys > 0 && st.polys < 40,
              "and comes out as a handful of polygons, not hundreds");
        check_mesh(mesh, "open floor");

        std::vector<PathPoint> path;
        check(mesh.find_path(Vec3(2, 0, 2), Vec3(18, 0, 18), &path),
              "a path across it is found");
        check_int(int(path.size()), 2, "with no corners, because there are none");
        check_near(path_length(path), std::sqrt(2.0f) * 16.0f, 0.3f,
                   "and it is the straight line");

        Vec3 hit;
        check(mesh.raycast(Vec3(2, 0, 2), Vec3(18, 0, 18), &hit),
              "and a ray across it is unobstructed");
    }

    // --------------------------------------------------- around a corner
    //
    // An L of two corridors. The shortest route hugs the inside
    // corner, so the path must have exactly one bend and must be
    // very close to the two straight runs that meet at it. A path
    // that wanders from polygon centre to polygon centre is a good
    // deal longer, which is what makes this measurable rather than
    // a matter of taste.
    {
        std::vector<Vec3> tris;
        box(tris, Vec3(0, -0.2f, 0), Vec3(12, 0, 4));    // the long arm
        box(tris, Vec3(8, -0.2f, 0), Vec3(12, 0, 12));   // the short one
        // Walls, so the corridors are corridors.
        box(tris, Vec3(-0.4f, -0.2f, -0.4f), Vec3(12.4f, 2.5f, 0.0f));
        box(tris, Vec3(-0.4f, -0.2f, -0.4f), Vec3(0.0f, 2.5f, 4.4f));
        box(tris, Vec3(-0.4f, -0.2f, 4.0f), Vec3(8.0f, 2.5f, 4.4f));
        box(tris, Vec3(8.0f, -0.2f, 4.0f), Vec3(8.4f, 2.5f, 12.4f));
        box(tris, Vec3(12.0f, -0.2f, -0.4f), Vec3(12.4f, 2.5f, 12.4f));
        box(tris, Vec3(8.0f, -0.2f, 12.0f), Vec3(12.4f, 2.5f, 12.4f));
        AABB b(Vec3(-1, -1, -1), Vec3(13, 4, 13));

        NavMesh mesh;
        check(mesh.bake(tris, b, settings, nullptr), "the L bakes");
        check_mesh(mesh, "L corridor");

        std::vector<PathPoint> path;
        bool partial = true;
        check(mesh.find_path(Vec3(1, 0, 2), Vec3(10, 0, 11), &path, &partial),
              "a path round the corner is found");
        check(!partial, "and it reaches the far end");
        check(path.size() >= 3 && path.size() <= 5,
              "it bends once or twice at the inside corner, not twenty times");

        // The inside corner is at (8, 4) before erosion. Held off by
        // the body's radius plus the ledge cell, the taut route runs
        // from the start to somewhere near there and on to the goal.
        float direct = std::sqrt(9.0f * 9.0f + 9.0f * 9.0f);
        check(path_length(path) > direct,
              "it is longer than the straight line, which goes through a wall");

        // Optimal, to within the slack the nudged probes cost the
        // brute-force answer. Not "about right" -- as short as any
        // route over this mesh can be.
        float ideal = shortest_by_brute_force(mesh, Vec3(1, 0, 2),
                                              Vec3(10, 0, 11));
        check_near(path_length(path), ideal, 0.15f,
                   "and it is as short as any route over this mesh");

        // Every leg of a taut path lies on the mesh. If one does
        // not, the funnel has pulled the string through a wall.
        int off_mesh = 0;
        for (size_t i = 1; i < path.size(); ++i) {
            Vec3 hit;
            if (!mesh.raycast(path[i - 1].position, path[i].position, &hit))
                ++off_mesh;
        }
        check_int(off_mesh, 0, "and every leg of it stays on the navmesh");

        // The wall is really a wall.
        Vec3 hit;
        check(!mesh.raycast(Vec3(1, 0, 2), Vec3(10, 0, 11), &hit),
              "a ray straight at the goal hits the corner wall");
    }

    // ------------------------------------------------ islands and links
    //
    // Two platforms with a gap between them. Nothing can walk across,
    // so a path must fail -- come back partial -- until a link is
    // added, and then it must go through the link and say so. This
    // is the whole reason links exist: without them a roof is an
    // island and nothing on it can plan a route to the street.
    {
        std::vector<Vec3> tris;
        box(tris, Vec3(0, -0.2f, 0), Vec3(8, 0, 8));
        box(tris, Vec3(14, 2.8f, 0), Vec3(22, 3.0f, 8));  // a roof, higher
        AABB b(Vec3(-1, -1, -1), Vec3(23, 6, 9));

        NavMesh mesh;
        BakeStats st;
        check(mesh.bake(tris, b, settings, &st), "two platforms bake");
        check_mesh(mesh, "two platforms");

        std::vector<PathPoint> path;
        bool partial = false;
        check(mesh.find_path(Vec3(4, 0, 4), Vec3(18, 3, 4), &path, &partial),
              "asking to cross the gap still answers");
        check(partial, "but only partly, because there is no way across");

        NavLink ladder;
        ladder.from = Vec3(7.0f, 0.0f, 4.0f);
        ladder.to = Vec3(15.0f, 3.0f, 4.0f);
        ladder.radius = 1.2f;
        ladder.name = "ladder";
        ladder.bidirectional = true;
        mesh.add_link(ladder);
        check(mesh.links()[0].from_poly != kNoPoly &&
                  mesh.links()[0].to_poly != kNoPoly,
              "the link finds a polygon at each end");

        partial = true;
        check(mesh.find_path(Vec3(4, 0, 4), Vec3(18, 3, 4), &path, &partial),
              "with the ladder there is a path");
        check(!partial, "and it is a complete one");

        int link_legs = 0;
        for (const PathPoint &p : path)
            if (p.flags == kPathLink) ++link_legs;
        check_int(link_legs, 1, "which uses the ladder exactly once");

        // And the other way, because the link said it was two-way.
        partial = true;
        check(mesh.find_path(Vec3(18, 3, 4), Vec3(4, 0, 4), &path, &partial) &&
                  !partial,
              "and back down again");
    }

    // ---------------------------------------------------- save and load
    //
    // A bake of a town is seconds of work; doing it at every level
    // load is seconds nobody wants to wait. What comes back must
    // path identically, which is a stronger check than comparing
    // bytes -- it is the property anyone actually depends on.
    {
        std::vector<Vec3> tris;
        box(tris, Vec3(0, -0.2f, 0), Vec3(10, 0, 10));
        box(tris, Vec3(4.9f, 0, 0), Vec3(5.1f, 2.5f, 7.0f));
        AABB b(Vec3(-1, -1, -1), Vec3(11, 4, 11));

        NavMesh mesh;
        mesh.bake(tris, b, settings, nullptr);
        NavLink hop;
        hop.from = Vec3(4.0f, 0.0f, 2.0f);
        hop.to = Vec3(6.0f, 0.0f, 2.0f);
        hop.radius = 1.0f;
        hop.name = "vault";
        mesh.add_link(hop);

        std::vector<uint8_t> blob = mesh.save();
        check(blob.size() > 32, "the mesh writes out");

        NavMesh back;
        check(back.load(blob), "and reads back in");
        check_int(back.poly_count(), mesh.poly_count(), "with the same polygons");
        check_int(back.vert_count(), mesh.vert_count(), "and the same vertices");
        check_int(back.link_count(), 1, "and the link");
        check_mesh(back, "reloaded");

        std::vector<PathPoint> a, c;
        mesh.find_path(Vec3(1, 0, 9), Vec3(9, 0, 9), &a);
        back.find_path(Vec3(1, 0, 9), Vec3(9, 0, 9), &c);
        check_int(int(c.size()), int(a.size()), "and it paths the same way");
        check_near(path_length(c), path_length(a), 1e-3f,
                   "over exactly the same distance");
    }

    // ------------------------------------------- two rooms, and a door
    //
    // The bake cuts open ground into regions wherever the ground
    // narrows, so any level with a doorway in it comes out as more
    // than one region, and every region seam is a place two contours
    // have to agree on to the last bit. Where they do not, the
    // polygons are never joined: the mesh looks continuous, the
    // crack detector above sees a wall with floor behind it, and a
    // path from one room to the other is reported impossible.
    //
    // This is the check that a multi-region bake is actually one
    // navmesh and not several laid side by side.
    {
        std::vector<Vec3> tris;
        box(tris, Vec3(-10, -0.2f, -10), Vec3(10, 0, 10));
        box(tris, Vec3(-0.3f, 0, -10), Vec3(0.3f, 2.5f, -2));
        box(tris, Vec3(-0.3f, 0, 2), Vec3(0.3f, 2.5f, 10));
        AABB b(Vec3(-11.2f, -1.4f, -11.2f), Vec3(11.2f, 3, 11.2f));

        NavMesh mesh;
        BakeStats st;
        check(mesh.bake(tris, b, settings, &st), "two rooms and a door bake");
        check(st.regions >= 2, "and come out as more than one region");
        check_mesh(mesh, "two rooms");

        // The polygons on the two sides must be joined through the
        // doorway, and joined means adjacency -- not merely that
        // both halves exist.
        uint16_t left = mesh.find_poly(Vec3(-5, 0, -6), Vec3(1, 2, 1));
        uint16_t right = mesh.find_poly(Vec3(5, 0, -6), Vec3(1, 2, 1));
        check(left != kNoPoly && right != kNoPoly,
              "there is floor in both rooms");
        std::vector<bool> seen(mesh.polys().size(), false);
        std::vector<uint16_t> stack{left};
        if (left != kNoPoly) seen[left] = true;
        bool joined = false;
        while (!stack.empty()) {
            uint16_t p = stack.back();
            stack.pop_back();
            if (p == right) joined = true;
            for (int k = 0; k < mesh.polys()[p].count; ++k) {
                uint16_t n = mesh.polys()[p].neis[k];
                if (n == kNoPoly || seen[n]) continue;
                seen[n] = true;
                stack.push_back(n);
            }
        }
        check(joined, "and the polygon graph goes through the doorway");

        std::vector<PathPoint> path;
        bool partial = true;
        check(mesh.find_path(Vec3(-5, 0, -6), Vec3(5, 0, -6), &path, &partial),
              "a path from one room to the other is found");
        check(!partial, "and it is a whole one, not a walk up to the wall");

        int off_mesh = 0;
        for (size_t i = 1; i < path.size(); ++i) {
            Vec3 h;
            if (!mesh.raycast(path[i - 1].position, path[i].position, &h))
                ++off_mesh;
        }
        check_int(off_mesh, 0, "every leg of which stays on the navmesh");
    }

    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
