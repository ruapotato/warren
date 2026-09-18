// Warren -- cutting the walkable surface into regions.
//
// A navmesh is a set of convex polygons. Getting there from a field
// of cells means first cutting that field into pieces that are
// roughly convex and simply connected, because a contour traced
// round such a piece triangulates cleanly and a contour traced round
// an L-shaped piece with a hole in it does not.
//
// WATERSHED, and why. Think of the distance field as terrain: high
// in the middle of open ground, zero at every wall. Flood it from
// the peaks downward. Each peak starts a region; as the water level
// drops, regions grow outward over the newly exposed ground, and
// where two regions meet they stop. The seams land in the narrow
// places -- doorways, corridor junctions -- which is exactly where a
// human would cut, and the pieces come out fat rather than stringy.
//
// The alternative, sweeping the grid in rows and cutting at every
// change, is far less code and produces long thin slivers that meet
// at hundreds of tiny edges. Paths through them zig-zag and the
// polygon count goes up, not down. The watershed earns its length.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "nav/heightfield.h"

namespace wr::nav {

namespace {

// A region as the filter pass sees it: how big, what it touches, and
// what it overlaps.
struct RegionInfo {
    uint16_t id = 0;
    int cells = 0;
    // Neighbouring region ids IN BOUNDARY ORDER, which is the whole
    // point -- see can_merge().
    std::vector<uint16_t> connections;
    // Regions occupying the same cell at a different height. A
    // mezzanine over a hall touches it in plan everywhere and is
    // reachable from it nowhere.
    std::vector<uint16_t> floors;
    bool visited = false;
    bool remap = false;
};

void add_unique(std::vector<uint16_t> &v, uint16_t x) {
    if (std::find(v.begin(), v.end(), x) == v.end()) v.push_back(x);
}

}  // namespace

// Grow the regions that already exist into ground newly exposed by
// the falling water level. Each unassigned cell takes the region of
// whichever neighbour is closest to its own region's seed, so a
// region spreads evenly rather than racing down one corridor.
void CompactField::expand_regions(int level, uint16_t *src_region,
                                  uint16_t *src_dist,
                                  std::vector<int32_t> &stack) {
    stack.clear();
    for (int32_t i = 0; i < int32_t(spans_.size()); ++i)
        if (spans_[size_t(i)].dist >= level && src_region[i] == kNoRegion)
            stack.push_back(i);

    // Bounded, because a level whose newly exposed ground is one big
    // sheet would otherwise be filled entirely by whichever region
    // happens to touch it first. Stopping early leaves the rest to
    // seed new regions, which is the behaviour that keeps regions
    // comparable in size.
    const int max_iterations = 8;
    int iteration = 0;
    std::vector<std::pair<int32_t, std::pair<uint16_t, uint16_t>>> dirty;

    while (!stack.empty()) {
        int failed = 0;
        dirty.clear();
        for (size_t j = 0; j < stack.size(); ++j) {
            int32_t i = stack[j];
            if (i < 0) {
                ++failed;
                continue;
            }
            uint16_t r = kNoRegion;
            uint16_t d2 = 0xffff;
            const CompactSpan &s = spans_[size_t(i)];
            for (int dir = 0; dir < 4; ++dir) {
                int32_t a = s.neighbour[dir];
                if (a == kNoNeighbour) continue;
                if (src_region[a] == kNoRegion) continue;
                if (uint32_t(src_dist[a]) + 2 < d2) {
                    r = src_region[a];
                    d2 = uint16_t(src_dist[a] + 2);
                }
            }
            if (r != kNoRegion) {
                stack[j] = -1;
                dirty.push_back({i, {r, d2}});
            } else {
                ++failed;
            }
        }
        // Applied after the sweep, not during it, so that the result
        // does not depend on the order cells happen to sit in.
        for (const auto &e : dirty) {
            src_region[e.first] = e.second.first;
            src_dist[e.first] = e.second.second;
        }
        if (failed == int(stack.size())) break;
        if (level > 0 && ++iteration >= max_iterations) break;
    }
}

// Flood one new region out from a seed, stopping where the ground
// falls below this level or where another region is already met.
bool CompactField::flood_region(int32_t start, int level, uint16_t region,
                                uint16_t *src_region, uint16_t *src_dist,
                                std::vector<int32_t> &stack) {
    const int lo = level >= 2 ? level - 2 : 0;
    stack.clear();
    stack.push_back(start);
    src_region[start] = region;
    src_dist[start] = 0;
    int count = 0;

    while (!stack.empty()) {
        int32_t c = stack.back();
        stack.pop_back();
        const CompactSpan &cs = spans_[size_t(c)];

        // Back off if anything adjacent -- including diagonally --
        // already belongs elsewhere. Leaving a one-cell gap between
        // two regions is what stops them interlocking along a ragged
        // seam that no contour can trace.
        uint16_t other = kNoRegion;
        for (int dir = 0; dir < 4 && other == kNoRegion; ++dir) {
            int32_t a = cs.neighbour[dir];
            if (a == kNoNeighbour) continue;
            if (src_region[a] != kNoRegion && src_region[a] != region) {
                other = src_region[a];
                break;
            }
            int32_t b = spans_[size_t(a)].neighbour[(dir + 1) & 3];
            if (b != kNoNeighbour && src_region[b] != kNoRegion &&
                src_region[b] != region)
                other = src_region[b];
        }
        if (other != kNoRegion) {
            src_region[c] = kNoRegion;
            continue;
        }
        ++count;

        for (int dir = 0; dir < 4; ++dir) {
            int32_t a = cs.neighbour[dir];
            if (a == kNoNeighbour) continue;
            if (spans_[size_t(a)].dist < lo) continue;
            if (src_region[a] != kNoRegion) continue;
            src_region[a] = region;
            src_dist[a] = 0;
            stack.push_back(a);
        }
    }
    return count > 0;
}

namespace {

// Walk the boundary of one region, collecting the region on the far
// side of each boundary edge IN ORDER. Order is what makes the merge
// test possible: a neighbour that appears once in the walk touches
// along a single stretch, and two regions that touch along a single
// stretch can be merged into one simply-connected piece. A
// neighbour that appears twice wraps around something, and merging
// there would produce a region with a hole in it.
void walk_region_boundary(const CompactField &f, int x, int z, int32_t i,
                          const std::vector<uint16_t> &reg,
                          std::vector<uint16_t> &out) {
    const std::vector<CompactSpan> &spans = f.spans();
    // Start on a direction that faces out of the region.
    int dir = 0;
    while (dir < 4) {
        int32_t a = spans[size_t(i)].neighbour[dir];
        if (a == kNoNeighbour || reg[size_t(a)] != reg[size_t(i)]) break;
        ++dir;
    }
    if (dir == 4) return;  // interior cell, nothing to walk

    const int start_dir = dir;
    const int32_t start_i = i;
    const uint16_t me = reg[size_t(i)];
    uint16_t last = 0xffff;

    // Left-hand rule on the cell grid: while the edge in front is a
    // boundary, record it and turn right; when it opens, step
    // through and turn left. Terminates because the boundary of a
    // finite region is a finite closed loop.
    for (int guard = 0; guard < 40000; ++guard) {
        int32_t a = spans[size_t(i)].neighbour[dir];
        uint16_t r = (a == kNoNeighbour) ? 0 : reg[size_t(a)];
        if (a == kNoNeighbour || r != me) {
            if (r != last) {
                out.push_back(r);
                last = r;
            }
            dir = (dir + 1) & 3;  // turn right, stay put
        } else {
            i = a;
            f.step(x, z, dir, &x, &z);
            dir = (dir + 3) & 3;  // turn left, having stepped
        }
        if (i == start_i && dir == start_dir) break;
    }

    // The walk is a loop, so its first and last entries are adjacent
    // and a neighbour spanning the seam would otherwise be counted
    // twice.
    if (out.size() > 1 && out.front() == out.back()) out.pop_back();
}

}  // namespace

void CompactField::filter_small_regions(int min_cells) {
    const size_t n = spans_.size();
    if (n == 0 || region_count_ <= 1) return;

    std::vector<uint16_t> reg(n);
    for (size_t i = 0; i < n; ++i) reg[i] = spans_[i].region;

    std::vector<RegionInfo> regions;
    regions.resize(size_t(region_count_));
    for (int r = 0; r < region_count_; ++r) regions[size_t(r)].id = uint16_t(r);

    // Sizes, boundaries and overlaps, in one sweep.
    for (int z = 0; z < depth_; ++z) {
        for (int x = 0; x < width_; ++x) {
            const Cell &c = cell(x, z);
            for (uint32_t i = c.start; i < c.start + c.count; ++i) {
                uint16_t r = reg[i];
                if (r == kNoRegion) continue;
                RegionInfo &info = regions[r];
                ++info.cells;

                // Another region in the same column is a different
                // storey, never a neighbour.
                for (uint32_t j = c.start; j < c.start + c.count; ++j) {
                    if (i == j) continue;
                    if (reg[j] != kNoRegion && reg[j] != r)
                        add_unique(info.floors, reg[j]);
                }
                if (!info.connections.empty()) continue;

                // Only the first cell of a region found on a
                // boundary needs walking: one walk yields the whole
                // ordered connection list.
                bool on_edge = false;
                for (int dir = 0; dir < 4; ++dir) {
                    int32_t a = spans_[i].neighbour[dir];
                    if (a == kNoNeighbour || reg[size_t(a)] != r) on_edge = true;
                }
                if (on_edge)
                    walk_region_boundary(*this, x, z, int32_t(i), reg,
                                         info.connections);
            }
        }
    }

    // A region that touches nothing but the void is an island: a
    // ledge on a chimney, a patch on a lamp post. Nothing can path to
    // it, and leaving it in means a body can be ordered somewhere it
    // will never arrive. Small ones go.
    for (int r = 1; r < region_count_; ++r) {
        RegionInfo &info = regions[size_t(r)];
        if (info.cells == 0 || info.cells >= min_cells) continue;
        bool connected = false;
        for (uint16_t c : info.connections)
            if (c != kNoRegion) connected = true;
        if (!connected) {
            info.cells = 0;
            info.id = kNoRegion;
        }
    }

    // Merge what is left of the small regions into a neighbour. Two
    // conditions, and the second is the one that matters: the
    // neighbour must be touched along exactly one stretch of
    // boundary, or the merged region has a hole and its contour is
    // not a simple loop.
    auto can_merge = [&](const RegionInfo &a, const RegionInfo &b) {
        int n_ab = 0;
        for (uint16_t c : a.connections)
            if (c == b.id) ++n_ab;
        if (n_ab != 1) return false;
        if (std::find(a.floors.begin(), a.floors.end(), b.id) != a.floors.end())
            return false;
        if (std::find(b.floors.begin(), b.floors.end(), a.id) != b.floors.end())
            return false;
        return true;
    };

    bool merged = true;
    while (merged) {
        merged = false;
        for (int r = 1; r < region_count_; ++r) {
            RegionInfo &info = regions[size_t(r)];
            if (info.id == kNoRegion || info.cells == 0) continue;
            if (info.cells >= min_cells) continue;

            // Into the smallest viable neighbour, so that merging
            // evens regions out instead of growing one giant.
            int best = -1;
            int best_cells = 0;
            for (uint16_t c : info.connections) {
                if (c == kNoRegion || c == info.id) continue;
                RegionInfo &other = regions[c];
                if (other.id == kNoRegion || other.cells == 0) continue;
                if (best >= 0 && other.cells >= best_cells) continue;
                if (!can_merge(info, other) || !can_merge(other, info)) continue;
                best = int(c);
                best_cells = other.cells;
            }
            if (best < 0) continue;

            RegionInfo &into = regions[size_t(best)];
            // Splice `info`'s boundary into `into`'s at the point
            // they touch, so the merged list stays in boundary order
            // and stays usable for the next merge.
            std::vector<uint16_t> joined;
            auto seam_a = std::find(into.connections.begin(),
                                    into.connections.end(), info.id);
            auto seam_b = std::find(info.connections.begin(),
                                    info.connections.end(), into.id);
            if (seam_a != into.connections.end() &&
                seam_b != info.connections.end()) {
                size_t ia = size_t(seam_a - into.connections.begin());
                size_t ib = size_t(seam_b - info.connections.begin());
                size_t na = into.connections.size(), nb = info.connections.size();
                for (size_t k = 1; k < na; ++k)
                    joined.push_back(into.connections[(ia + k) % na]);
                for (size_t k = 1; k < nb; ++k)
                    joined.push_back(info.connections[(ib + k) % nb]);
                into.connections.swap(joined);
            }
            for (uint16_t fl : info.floors) add_unique(into.floors, fl);
            into.cells += info.cells;
            info.cells = 0;
            info.id = uint16_t(best);
            info.remap = true;
            merged = true;
        }
    }

    // Follow merge chains to their end, then hand out dense ids so
    // that the polygon stage can index by region without a map.
    for (int r = 1; r < region_count_; ++r) {
        RegionInfo &info = regions[size_t(r)];
        if (!info.remap) continue;
        uint16_t target = info.id;
        for (int guard = 0; guard < region_count_; ++guard) {
            if (target == kNoRegion || !regions[target].remap) break;
            target = regions[target].id;
        }
        info.id = target;
    }
    std::vector<uint16_t> dense(size_t(region_count_), kNoRegion);
    uint16_t next = 1;
    for (int r = 1; r < region_count_; ++r) {
        RegionInfo &info = regions[size_t(r)];
        if (info.remap || info.id == kNoRegion || info.cells == 0) continue;
        dense[size_t(r)] = next++;
    }
    for (int r = 1; r < region_count_; ++r) {
        RegionInfo &info = regions[size_t(r)];
        if (info.remap && info.id != kNoRegion) dense[size_t(r)] = dense[info.id];
    }
    for (size_t i = 0; i < n; ++i) {
        uint16_t r = reg[i];
        spans_[i].region = r == kNoRegion ? kNoRegion : dense[r];
    }
    region_count_ = int(next);
}

int CompactField::build_regions(float min_region_area) {
    const size_t n = spans_.size();
    region_count_ = 0;
    if (n == 0) return 0;
    if (max_dist_ == 0) build_distance_field();

    std::vector<uint16_t> src_region(n, kNoRegion);
    std::vector<uint16_t> src_dist(n, 0);
    std::vector<int32_t> stack;
    stack.reserve(n / 4 + 16);

    uint16_t next = 1;
    // Start at the peaks and let the level fall two half-cells at a
    // time -- one whole cell, the finest step the chamfer metric can
    // actually distinguish.
    int level = (int(max_dist_) + 1) & ~1;
    while (level > 0) {
        level = level >= 2 ? level - 2 : 0;

        expand_regions(level, src_region.data(), src_dist.data(), stack);

        // Whatever the existing regions did not reach is a new peak.
        for (int z = 0; z < depth_; ++z) {
            for (int x = 0; x < width_; ++x) {
                const Cell &c = cell(x, z);
                for (uint32_t i = c.start; i < c.start + c.count; ++i) {
                    if (spans_[i].dist < level) continue;
                    if (src_region[i] != kNoRegion) continue;
                    if (next == 0xffff) continue;  // out of ids; stop seeding
                    if (flood_region(int32_t(i), level, next, src_region.data(),
                                     src_dist.data(), stack))
                        ++next;
                }
            }
        }
    }
    // One last growth at level zero, so that the cells right up
    // against the walls belong to something.
    expand_regions(0, src_region.data(), src_dist.data(), stack);

    for (size_t i = 0; i < n; ++i) spans_[i].region = src_region[i];
    region_count_ = int(next);

    const float cell_area = cell_size_ * cell_size_;
    int min_cells = int(std::ceil(min_region_area / cell_area));
    filter_small_regions(std::max(min_cells, 1));
    return region_count_;
}

}  // namespace wr::nav
