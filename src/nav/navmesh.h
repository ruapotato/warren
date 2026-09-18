// Warren -- the navigation mesh: convex polygons, who is next to
// whom, and the shortest way across.
//
// WHAT A NAVMESH IS FOR. Not "where can a body stand" -- the compact
// field already knows that, cell by cell. A navmesh exists because
// searching a grid of a quarter of a million cells for every one of
// thirty bodies, every time one of them changes its mind, is work
// nobody has the budget for. Cutting the same ground into a few
// hundred convex pieces makes the search a few hundred nodes wide,
// and convexity means that once inside a piece a body can walk
// straight to any point of it without checking anything.
//
// The path then comes out in two stages, and both are necessary. A*
// over the polygons gives the CORRIDOR -- which pieces to cross, in
// order. That corridor is not a route: walking polygon centres makes
// a body stagger from middle to middle like it is avoiding cracks in
// the pavement. The funnel pulls a string taut through the corridor,
// and what comes back is the shortest route that stays inside it,
// which is the shortest route there is.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/math/transform.h"
#include "nav/contour.h"
#include "nav/heightfield.h"
#include "resource/resource.h"

namespace wr::nav {

inline constexpr int kMaxVertsPerPoly = 6;
inline constexpr uint16_t kNoPoly = 0xffff;

struct NavPoly {
    uint16_t verts[kMaxVertsPerPoly];
    // The polygon across each edge, kNoPoly where the edge is a wall.
    // Edge i runs from verts[i] to verts[(i+1) % count].
    uint16_t neis[kMaxVertsPerPoly];
    uint8_t count = 0;
    uint16_t region = kNoRegion;
    // Free for the game: door-only, water, crawl-space. The filter
    // passed to a query decides what a given body may cross.
    uint16_t area = 1;
};

// AN OFF-MESH LINK: a way between two points that is not walking.
// A ladder, a drop from a roof, a vault over a rail, a jump across
// an alley. Without these a navmesh of a town is a set of islands --
// the street, each roof, each floor -- and nothing on a roof can
// plan a route to anything not on that roof.
//
// The link is deliberately not geometry. It says only "from here you
// can get to there, at this cost, if you are the sort of thing that
// can". What the body DOES between the two points -- climb, fall,
// jump -- is the game's business, and it reads `area` to decide.
struct NavLink {
    Vec3 from, to;
    float radius = 0.6f;    // how near an agent must be to use it
    float cost = 0.0f;      // extra cost; 0 means the straight distance
    uint16_t area = 1;
    bool bidirectional = true;
    std::string name;

    // Resolved at bake time.
    uint16_t from_poly = kNoPoly, to_poly = kNoPoly;
};

struct BakeSettings {
    AgentSpec agent;
    // Metres per cell in plan, and vertically. The horizontal one
    // decides how small a gap can be represented; the vertical one
    // decides how finely a step is measured, and wants to be a good
    // deal smaller than max_climb, or every slope becomes a cliff.
    float cell_size = 0.25f;
    float cell_height = 0.15f;
    // Regions smaller than this, in square metres, that touch
    // nothing else are dropped -- a patch on a lamp post is
    // somewhere a body can be sent and can never reach.
    float min_region_area = 1.5f;
    // How far a simplified boundary may stray from the rasterised
    // one, in cells, and the longest edge to leave unsplit.
    float contour_max_error = 1.3f;
    float max_edge_length = 12.0f;
    // If any are given, the bake keeps only the ground reachable
    // from them. See NavMesh::prune_unreachable.
    std::vector<Vec3> reachable_from;
};

struct BakeStats {
    int spans = 0;
    int regions = 0;
    int contours = 0;
    int polys = 0;
    int verts = 0;
    int merged_holes = 0;
    // Polygons removed for being unreachable from the bake's seeds.
    int pruned = 0;
    float seconds = 0.0f;
};

// Where a path leg came from, so the game can tell walking from the
// rest. A leg ENTERING a link carries kPathLink and the link's area;
// the body is expected to do whatever that area means, and arrive at
// the next point.
inline constexpr uint8_t kPathWalk = 0;
inline constexpr uint8_t kPathLink = 1;
inline constexpr uint8_t kPathEnd = 2;

struct PathPoint {
    Vec3 position;
    uint16_t poly = kNoPoly;
    uint16_t link = 0xffff;  // index into links, when flags has kPathLink
    uint8_t flags = kPathWalk;
};

// Which polygons a query may cross. The default admits everything.
struct NavFilter {
    uint16_t include = 0xffff;  // bitmask against NavPoly::area
    uint16_t exclude = 0;
    // Per-area multipliers, so a body can be made to prefer the
    // street and tolerate the sewer.
    float cost[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

    bool passes(uint16_t area) const {
        return (area & include) != 0 && (area & exclude) == 0;
    }
    float multiplier(uint16_t area) const {
        int i = 0;
        while (i < 15 && !(area & (1u << i))) ++i;
        return cost[i];
    }
};

class NavMesh : public Resource {
    WR_CLASS(NavMesh, Resource)

public:
    // Bake from raw triangles. `links` are carried through and
    // resolved against the polygons that come out.
    bool bake(const std::vector<Vec3> &triangles, const AABB &bounds,
              const BakeSettings &settings, BakeStats *stats = nullptr);
    // Bake from a field someone else has already prepared, for when
    // the caller wants to inspect or alter it in between.
    bool bake_from(const CompactField &field, const BakeSettings &settings,
                   BakeStats *stats = nullptr);

    // KEEP ONLY WHAT CAN BE REACHED FROM HERE, and delete the rest.
    // Returns how many polygons went.
    //
    // A bake rasterises surfaces, not solids, so the floor under a
    // sealed box is walkable ground with a roof over it -- correct,
    // and enclosed by walls nothing can pass. Every building in a
    // town has one, and each is somewhere a body can be told to go
    // and will never arrive; a wandering monster picking a random
    // point finds itself pathing into a wall for ever.
    //
    // The region filter only drops islands too SMALL to be worth
    // reaching, because size is all it can judge from. Reachability
    // needs somewhere to start from, which only the game knows: the
    // player's spawn, the mouth of the sewer, wherever the level
    // begins. Given that, this is exact -- it floods the polygon
    // graph, links included, and removes what the flood did not
    // touch.
    int prune_unreachable(const std::vector<Vec3> &seeds);

    // CHANGE WHAT A FILTER SEES, without re-baking.
    //
    // Every polygon whose centre lies in `box` has `set_bits` turned
    // on in its area mask and `clear_bits` turned off. Returns how
    // many changed.
    //
    // This is how a level opens up. A town whose doors are bought
    // one at a time could re-bake as each one opens, and a bake of a
    // hundred and thirty metres is a fraction of a second -- spent
    // at the exact moment a player has just committed to a doorway
    // with a crowd behind them. Baking once with every doorway
    // passable and gating it with a bit costs nothing and happens
    // between frames.
    int set_area_in(const AABB &box, uint16_t set_bits, uint16_t clear_bits);
    // The area mask of whatever polygon is at `p`, or zero if none.
    uint16_t area_at(const Vec3 &p, const Vec3 &extents) const;

    void add_link(const NavLink &link);
    void clear_links();
    const std::vector<NavLink> &links() const { return links_; }
    // Re-resolve every link against the polygons. Called by bake;
    // call it again after adding links to a mesh already baked.
    void resolve_links();
    // One link, against the current mesh. Quiet when there is no
    // mesh yet.
    void resolve_link(NavLink &link);

    // Counted accessors, for the places that only want the size --
    // reflection among them, since a vector of structs is not a
    // Variant and a count is.
    int poly_count() const { return int(polys_.size()); }
    int vert_count() const { return int(verts_.size()); }
    int link_count() const { return int(links_.size()); }

    const std::vector<Vec3> &verts() const { return verts_; }
    const std::vector<NavPoly> &polys() const { return polys_; }
    const AABB &bounds() const { return bounds_; }
    bool empty() const { return polys_.empty(); }

    // ---------------------------------------------------- queries

    // The polygon containing `p`, searching within `extents` of it.
    // kNoPoly if there is none -- which is the honest answer when a
    // body has been shoved inside a wall, and the caller should then
    // ask for the nearest point instead.
    uint16_t find_poly(const Vec3 &p, const Vec3 &extents,
                       const NavFilter &filter = {}) const;
    // The closest point on the navmesh to `p`. Always succeeds if
    // the mesh has any polygons at all.
    bool nearest_point(const Vec3 &p, const Vec3 &extents, Vec3 *out,
                       uint16_t *poly = nullptr,
                       const NavFilter &filter = {}) const;
    // The height of the navmesh at `p` within polygon `poly`.
    bool height_at(uint16_t poly, const Vec3 &p, float *y) const;

    // The route from `from` to `to`, string-pulled. Returns false
    // when neither end is on the mesh. When the two ends are on the
    // mesh but not connected, this returns TRUE with a path to the
    // reachable point nearest the goal, and sets `partial` -- a body
    // that walks as far as it can is better behaved than one that
    // stands still, and the caller can tell the difference.
    bool find_path(const Vec3 &from, const Vec3 &to,
                   std::vector<PathPoint> *out, bool *partial = nullptr,
                   const NavFilter &filter = {}) const;

    // Can a body walk from `from` to `to` in a straight line without
    // leaving the mesh? Used to shorten a path as an agent moves,
    // and to decide whether a path is needed at all.
    // On a hit, `normal` is the wall's outward direction in the xz
    // plane -- which is what a caller needs in order to slide along
    // it rather than stop dead against it.
    bool raycast(const Vec3 &from, const Vec3 &to, Vec3 *hit,
                 const NavFilter &filter = {}, Vec3 *normal = nullptr) const;

    // ------------------------------------------------ persistence
    std::vector<uint8_t> save() const;
    bool load(const std::vector<uint8_t> &data);

private:
    void build_grid();
    // Polygons whose bounds overlap the cell at `p`.
    void query_cells(const Vec3 &mn, const Vec3 &mx,
                     std::vector<uint16_t> *out) const;
    Vec3 poly_center(const NavPoly &p) const;
    // The two vertices shared with the neighbour across edge `e`.
    void portal(uint16_t a, int e, Vec3 *left, Vec3 *right) const;

    std::vector<Vec3> verts_;
    std::vector<NavPoly> polys_;
    std::vector<NavLink> links_;
    AABB bounds_;

    // A flat grid over the xz plane, for point queries. Rebuilt with
    // the mesh; nothing here changes between bakes.
    int grid_w_ = 0, grid_d_ = 0;
    float grid_cell_ = 1.0f;
    std::vector<uint32_t> grid_start_;
    std::vector<uint16_t> grid_items_;
};

// Turn a contour set into convex polygons. Exposed because it is
// worth testing on its own: everything after it depends on the
// polygons being convex and on the adjacency being right, and both
// are cheap to check and expensive to debug later.
bool build_poly_mesh(const ContourSet &contours, std::vector<Vec3> *verts,
                     std::vector<NavPoly> *polys);

}  // namespace wr::nav
