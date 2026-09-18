// Warren -- region boundaries, as loops of points.
//
// A region is a set of cells. What the polygon builder needs is the
// outline of that set: a closed loop of points, few enough to be
// worth triangulating, and agreeing exactly with the outlines of the
// regions next door -- because two polygons that are supposed to
// share an edge must share it to the last bit, or the navmesh has a
// crack in it and a path cannot cross.
//
// That last requirement is why the simplifier cannot just be a
// line-fitter. Where a boundary changes from bordering region A to
// bordering region B, the point of change is MANDATORY in both
// outlines, and both simplify the stretch between two such points
// independently but from identical input. Same input, same
// algorithm, same output, shared edge.
#pragma once

#include <cstdint>
#include <vector>

#include "nav/heightfield.h"

namespace wr::nav {

// A point on a boundary, in cell coordinates -- integers, because
// two contours agreeing to the last bit is the whole point, and
// floats that agree to the last bit are a thing one hopes for.
struct ContourVert {
    int32_t x = 0, y = 0, z = 0;
    // The region on the other side of the edge leaving this vertex,
    // or kNoRegion where the edge faces the void. This is what marks
    // the mandatory vertices, and what the polygon builder later
    // uses to find which polygon is across an edge.
    uint16_t region = kNoRegion;
};

struct Contour {
    std::vector<ContourVert> verts;  // simplified
    std::vector<ContourVert> raw;    // one point per cell edge
    uint16_t region = kNoRegion;
};

struct ContourSet {
    std::vector<Contour> contours;
    AABB bounds;
    float cell_size = 0.25f;
    float cell_height = 0.15f;
    int width = 0, depth = 0;

    // How many regions produced a hole that had to be bridged into
    // its outline. Not an error -- a room with a pillar in it is a
    // ring, and a ring has a hole -- but worth reporting, because a
    // bake full of them usually means the region size is tuned wrong.
    int merged_holes = 0;
};

// `max_error` is how far, in cells, a simplified edge may stray from
// the rasterised boundary. `max_edge_cells` splits any edge longer
// than that, which keeps polygons from becoming long slabs whose
// interiors are far from any vertex; zero disables it.
bool build_contours(const CompactField &field, float max_error,
                    int max_edge_cells, ContourSet *out);

}  // namespace wr::nav
