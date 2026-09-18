// Warren -- many bodies moving at once without walking through each
// other.
//
// A path is not movement. Give thirty shamblers the same target and
// thirty correct paths, and they converge on it as one shambler
// thirty deep: each is following its own shortest route and none of
// them knows the others exist. Nothing about the navmesh can fix
// this, because the navmesh describes the ground, and the problem is
// the traffic.
//
// WHY RECIPROCAL AVOIDANCE, and not steering away from neighbours.
// The obvious fix -- push apart from anything too close -- oscillates
// badly. Two bodies approaching head on both dodge, see the dodge,
// dodge back, and shuffle down the corridor in a slow dance. The
// cause is that each treats the other as an obstacle that will hold
// still, and then it does not.
//
// ORCA fixes it by having each body assume the other is solving the
// same problem and will take HALF the correction. Each neighbour
// then rules out a half-plane of velocities, the body picks the
// allowed velocity nearest the one it wanted, and the two dodges
// compose into one clean pass with no negotiation and no message
// passing. Thirty bodies is thirty little linear programs of a
// handful of constraints each, which is nothing.
//
// The half-planes are exact -- pick a velocity inside them all and
// no collision is possible for the next `time_horizon` seconds. When
// there is no such velocity, which happens in a crush, the fallback
// picks the velocity that violates the worst constraint least, so a
// packed crowd squeezes rather than jams.
#pragma once

#include <cstdint>
#include <vector>

#include "nav/navmesh.h"

namespace wr::nav {

struct CrowdAgent {
    Vec3 position;
    Vec3 velocity;
    // What the body would do if it were alone. Path following sets
    // this; avoidance then finds the nearest thing to it that is
    // actually safe.
    Vec3 desired;

    float radius = 0.4f;
    float height = 1.8f;
    float max_speed = 3.0f;
    // How hard it may change velocity. Without this, avoidance
    // produces instant reversals that look like a glitch rather
    // than like something heavy changing its mind.
    float max_accel = 14.0f;
    // How far ahead to look. Longer is smoother and more timid;
    // shorter cuts it finer and starts to bump.
    float time_horizon = 2.0f;

    // WHICH GROUND THIS BODY MAY USE. The navmesh is baked once
    // with everything passable; what a given body is allowed to
    // cross is a filter, so a town can open a zone at a time, a
    // hound can be barred from the sewer, and none of it costs a
    // re-bake.
    NavFilter filter;

    bool avoidance = true;
    // Who avoids whom. A body avoids another when its mask overlaps
    // the other's layer -- so the living can be made to dodge each
    // other and the dead to walk through the living, which is a
    // request games make more often than one would think.
    uint16_t layer = 1, mask = 0xffff;

    // ------------------------------------------------ path state
    std::vector<PathPoint> path;
    size_t leg = 1;  // the point being walked toward
    Vec3 target;
    // HOW NEAR IS ARRIVED. Zero means the target point itself,
    // within the crowd's `arrive_radius`.
    //
    // It is worth setting for anything chasing something. Thirty
    // bodies sent to one point cannot all stand on it -- they have
    // size -- so they orbit it for ever, each correctly walking
    // toward a spot the others are in. That is not a bug in the
    // avoidance; it is an impossible instruction. "Get within two
    // metres of the player" is a possible one, and is what was
    // meant.
    float goal_radius = 0.0f;
    bool has_target = false;
    bool path_partial = false;
    // True while crossing an off-mesh link. The game reads this to
    // play a climb or a vault; the crowd just moves the body along
    // the link at its own speed.
    bool on_link = false;
    uint16_t link = 0xffff;

    // Set by the crowd, read by whoever cares.
    bool arrived = false;
    // How long this body has been claiming to go somewhere while
    // barely moving. The crowd re-plans off it; a game can read it
    // to decide something has gone wrong enough to act on.
    float stuck_for = 0.0f;
    // How much avoidance had to bend the desired velocity, in metres
    // per second. Large and sustained means the body is stuck in a
    // crowd, which is the cue for a game to do something else.
    float deflection = 0.0f;

    uint32_t id = 0;
};

class Crowd {
public:
    void set_navmesh(const NavMesh *mesh) { mesh_ = mesh; }
    const NavMesh *navmesh() const { return mesh_; }

    // Returns the agent's id, which is stable across removals --
    // unlike its index, which is not.
    uint32_t add(const CrowdAgent &agent);
    void remove(uint32_t id);
    CrowdAgent *find(uint32_t id);
    const CrowdAgent *find(uint32_t id) const;
    const std::vector<CrowdAgent> &agents() const { return agents_; }
    std::vector<CrowdAgent> &agents() { return agents_; }
    void clear();

    // Ask for a path. Repeats to the same place are ignored, so this
    // is safe to call every frame from a game that does not want to
    // track whether the target moved.
    bool set_target(uint32_t id, const Vec3 &target);
    void stop(uint32_t id);

    // How close counts as arrived, and how close counts as having
    // reached an intermediate corner.
    float arrive_radius = 0.35f;
    float corner_radius = 0.45f;
    // A body shoved this far off its path gives up and asks for a
    // new one. Zero disables re-planning.
    float replan_distance = 1.5f;
    // Paths recomputed per step, at most. Thirty bodies all
    // re-planning on the same frame is a spike; spreading it over a
    // few frames is invisible and flat.
    int replans_per_step = 4;

    void step(float dt);

    // Diagnostics for the frame just stepped.
    struct Stats {
        int agents = 0;
        int avoiding = 0;
        int replanned = 0;
        int infeasible = 0;  // crowded enough that no safe velocity existed
    };
    const Stats &stats() const { return stats_; }

private:
    void follow_paths(float dt);
    void avoid(float dt);
    void integrate(float dt);
    void rebuild_grid();
    void neighbours_of(size_t index, float range, std::vector<size_t> *out) const;

    const NavMesh *mesh_ = nullptr;
    std::vector<CrowdAgent> agents_;
    uint32_t next_id_ = 1;
    Stats stats_;

    // A uniform grid over the xz plane for neighbour lookups,
    // rebuilt each step. Agents move every frame, so there is
    // nothing to be gained by keeping it between steps.
    float grid_cell_ = 2.0f;
    std::vector<int32_t> grid_head_;
    std::vector<int32_t> grid_next_;
    Vec3 grid_min_;
    int grid_w_ = 0, grid_d_ = 0;
    // Round-robin cursor, so replanning is spread rather than
    // always falling on the same few agents.
    size_t replan_cursor_ = 0;
};

// The ORCA solve on its own, in the xz plane. Exposed because it is
// the part with the interesting arithmetic and it is worth being
// able to test a head-on pass without a scene, a navmesh or a clock.
struct OrcaLine {
    Vec2 point;      // a point on the boundary of the half-plane
    Vec2 direction;  // unit; the allowed side is to the LEFT
};

// Fills `lines` with one constraint per neighbour.
void orca_constraints(const Vec2 &position, const Vec2 &velocity, float radius,
                      const std::vector<Vec2> &n_pos,
                      const std::vector<Vec2> &n_vel,
                      const std::vector<float> &n_radius, float time_horizon,
                      float dt, std::vector<OrcaLine> *lines);

// The velocity nearest `preferred` that satisfies every line and is
// no longer than `max_speed`. Returns false when no such velocity
// exists -- the result is then the least-bad one, which is what a
// body in a crush should use.
bool orca_solve(const std::vector<OrcaLine> &lines, float max_speed,
                const Vec2 &preferred, Vec2 *out);

}  // namespace wr::nav
