// Warren -- the world as columns of solid, which is where a navmesh
// starts.
//
// WHY VOXELISE AT ALL.
//
// The obvious way to find the walkable floor is to take every
// triangle that faces up and call it walkable. It does not work, and
// every reason is the same reason: a triangle knows nothing about
// what is above or below it. The floor under a table is a walkable
// triangle and unwalkable space. A staircase is thirty triangles that
// overlap in plan. Two buildings' floors at the same height are one
// triangle soup and two rooms. A kerb is a surface a body can get
// onto from one side and not the other, and the triangle is the same
// triangle from both.
//
// Rasterising into columns answers all of those with one structure.
// A column holds the solid SPANS at that x, z -- floor 0.0 to 0.2,
// table top 0.7 to 0.8 -- and a body stands on the top of a span with
// enough clear air above it. Overlap, clearance and separation all
// become arithmetic on intervals, and arithmetic on intervals is
// something that can be got right.
//
// This is the shape Recast uses, for these reasons. Two structures,
// because they want different things:
//
//   Heightfield   sparse, a sorted span list per column. Built by
//                 rasterising triangles. Knows about solid.
//   CompactField  dense, one entry per WALKABLE SURFACE, with its
//                 four neighbours resolved once. Knows about walking.
//
// Everything downstream -- erosion, the distance field, regions,
// contours -- is a graph walk, and wants the neighbour links already
// there rather than a binary search into a column each step.
#pragma once

#include <cstdint>
#include <vector>

#include "core/math/transform.h"

namespace wr::nav {

// HOW A BODY IS MEASURED. Everything the baker does is in these
// terms, and a navmesh is only correct for the body it was baked
// for -- a hound and a survivor want two bakes, not one.
struct AgentSpec {
    float radius = 0.4f;
    float height = 1.8f;
    // The tallest thing it can step onto without jumping.
    float max_climb = 0.45f;
    // The steepest ground it can stand on.
    float max_slope_degrees = 48.0f;
};

// ONE SOLID INTERVAL in a column, in cell_height units above the
// field's floor. `top` is exclusive, so two spans touching is
// `a.top == b.bottom` and not an off-by-one argument.
struct Span {
    uint16_t bottom = 0;
    uint16_t top = 0;
    // Set at rasterisation from the triangle normal: this surface is
    // not too steep to stand on. Says nothing yet about clearance or
    // about being able to get here, which need the neighbours.
    bool slope_ok = false;
};

class Heightfield {
public:
    // `triangles` is a flat list, three vertices per triangle, in
    // world space. Anything outside `bounds` is clipped away.
    bool build(const std::vector<Vec3> &triangles, const AABB &bounds,
               float cell_size, float cell_height, float max_slope_degrees);

    int width() const { return width_; }
    int depth() const { return depth_; }
    int height_cells() const { return height_; }
    const AABB &bounds() const { return bounds_; }
    float cell_size() const { return cell_size_; }
    float cell_height() const { return cell_height_; }

    const std::vector<Span> &column(int x, int z) const {
        return columns_[size_t(z) * size_t(width_) + size_t(x)];
    }
    bool inside(int x, int z) const {
        return x >= 0 && z >= 0 && x < width_ && z < depth_;
    }
    size_t span_count() const;

private:
    // Inserts a span into a column, merging with anything it touches
    // or overlaps. The merged span keeps the higher top, and takes
    // its slope flag from whichever input owns that top -- the
    // surface you stand on is the top one.
    void add_span(int x, int z, uint16_t bottom, uint16_t top, bool slope_ok);

    int width_ = 0, depth_ = 0, height_ = 0;
    float cell_size_ = 0.25f, cell_height_ = 0.15f;
    AABB bounds_;
    std::vector<std::vector<Span>> columns_;
};

// ---------------------------------------------------------------- compact

// Four neighbours, in this order everywhere in the navigation code.
// Kept as a constant rather than written out, because a contour walk
// that disagrees with a flood fill about which way is 1 produces a
// navmesh that is subtly inside out and looks nearly right.
inline constexpr int kDirX[4] = {-1, 0, 1, 0};
inline constexpr int kDirZ[4] = {0, 1, 0, -1};

inline constexpr uint16_t kNoRegion = 0;
inline constexpr int32_t kNoNeighbour = -1;

// ONE WALKABLE SURFACE. `y` is the floor -- the top of the solid
// below -- and `clearance` the clear air above it, both in
// cell_height units.
struct CompactSpan {
    uint16_t y = 0;
    uint16_t clearance = 0;
    uint16_t dist = 0;    // to the nearest border, in chamfer units
    uint16_t region = kNoRegion;
    int32_t neighbour[4] = {kNoNeighbour, kNoNeighbour, kNoNeighbour,
                            kNoNeighbour};
};

class CompactField {
public:
    // Keeps every span that is not too steep and has the headroom,
    // and links each to the neighbours it can step to without
    // climbing more than `max_climb`. Drops ledges -- see the .cpp,
    // where the rule is more interesting than it sounds.
    bool build(const Heightfield &hf, const AgentSpec &agent);

    // Distance from each span to the nearest border, where a border
    // is a span missing one of its four neighbours. Chamfer 2-3
    // metric, so `dist` is in half-cells and a straight run of n
    // cells is 2n. Also the input to the watershed.
    void build_distance_field();

    // Remove everything within `radius` of a wall or a drop. A body
    // has width, and a polygon that reaches the edge is a polygon
    // whose centre can be told to stand with half of it in the air.
    // Needs the distance field; builds it if it is not there.
    void erode(float radius);

    // Watershed partition into regions, each of which becomes one
    // contour and then some polygons. Returns the region count,
    // counting region 0 (nothing) -- so ids run 1..count-1.
    int build_regions(float min_region_area);

    int width() const { return width_; }
    int depth() const { return depth_; }
    const AABB &bounds() const { return bounds_; }
    float cell_size() const { return cell_size_; }
    float cell_height() const { return cell_height_; }
    int region_count() const { return region_count_; }
    uint16_t max_distance() const { return max_dist_; }

    // Spans of a cell live contiguously: [start, start + count).
    struct Cell {
        uint32_t start = 0;
        uint16_t count = 0;
    };
    const Cell &cell(int x, int z) const {
        return cells_[size_t(z) * size_t(width_) + size_t(x)];
    }
    const std::vector<CompactSpan> &spans() const { return spans_; }
    std::vector<CompactSpan> &spans() { return spans_; }
    bool inside(int x, int z) const {
        return x >= 0 && z >= 0 && x < width_ && z < depth_;
    }

    // Where a neighbour link lands, as a cell. The span index is in
    // `neighbour[dir]`; this is for when the walk needs the x, z too.
    static void step(int x, int z, int dir, int *nx, int *nz) {
        *nx = x + kDirX[dir];
        *nz = z + kDirZ[dir];
    }

    Vec3 span_position(int x, int z, const CompactSpan &s) const {
        return Vec3(bounds_.min.x + (float(x) + 0.5f) * cell_size_,
                    bounds_.min.y + float(s.y) * cell_height_,
                    bounds_.min.z + (float(z) + 0.5f) * cell_size_);
    }

private:
    // Resolve the four neighbours of every span. Called on build
    // and again after anything removes spans, because an index into
    // a compacted array is only meaningful for the array it was
    // taken from.
    void link_neighbours();
    // Drop every span whose `keep` entry is false and re-index.
    void rebuild(const std::vector<bool> &keep);

    void expand_regions(int level, uint16_t *src_region, uint16_t *src_dist,
                        std::vector<int32_t> &stack);
    bool flood_region(int32_t start, int level, uint16_t region,
                      uint16_t *src_region, uint16_t *src_dist,
                      std::vector<int32_t> &stack);
    void filter_small_regions(int min_cells);

    int width_ = 0, depth_ = 0;
    float cell_size_ = 0.25f, cell_height_ = 0.15f;
    AABB bounds_;
    std::vector<Cell> cells_;
    std::vector<CompactSpan> spans_;
    int region_count_ = 0;
    uint16_t max_dist_ = 0;
    // The agent, in cells, remembered from build() so that a relink
    // after erosion applies the same rule as the first one did.
    int height_cells_ = 1;
    int climb_cells_ = 0;
};

}  // namespace wr::nav
