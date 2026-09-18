#include "nav/contour.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace wr::nav {

namespace {

// Twice the signed area of a polygon in the xz plane. Positive is
// counter-clockwise looking down the +y axis. Sign is how an outline
// is told from a hole, so it is worth being precise about: this is
// the shoelace sum, in integers, exact.
int64_t area2(const std::vector<ContourVert> &v) {
    int64_t a = 0;
    for (size_t i = 0, j = v.size() - 1; i < v.size(); j = i++)
        a += int64_t(v[j].x) * int64_t(v[i].z) - int64_t(v[i].x) * int64_t(v[j].z);
    return a;
}

int64_t cross2(const ContourVert &a, const ContourVert &b,
               const ContourVert &c) {
    return int64_t(b.x - a.x) * int64_t(c.z - a.z) -
           int64_t(c.x - a.x) * int64_t(b.z - a.z);
}

// THE HEIGHT AT A CELL CORNER. A contour vertex sits on the corner
// shared by up to four cells, which may be at four different
// heights -- the top of a kerb and the road beside it meet at a
// corner belonging to both. Taking the highest is what puts the
// navmesh edge on top of the kerb rather than sunk into it, and a
// body walking the edge stays above the ground rather than in it.
int corner_height(const CompactField &f, int32_t i, int dir) {
    const std::vector<CompactSpan> &spans = f.spans();
    const CompactSpan &s = spans[size_t(i)];
    int h = s.y;
    const int dirp = (dir + 1) & 3;

    int32_t a = s.neighbour[dir];
    if (a != kNoNeighbour) {
        h = std::max(h, int(spans[size_t(a)].y));
        int32_t b = spans[size_t(a)].neighbour[dirp];
        if (b != kNoNeighbour) h = std::max(h, int(spans[size_t(b)].y));
    }
    a = s.neighbour[dirp];
    if (a != kNoNeighbour) {
        h = std::max(h, int(spans[size_t(a)].y));
        int32_t b = spans[size_t(a)].neighbour[dir];
        if (b != kNoNeighbour) h = std::max(h, int(spans[size_t(b)].y));
    }
    return h;
}

// WALK ONE BOUNDARY LOOP, left hand on the wall.
//
// Standing on a cell facing `dir`: if the edge in front is a
// boundary, note the corner and turn right without moving. If it is
// open, step through it and turn left. That traces the outside of
// the region exactly once and comes back to where it started facing
// the way it started, which is the loop's own termination proof.
//
// The corner noted is the one on the left end of the edge being
// faced, so consecutive boundary edges hand over cleanly and the
// polygon closes.
void walk_contour(const CompactField &f, int x, int z, int32_t i, int dir,
                  std::vector<uint8_t> &flags, std::vector<ContourVert> &out) {
    const std::vector<CompactSpan> &spans = f.spans();
    const int start_dir = dir;
    const int32_t start_i = i;

    for (int guard = 0; guard < 1 << 22; ++guard) {
        if (flags[size_t(i)] & (1u << dir)) {
            int px = x, pz = z;
            switch (dir) {
                case 0: pz++; break;
                case 1: px++; pz++; break;
                case 2: px++; break;
                default: break;
            }
            ContourVert v;
            v.x = px;
            v.y = corner_height(f, i, dir);
            v.z = pz;
            int32_t a = spans[size_t(i)].neighbour[dir];
            v.region = a == kNoNeighbour ? kNoRegion : spans[size_t(a)].region;
            out.push_back(v);

            flags[size_t(i)] &= uint8_t(~(1u << dir));
            dir = (dir + 1) & 3;
        } else {
            int32_t a = spans[size_t(i)].neighbour[dir];
            f.step(x, z, dir, &x, &z);
            i = a;
            dir = (dir + 3) & 3;
        }
        if (i == start_i && dir == start_dir) break;
    }
}

// SIMPLIFY, keeping the joins.
//
// Every point where the neighbouring region changes stays, because
// the region next door will keep the same point and the two outlines
// must meet there. Between those fixed points the boundary is fitted
// by splitting at whichever point strays furthest, until nothing
// strays more than `max_error`. That is Douglas-Peucker, and its one
// useful property here is determinism: the same stretch of boundary
// seen from either side simplifies identically.
void simplify(const std::vector<ContourVert> &raw, float max_error,
              int max_edge_cells, std::vector<ContourVert> &out) {
    const int n = int(raw.size());
    out.clear();
    if (n < 3) return;

    // WHICH VERTEX OF A RUN IS THE FIXED POINT, and it is the last
    // one, which is not a detail.
    //
    // The walk records on each vertex the region across the edge
    // ARRIVING at it, because that is the edge it was facing when
    // it emitted the corner. So a stretch of boundary shared with
    // region B is a run of vertices carrying B, and the corner
    // where that stretch BEGINS is the last vertex of the run
    // before it -- not the first vertex of the run itself.
    //
    // Region B walks the same stretch the other way and, applying
    // the same rule, arrives at the same two corners. Take the
    // first of each run instead and the two sides bracket the seam
    // one cell apart: neither the vertices nor the edge match, the
    // polygons never get joined, and the level comes out as two
    // rooms with no door between them. Which is exactly the bug
    // this rule was written to fix.
    bool has_joins = false;
    for (int i = 0; i < n; ++i)
        if (raw[size_t(i)].region != raw[size_t((i + 1) % n)].region)
            has_joins = true;

    std::vector<int> keep;
    if (has_joins) {
        for (int i = 0; i < n; ++i) {
            int next = (i + 1) % n;
            if (raw[size_t(i)].region != raw[size_t(next)].region)
                keep.push_back(i);
        }
    } else {
        // A loop with no joins touches nothing: an outer wall, or an
        // island. Any two opposite points will do to start, so pick
        // extremes -- they are the two that are certainly on the
        // hull, so the fit cannot start inside the shape.
        int lo = 0, hi = 0;
        for (int i = 1; i < n; ++i) {
            const ContourVert &v = raw[size_t(i)];
            if (v.x < raw[size_t(lo)].x ||
                (v.x == raw[size_t(lo)].x && v.z < raw[size_t(lo)].z))
                lo = i;
            if (v.x > raw[size_t(hi)].x ||
                (v.x == raw[size_t(hi)].x && v.z > raw[size_t(hi)].z))
                hi = i;
        }
        if (lo == hi) return;
        keep.push_back(lo);
        keep.push_back(hi);
    }
    std::sort(keep.begin(), keep.end());
    keep.erase(std::unique(keep.begin(), keep.end()), keep.end());
    if (keep.size() < 2) return;

    // Split the worst-fitting stretch until all fit. `keep` stays
    // sorted, so each insertion is into the segment being examined
    // and the scan can simply re-examine it.
    const float err_sq = max_error * max_error;
    for (size_t si = 0; si < keep.size();) {
        int ia = keep[si];
        int ib = keep[(si + 1) % keep.size()];
        const ContourVert &a = raw[size_t(ia)];
        const ContourVert &b = raw[size_t(ib)];

        float dx = float(b.x - a.x), dz = float(b.z - a.z);
        float len_sq = dx * dx + dz * dz;

        int worst = -1;
        float worst_d = err_sq;
        int step = 1, ci = (ia + 1) % n, end = ib;
        // Walk forward round the loop from a to b.
        for (int k = ci; k != end; k = (k + step) % n) {
            const ContourVert &p = raw[size_t(k)];
            float d;
            if (len_sq > 0.0f) {
                float t = (float(p.x - a.x) * dx + float(p.z - a.z) * dz) / len_sq;
                t = std::clamp(t, 0.0f, 1.0f);
                float ex = float(a.x) + dx * t - float(p.x);
                float ez = float(a.z) + dz * t - float(p.z);
                d = ex * ex + ez * ez;
            } else {
                float ex = float(p.x - a.x), ez = float(p.z - a.z);
                d = ex * ex + ez * ez;
            }
            if (d > worst_d) {
                worst_d = d;
                worst = k;
            }
        }
        if (worst >= 0) {
            keep.insert(keep.begin() + long(si) + 1, worst);
            // Do not advance: the first half of the split stretch
            // may itself still need splitting.
        } else {
            ++si;
        }
    }

    // Long edges get split at their midpoint on the raw boundary.
    // A polygon whose interior is far from all of its vertices makes
    // for bad heights when a path is projected onto it, and for a
    // funnel that has nothing to pull against.
    if (max_edge_cells > 0) {
        for (size_t si = 0; si < keep.size() && keep.size() < 512;) {
            int ia = keep[si];
            int ib = keep[(si + 1) % keep.size()];
            const ContourVert &a = raw[size_t(ia)];
            const ContourVert &b = raw[size_t(ib)];
            int dx = b.x - a.x, dz = b.z - a.z;
            if (dx * dx + dz * dz > max_edge_cells * max_edge_cells) {
                int span = (ib > ia) ? (ib - ia) : (ib + n - ia);
                if (span > 1) {
                    // WHICH MIDPOINT, and this is not pedantry. The
                    // region on the other side simplifies this same
                    // stretch of boundary walking the other way, and
                    // if the two pick different corners to split at,
                    // the polygons no longer share an edge and the
                    // navmesh has a crack a path cannot cross.
                    //
                    // Rounding by index gives opposite answers from
                    // the two directions when the stretch has an odd
                    // number of corners. Rounding by the direction
                    // of the edge in space does not: both sides see
                    // the same two endpoints, so both compute the
                    // same comparison and land on the same corner.
                    bool forward = (b.x > a.x) || (b.x == a.x && b.z > a.z);
                    int half = forward ? span / 2 : (span + 1) / 2;
                    keep.insert(keep.begin() + long(si) + 1, (ia + half) % n);
                    continue;
                }
            }
            ++si;
        }
    }

    out.reserve(keep.size());
    for (int k : keep) {
        // Turn the bookkeeping round on the way out. Everything
        // downstream wants "the region across the edge LEAVING this
        // vertex", because that is how a polygon's edges are
        // numbered; the walk recorded the edge arriving. The edge
        // leaving a kept vertex is the one arriving at the next raw
        // point, and the whole stretch up to the next kept vertex
        // borders that same region, by construction.
        ContourVert v = raw[size_t(k)];
        v.region = raw[size_t((k + 1) % n)].region;
        out.push_back(v);
    }
}

// Drop consecutive duplicate points, and any spur where a vertex
// doubles straight back on itself. Both come out of simplification
// on thin regions, and either one makes the triangulator loop.
void remove_degenerate(std::vector<ContourVert> &v) {
    for (size_t i = 0; i < v.size() && v.size() > 3;) {
        size_t j = (i + 1) % v.size();
        if (v[i].x == v[j].x && v[i].z == v[j].z) {
            v.erase(v.begin() + long(j));
        } else {
            ++i;
        }
    }
}

// ------------------------------------------------------------ holes
//
// A region shaped like a ring -- open ground around a pillar, a
// gallery round a stairwell -- has an outer boundary and an inner
// one. The inner one is a hole, and a hole cannot be triangulated
// alongside its outline as two separate loops.
//
// The fix is a zero-width bridge: cut the outline open at one
// vertex, run round the hole, and come back. The result is one loop
// that traces out to the hole, round it, and back, with two
// coincident edges in the middle that enclose no area. Every
// triangulator handles it, and the navmesh is correct -- the hole is
// outside the polygon because the bridge separates it.
bool segments_cross(const ContourVert &a, const ContourVert &b,
                    const ContourVert &c, const ContourVert &d) {
    int64_t d1 = cross2(c, d, a), d2 = cross2(c, d, b);
    int64_t d3 = cross2(a, b, c), d4 = cross2(a, b, d);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

bool bridge_is_clear(const std::vector<ContourVert> &loop, size_t skip_a,
                     size_t skip_b, const ContourVert &p,
                     const ContourVert &q) {
    for (size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++) {
        if (i == skip_a || j == skip_a || i == skip_b || j == skip_b) continue;
        if (segments_cross(p, q, loop[j], loop[i])) return false;
    }
    return true;
}

void merge_hole(std::vector<ContourVert> &outline,
                std::vector<ContourVert> &hole) {
    // Closest mutually visible pair. Closest alone is nearly always
    // visible; the check is there for the times it is not, and
    // falling back to the closest pair regardless beats giving up,
    // because a dropped hole is a navmesh laid over a pillar.
    size_t best_o = 0, best_h = 0;
    int64_t best_d = INT64_MAX;
    size_t fallback_o = 0, fallback_h = 0;
    int64_t fallback_d = INT64_MAX;
    for (size_t o = 0; o < outline.size(); ++o) {
        for (size_t h = 0; h < hole.size(); ++h) {
            int64_t dx = outline[o].x - hole[h].x;
            int64_t dz = outline[o].z - hole[h].z;
            int64_t d = dx * dx + dz * dz;
            if (d < fallback_d) {
                fallback_d = d;
                fallback_o = o;
                fallback_h = h;
            }
            if (d >= best_d) continue;
            if (!bridge_is_clear(outline, o, o, outline[o], hole[h])) continue;
            if (!bridge_is_clear(hole, h, h, outline[o], hole[h])) continue;
            best_d = d;
            best_o = o;
            best_h = h;
        }
    }
    if (best_d == INT64_MAX) {
        best_o = fallback_o;
        best_h = fallback_h;
    }

    std::vector<ContourVert> merged;
    merged.reserve(outline.size() + hole.size() + 2);
    for (size_t k = 0; k <= best_o; ++k) merged.push_back(outline[k]);
    for (size_t k = 0; k < hole.size(); ++k)
        merged.push_back(hole[(best_h + k) % hole.size()]);
    merged.push_back(hole[best_h]);
    for (size_t k = best_o; k < outline.size(); ++k) merged.push_back(outline[k]);
    outline.swap(merged);
}

}  // namespace

bool build_contours(const CompactField &field, float max_error,
                    int max_edge_cells, ContourSet *out) {
    if (!out) return false;
    out->contours.clear();
    out->bounds = field.bounds();
    out->cell_size = field.cell_size();
    out->cell_height = field.cell_height();
    out->width = field.width();
    out->depth = field.depth();
    out->merged_holes = 0;

    const std::vector<CompactSpan> &spans = field.spans();
    if (spans.empty()) return true;

    // Which of a cell's four edges are on a region boundary. Doing
    // this up front means the walk never has to ask twice, and
    // clearing a bit as it is consumed is what stops one loop being
    // traced from every cell along it.
    std::vector<uint8_t> flags(spans.size(), 0);
    for (int z = 0; z < field.depth(); ++z) {
        for (int x = 0; x < field.width(); ++x) {
            const CompactField::Cell &c = field.cell(x, z);
            for (uint32_t i = c.start; i < c.start + c.count; ++i) {
                if (spans[i].region == kNoRegion) continue;
                uint8_t f = 0;
                for (int dir = 0; dir < 4; ++dir) {
                    int32_t a = spans[i].neighbour[dir];
                    uint16_t r = a == kNoNeighbour ? kNoRegion
                                                   : spans[size_t(a)].region;
                    if (r != spans[i].region) f |= uint8_t(1u << dir);
                }
                flags[i] = f;
            }
        }
    }

    // Loops per region, so that a ring's outline and hole can be
    // matched up afterwards.
    std::vector<std::vector<std::vector<ContourVert>>> per_region(
        size_t(field.region_count()));

    std::vector<ContourVert> raw, simple;
    for (int z = 0; z < field.depth(); ++z) {
        for (int x = 0; x < field.width(); ++x) {
            const CompactField::Cell &c = field.cell(x, z);
            for (uint32_t i = c.start; i < c.start + c.count; ++i) {
                if (flags[i] == 0) continue;
                uint16_t region = spans[i].region;
                if (region == kNoRegion) continue;

                int dir = 0;
                while (dir < 4 && !(flags[i] & (1u << dir))) ++dir;
                if (dir == 4) continue;

                raw.clear();
                walk_contour(field, x, z, int32_t(i), dir, flags, raw);
                if (raw.size() < 3) continue;

                simple.clear();
                simplify(raw, max_error, max_edge_cells, simple);
                remove_degenerate(simple);
                if (simple.size() < 3) continue;
                per_region[region].push_back(simple);
            }
        }
    }

    for (uint16_t r = 1; r < uint16_t(field.region_count()); ++r) {
        std::vector<std::vector<ContourVert>> &loops = per_region[r];
        if (loops.empty()) continue;

        // The outline is the loop enclosing the most area. Holes come
        // out wound the other way, so the comparison is on magnitude
        // and the sign then says which is which.
        size_t outline = 0;
        int64_t best = std::llabs(area2(loops[0]));
        for (size_t k = 1; k < loops.size(); ++k) {
            int64_t a = std::llabs(area2(loops[k]));
            if (a > best) {
                best = a;
                outline = k;
            }
        }
        std::vector<ContourVert> merged = loops[outline];
        const bool outline_ccw = area2(merged) > 0;

        for (size_t k = 0; k < loops.size(); ++k) {
            if (k == outline) continue;
            std::vector<ContourVert> hole = loops[k];
            // A hole must run opposite to its outline, or the bridge
            // splices it in the wrong way round and the triangulator
            // covers the hole instead of avoiding it.
            if ((area2(hole) > 0) == outline_ccw)
                std::reverse(hole.begin(), hole.end());
            merge_hole(merged, hole);
            ++out->merged_holes;
        }

        Contour c;
        c.region = r;
        c.verts = std::move(merged);
        out->contours.push_back(std::move(c));
    }
    return true;
}

}  // namespace wr::nav
