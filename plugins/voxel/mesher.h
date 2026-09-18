// Warren voxel -- turning a field into triangles.
//
// DUAL CONTOURING, not marching cubes.
//
// Marching cubes puts its vertices on the grid edges, which means a
// sharp edge in the field -- the lip of a quarry, the corner of a
// dug-out room -- comes out rounded off to the cell size, and no
// amount of resolution fixes it because the vertices are constrained
// to the edges. Dual contouring puts ONE vertex inside each cell and
// solves for where it should go from the surface normals at the
// crossings, so a corner is reproduced exactly when the field has
// one and smoothly when it does not.
//
// The cost is the solve -- a 3x3 least squares per surface cell --
// and the need for the normal at every crossing, which is why the
// density source is asked for its gradient.
#pragma once

#include <vector>

#include "density.h"
#include "render/mesh.h"

namespace wr::voxel {

struct MeshRequest {
    const DensitySource *density = nullptr;
    // World position of the chunk's corner (0, 0, 0).
    Vec3 origin;
    // Metres per cell. Doubling it halves the detail, which is how
    // level of detail works here: the same field, sampled coarsely.
    float cell_size = 1.0f;
    // Cells along each axis.
    int resolution = 32;
    // How hard to pull the solved vertex back towards the middle of
    // its cell. Zero gives the sharpest corners and, where the field
    // is noisy, vertices that shoot off outside their cell; a little
    // is much more robust and the difference is not visible.
    float qef_regularisation = 0.02f;
    // Blend normals across a cell rather than taking the field's, so
    // a coarse chunk does not look faceted.
    bool smooth_normals = true;
};

struct MeshResult {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    AABB bounds;
    // Set when the chunk is entirely solid or entirely empty. The
    // common case by a wide margin, and the reason a world can be
    // large.
    bool empty = true;
    // How much of the work was actually done, for the statistics.
    uint32_t surface_cells = 0;
    uint32_t samples = 0;
};

// Thread-safe: it touches nothing but its arguments, so a hundred
// chunks can be meshed at once.
void mesh_chunk(const MeshRequest &request, MeshResult *out);

// The colour a material is drawn in until there is a texture array.
// Exposed so a game can replace the palette.
struct Palette {
    Color colours[256];
    float roughness[256];
    Palette();
    static Palette &instance();
};

}  // namespace wr::voxel
