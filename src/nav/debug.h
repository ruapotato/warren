// Warren -- seeing the navmesh.
//
// A navmesh is invisible and everything about it is geometric, which
// is a bad combination. "The zombies will not go upstairs" has a
// dozen causes -- the stairs were filtered as a ledge, the climb
// quantised too small, the region got dropped as an island, the
// contours never joined -- and they all look the same from outside.
// They look entirely different the moment the polygons are on screen
// with their seams drawn in.
//
// This is a separate file because it is the one part of navigation
// that knows what a Mesh is. Everything else works on triangles and
// returns polygons, which is what lets the bake be tested without a
// renderer.
#pragma once

#include "nav/navmesh.h"
#include "render/mesh.h"

namespace wr::nav {

enum class DebugColour : uint8_t {
    // A colour per region, so the seams the watershed chose are
    // visible. This is the one to look at when polygons will not
    // join: a crack shows as two regions meeting with a gap.
    Region,
    // Alternating per polygon, which shows the merge step's work.
    Polygon,
    // Distance from the nearest wall, dark at the edges. Shows what
    // the erosion did and where a passage is too tight.
    Flat,
};

// The walkable surface as filled triangles, lifted `lift` metres so
// it sits above the floor it was baked from rather than z-fighting
// with it.
Ref<Mesh> debug_surface(const NavMesh &mesh, float lift = 0.05f,
                        DebugColour colour = DebugColour::Region);

// The edges, as thin flat quads: walls in one colour, the seams
// between polygons in another. Drawn on top of the surface, this is
// what makes a crack obvious -- a wall drawn down the middle of what
// looks like open floor is a wall down the middle of open floor.
Ref<Mesh> debug_edges(const NavMesh &mesh, float lift = 0.06f,
                      float width = 0.04f);

// The off-mesh links, as a bar from each end and a line between
// them. A link whose ends did not land on the mesh is drawn in a
// different colour, because that is the failure worth seeing: it is
// silent otherwise, and the level just has a ladder nothing uses.
Ref<Mesh> debug_links(const NavMesh &mesh, float width = 0.06f);

}  // namespace wr::nav
