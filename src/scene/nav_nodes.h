// Warren -- navigation, as nodes in a scene.
//
// Everything underneath this file works on triangles and returns
// polygons, and knows nothing about the tree. That separation is on
// purpose -- a bake is testable without a scene, a window or a
// clock, and the tests in test_nav and test_crowd take advantage of
// it. What is missing is the ordinary way to use it: put a region in
// the level, put agents in the region, and let the level's geometry
// be what gets baked.
//
//   NavRegion3D   owns one NavMesh and one Crowd. Bakes from the
//                 meshes under it. Steps the crowd.
//   NavLink3D     an authored way between two points that is not
//                 walking: a ladder, a drop, a vault.
//   NavAgent3D    a body in the region's crowd. Reads a target,
//                 writes a position.
//
// ONE MESH PER REGION, AND LINKS BETWEEN LEVELS. Not one mesh per
// building. A town's street and its roofs bake into a single navmesh
// with several disconnected pieces, and the links are what join
// them -- which is exactly the right shape, because a ladder is a
// link whether or not the two ends happen to be in one mesh. Tiled
// bakes, for streaming a world too big to do at once, are a
// different feature and not this one.
#pragma once

#include <string>
#include <vector>

#include "nav/crowd.h"
#include "render/mesh.h"
#include "nav/navmesh.h"
#include "scene/node.h"

namespace wr {

class NavAgent3D;
class NavLink3D;

class NavRegion3D : public Node3D {
    WR_CLASS(NavRegion3D, Node3D)

public:
    NavRegion3D();

    // The body the mesh is baked for. A navmesh is only correct for
    // one size of thing -- a hound and a survivor want two bakes,
    // not one -- so this is part of the region, not of the agent.
    nav::BakeSettings settings;

    // THE SAME SETTINGS, ONE FIELD AT A TIME, for a script.
    //
    // BakeSettings is a plain struct and has no business becoming a
    // reflected class -- it is a parameter block, not an object with
    // an identity. But a game has to be able to say how big its
    // bodies are, and every one of these is a number somebody tunes.
    float get_agent_radius() const { return settings.agent.radius; }
    void set_agent_radius(float v) { settings.agent.radius = v; }
    float get_agent_height() const { return settings.agent.height; }
    void set_agent_height(float v) { settings.agent.height = v; }
    float get_agent_climb() const { return settings.agent.max_climb; }
    void set_agent_climb(float v) { settings.agent.max_climb = v; }
    float get_agent_slope() const { return settings.agent.max_slope_degrees; }
    void set_agent_slope(float v) { settings.agent.max_slope_degrees = v; }
    float get_cell_size() const { return settings.cell_size; }
    void set_cell_size(float v) { settings.cell_size = v; }
    float get_cell_height() const { return settings.cell_height; }
    void set_cell_height(float v) { settings.cell_height = v; }
    float get_min_region_area() const { return settings.min_region_area; }
    void set_min_region_area(float v) { settings.min_region_area = v; }

    // Where the level starts, for pruning what cannot be reached
    // from it. See NavMesh::prune_unreachable -- and note that a
    // seed standing on something small keeps only that something.
    // PRUNE AFTER THE FACT, from points chosen once there is a mesh
    // to choose them on.
    //
    // Seeding the bake itself means naming the points before the
    // navmesh exists, and a point named blind lands wherever it
    // lands -- on a crate, on a statue, on the roof of the thing it
    // was meant to stand beside. What survives is then whatever is
    // reachable from the top of a crate. Baking first and pruning
    // second lets a caller ask where the ground actually is.
    int prune_from(const Array &points);

    void add_seed(const Vec3 &p) { settings.reachable_from.push_back(p); }
    void clear_seeds() { settings.reachable_from.clear(); }
    int seed_count() const { return int(settings.reachable_from.size()); }

    // Collect the geometry under `source` (or under this node, when
    // it is null) and bake. Everything visible with a mesh counts,
    // unless it is marked not to navigate. Returns false and leaves
    // the old mesh in place on failure, because a level with a stale
    // navmesh is a great deal more playable than one with none.
    bool bake(Node *source = nullptr);
    // Bake using an explicit bounding volume rather than the one
    // worked out from the geometry. Worth it when the level has one
    // stray object a mile away.
    bool bake_within(const AABB &bounds, Node *source = nullptr);

    const nav::NavMesh *mesh() const { return mesh_.get(); }
    nav::NavMesh *mesh() { return mesh_.get(); }
    void set_mesh(nav::NavMesh *m);
    nav::NavMesh *get_mesh() const { return mesh_.get(); }

    nav::Crowd &crowd() { return crowd_; }
    const nav::Crowd &crowd() const { return crowd_; }

    // What came of the last bake, for an editor to show and a
    // developer to worry about.
    const nav::BakeStats &stats() const { return stats_; }
    int poly_count() const { return mesh_ ? mesh_->poly_count() : 0; }
    float bake_seconds() const { return stats_.seconds; }

    // Gather every NavLink3D under this region and hand them to the
    // mesh. Called by bake; call it again after moving one.
    void collect_links(Node *source = nullptr);

    // ------------------------------------------------- queries
    //
    // In world space, which is the only space a caller has. The
    // defaults for `extents` are generous horizontally and tight
    // vertically, because the usual mistake is a body half a metre
    // above the floor and the usual disaster is snapping it to the
    // storey below.
    // WHAT THE BAKE ACTUALLY PRODUCED, as geometry to look at.
    //
    // A navmesh is invisible and everything about it is geometric,
    // which is a bad combination: "the bodies will not go upstairs"
    // has a dozen causes that look identical from outside and
    // entirely different with the polygons on screen. `mode` picks
    // the colouring -- 0 per region, 1 per polygon, 2 flat.
    Mesh *debug_surface(float lift, int mode) const;
    Mesh *debug_edges(float lift) const;
    Mesh *debug_links() const;

    // Turn area bits on or off over a box, which is how a zone
    // opens without re-baking. See NavMesh::set_area_in.
    int set_area_in(const AABB &box, int set_bits, int clear_bits);
    int area_at(const Vec3 &p) const;

    bool nearest_point(const Vec3 &p, Vec3 *out) const;
    bool random_point(const Vec3 &near, float radius, Vec3 *out) const;
    // The same two, shaped for a script: they return the point
    // rather than a flag, falling back to what was asked for when
    // there is no mesh. A caller that needs to tell the difference
    // uses the pointer forms above.
    Vec3 nearest_point_or(const Vec3 &p) const;
    Vec3 random_point_or(const Vec3 &near, float radius) const;
    bool bake_now() { return bake(nullptr); }
    void collect_links_now() { collect_links(nullptr); }
    Array find_path(const Vec3 &from, const Vec3 &to) const;
    bool is_reachable(const Vec3 &from, const Vec3 &to) const;

    // Whether to step the crowd. Off means agents hold still, which
    // is what a paused game wants.
    bool active = true;

    void on_ready() override;
    void on_physics(float dt) override;

private:
    friend class NavAgent3D;
    void gather(Node *n, std::vector<Vec3> *tris) const;

    // The debug meshes are kept alive here, because a script that
    // assigns one to a MeshInstance3D and drops its own reference
    // would otherwise be holding a freed mesh.
    mutable Ref<Mesh> debug_surface_, debug_edges_, debug_links_;

    Ref<nav::NavMesh> mesh_;
    nav::Crowd crowd_;
    nav::BakeStats stats_;
    bool want_bake_ = false;
};

class NavLink3D : public Node3D {
    WR_CLASS(NavLink3D, Node3D)

public:
    NavLink3D() = default;

    // Both ends in the link's own local space, so moving the node
    // moves the link and a prefabricated ladder can be dropped
    // anywhere. `start` is usually the origin and left alone.
    Vec3 start;
    Vec3 end = Vec3(0, 3, 0);
    float radius = 0.8f;
    // Extra cost of using it, beyond the distance. A ladder that
    // takes four seconds to climb should say so, or every route
    // will prefer it to walking round.
    float cost = 0.0f;
    bool bidirectional = true;
    // Passed through to the path, so a game can tell a ladder from a
    // drop and play the right thing. Also what a NavFilter tests.
    uint16_t area = 1;

    nav::NavLink resolve() const;

    // The two ends in world space, for gizmos and for anything that
    // wants to place a body at one.
    Vec3 world_start() const { return global_transform() * start; }
    Vec3 world_end() const { return global_transform() * end; }
};

class NavAgent3D : public Node3D {
    WR_CLASS(NavAgent3D, Node3D)

public:
    NavAgent3D() = default;
    ~NavAgent3D() override;

    float radius = 0.4f;
    float height = 1.8f;
    float max_speed = 3.0f;
    float max_accel = 14.0f;
    // How near counts as arrived. Set it for anything chasing
    // something -- see CrowdAgent::goal_radius, which explains why
    // a mob told to stand on one point orbits it for ever.
    float goal_radius = 0.0f;
    bool avoidance = true;
    // Who avoids whom, in the crowd.
    uint16_t layer = 1, mask = 0xffff;
    // WHICH GROUND THIS BODY MAY WALK ON, against NavPoly::area.
    // Separate from `layer`/`mask`, which are about bodies; this is
    // about the level. A town that opens a zone at a time sets a
    // bit on that zone's polygons and puts the bit in here.
    int nav_include = 0xffff;
    int nav_exclude = 0;

    // WHO MOVES THE NODE. By default the agent does: the crowd
    // works out where the body should be and the transform follows.
    // A game with its own character controller wants the opposite --
    // it reads `velocity` and moves itself, and tells the agent
    // where it ended up. Set this false for that.
    bool drives_transform = true;

    void set_target(const Vec3 &p);
    void stop();
    bool has_target() const;
    bool arrived() const;
    // True when the route ran out before reaching the target, which
    // usually means the goal is somewhere nothing can get to.
    bool path_partial() const;

    Vec3 velocity() const;
    // Where the crowd wants the body, whether or not the node is
    // being driven by it.
    Vec3 desired_position() const;
    // True while crossing a link, with the link's area -- the cue to
    // play a climb, a vault or a fall rather than a walk.
    bool on_link() const;
    uint16_t link_area() const;
    // How hard avoidance is bending the route, in metres per second.
    // Large and sustained means stuck in a crowd.
    float deflection() const;
    // How long this body has been claiming to be going somewhere
    // while barely moving. The crowd re-plans off it by itself; a
    // game reads it to decide something has gone wrong enough to do
    // something else -- give up, attack what is in the way, despawn.
    float stuck_time() const;

    NavRegion3D *region() const { return region_; }

    void on_ready() override;
    void on_exit_tree() override;
    void on_physics(float dt) override;

private:
    void join();
    void leave();
    const nav::CrowdAgent *me() const;
    nav::CrowdAgent *me();

    NavRegion3D *region_ = nullptr;
    uint32_t id_ = 0;
    Vec3 pending_target_;
    bool pending_ = false;
};

}  // namespace wr
