// Warren -- liquids.
//
// POSITION BASED FLUIDS (Macklin & Muller 2013), which is the same
// family as the position-based solver next door and was chosen for
// the same reason: it is stable at a game's timestep. Smoothed
// particle hydrodynamics with a real pressure term is more
// physically direct and needs a step small enough that a game
// cannot afford it; PBF states incompressibility as a CONSTRAINT
// and solves it the way a constraint solver does, so it holds
// together at 1/60 and does not care if one frame is long.
//
// The loop, which is short:
//
//   predict  -- gravity, then x* = x + v dt
//   find     -- neighbours, from a spatial hash of the predictions
//   solve    -- a few iterations of "your neighbourhood is the
//               right density", each producing a position nudge
//   collide  -- push out of the world
//   finish   -- v = (x* - x) / dt, then vorticity and viscosity
//
// The two corrections at the end are not polish. Without vorticity
// confinement the solver's own damping eats every swirl and the
// liquid moves like wet sand; without XSPH viscosity neighbouring
// particles never agree on a velocity and the surface boils.
//
// FLUID GOES THROUGH PORTALS, and that is the reason for half the
// complexity here. Warping a particle when it crosses the plane is
// the easy part and on its own it looks terrible: a particle just
// short of the aperture has neighbours on its own side only, so its
// density reads low, the solver decides it is being stretched and
// pushes it away -- and the stream TEARS at the hole and sprays.
//
// So the neighbourhood has to see through. For every portal, the
// particles in front of its partner are warped into place behind it
// as GHOSTS: real contributors to density and pressure, owned by
// nobody, thrown away at the end of the step. A particle at the
// mouth of a portal then has a full neighbourhood, half of it on
// the other side of the map, and the stream crosses without
// noticing. See `build_ghosts`.
#pragma once

#include <cstdint>
#include <vector>

#include "core/math/transform.h"

namespace wr {

class PhysicsWorld;
class Portal3D;

// One kind of liquid. The numbers that make water look like water
// and a gel look like a gel are all here, so a game can have
// several at once without a solver per fluid.
struct FluidMaterial {
    // Particles per cubic metre, effectively. Everything else is
    // relative to this: the solver's whole job is to hold the local
    // density here.
    float rest_density = 1000.0f;
    // How far a particle sees. The single most important number --
    // too small and the fluid is a gas, too large and it is
    // treacle and costs the cube of the difference.
    float smoothing_radius = 0.12f;
    // Constraint softening. Zero is a solver that oscillates.
    float relaxation = 600.0f;
    // HOW MUCH IT STICKS TO ITSELF. XSPH: each particle is pulled
    // toward the average velocity of its neighbours. Low for water,
    // high for a gel that should move as a sheet.
    float viscosity = 0.02f;
    // How hard the solver puts swirl back in after its own damping
    // has taken it out.
    float vorticity = 0.15f;
    // THE TENSILE INSTABILITY FIX, and it is not optional.
    //
    // A particle with too few neighbours -- at a surface, which is
    // most of a splash -- reads as under-dense and the solver pulls
    // its neighbours in to help. They clump into strings and
    // beads, and a waterfall becomes a bead curtain. An artificial
    // repulsion, strongest at close range, cancels it. The numbers
    // are the paper's.
    float surface_pressure = 0.01f;
    float surface_power = 4.0f;
    // Bounce and grip against the world.
    float restitution = 0.0f;
    float friction = 0.15f;
    // Purely for the renderer.
    Color colour{0.25f, 0.55f, 0.95f, 1.0f};
};

class Fluid {
public:
    explicit Fluid(PhysicsWorld *world) : world_(world) {}

    // The radius used for collision and drawing. The smoothing
    // radius is about three times this; a particle is not a sphere
    // of liquid, it is a sample of one.
    float particle_radius = 0.04f;
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    // HOW DEEP A POOL THIS CAN HOLD UP, essentially.
    //
    // Position-based fluid propagates pressure one particle per
    // iteration: the bottom of a column learns it is being stood
    // on immediately, the layer above learns it next iteration,
    // and so on. So a pool ten particles deep needs about ten
    // iterations before the top of it knows, and with fewer the
    // lower layers stay compressed. Measured, on a column of
    // twenty: 4 iterations settles 12% over the rest density, 8
    // gives 6%, 16 gives 3%, 30 gives 1.4%.
    //
    // Six is the default because it is the knee for the pools a
    // game actually has -- a puddle, a stream, a tank you wade
    // through -- and because it is cheap. A game that wants a
    // deep reservoir to hold its level exactly should raise it
    // and pay for it. This is a property of the method and not a
    // bug to be tuned out; a different density solver is the only
    // real answer, and it costs more than the compression does.
    int solver_iterations = 6;
    // Above this a step is split, so one long frame does not move
    // every particle a metre and turn the fluid to spray.
    float max_step = 1.0f / 60.0f;
    // Particles further than this from everything are removed --
    // the ones that escaped through a seam. Zero disables it.
    float kill_below_y = -500.0f;
    // SEEING THROUGH THE HOLE. Off, the liquid still crosses --
    // a particle past the plane is still warped -- but its
    // neighbourhood stops at the aperture, so the stream reads as
    // under-dense there and is pushed away from the opening. It
    // tears. Kept as a switch because the difference is the whole
    // argument for the mechanism and has to be measurable.
    bool portal_ghosts = true;

    FluidMaterial material;

    // --- the fluid -------------------------------------------------
    uint32_t emit(const Vec3 &at, const Vec3 &velocity = Vec3());
    // A solid block of particles on a lattice, which is how every
    // fluid demo starts and how a game fills a tank.
    int fill(const AABB &box, float spacing = 0.0f);
    void clear();
    size_t count() const { return pos_.size(); }
    // The hard cap. A fluid that grows without bound is a frame
    // time that grows without bound.
    uint32_t max_particles = 40000;

    const std::vector<Vec3> &positions() const { return pos_; }
    const std::vector<Vec3> &velocities() const { return vel_; }

    void step(float dt);
    // What one particle weighs, for the rest density and the two
    // radii. See the comment on the implementation -- it is not
    // density times volume.
    float particle_mass() const;

    // --- asking about a point ----------------------------------------
    //
    // THE SURFACE COMES FROM HERE. A liquid drawn as particles is a
    // liquid that looks like particles; what makes it read as
    // liquid is an isosurface, and the engine already has a dual
    // contourer that will build one out of any field. So the fluid
    // answers "how dense is it here" and the mesher does the rest.
    //
    // Valid after a step, against the grid that step built.
    float density_at(const Vec3 &p) const;
    // The gradient of that, which is the surface normal. Worth
    // having analytically: the contourer's fallback takes six extra
    // samples per crossing and there are a great many crossings.
    Vec3 density_gradient_at(const Vec3 &p) const;
    // Everything the liquid occupies, grown by a smoothing radius.
    AABB bounds() const;
    // IS THERE ANY LIQUID IN THIS BOX. The surface mesher asks it
    // per chunk, because a body of liquid's bounding box is mostly
    // empty -- a stream across a room occupies a few per cent of
    // the volume it spans -- and contouring costs the cube of the
    // side. Answered from the grid the last step built, so it is a
    // handful of cell lookups and not a scan.
    bool occupied(const AABB &box) const;

    // --- what happened ---------------------------------------------
    struct Stats {
        uint32_t particles = 0;
        uint32_t ghosts = 0;
        uint32_t neighbours = 0;
        uint32_t portal_crossings = 0;
        uint32_t killed = 0;
        // HOW FAR OVER the rest density the worst particle is, as
        // a fraction. Over, not off: a particle at a free surface
        // has half a neighbourhood and reads under-dense by
        // definition -- a lone droplet is 50% "wrong" and there is
        // nothing to fix. Measuring |error| therefore reports the
        // shape of the surface and says nothing about the solver.
        // Compression is the thing a liquid must not do.
        float worst_compression = 0.0f;
        // The mean density of particles that HAVE a full
        // neighbourhood, over the rest density. The honest
        // incompressibility number: 1.0 is a liquid holding its
        // volume, 1.3 is a liquid being crushed, and it does not
        // move when the surface does.
        float interior_density = 0.0f;
        uint32_t interior_count = 0;
    };
    Stats stats;

private:
    void predict(float dt);
    void build_grid();
    void build_ghosts();
    void find_neighbours();
    void solve_density();
    void collide_world();
    void finish(float dt);

    // The spatial hash. One cell per smoothing radius, so a
    // neighbourhood is the 27 cells around a particle.
    int64_t cell_of(const Vec3 &p) const;

    PhysicsWorld *world_ = nullptr;

    // Real particles.
    std::vector<Vec3> pos_, vel_, predicted_, delta_;
    // Where each particle was when the step began -- the near end
    // of the ray that does its collision -- and the surface normal
    // it ended up against, if any.
    std::vector<Vec3> prev_, contact_;
    // How far the walls moved it this step. Subtracted before the
    // velocity is derived: a depenetration is not motion.
    std::vector<Vec3> pushed_;
    std::vector<float> lambda_, denom_, density_;
    std::vector<Vec3> vorticity_;

    // Ghosts: warped copies of particles on the far side of a
    // portal. Indices from `kGhostBase` upward refer to these.
    std::vector<Vec3> ghost_pos_;
    std::vector<float> ghost_lambda_;
    std::vector<uint32_t> ghost_source_;

    // Hash -> the particles in that cell. Rebuilt every step;
    // keeping the storage avoids the allocation.
    std::vector<int64_t> keys_;
    std::vector<uint32_t> order_;
    std::vector<uint32_t> cell_start_, cell_count_;
    std::vector<int64_t> cell_key_;
    // AN OPEN-ADDRESSED INDEX OVER cell_key_, because a binary
    // search is the wrong shape for this.
    //
    // Every density sample looks up twenty-seven cells and every
    // particle does the same once a step. A binary search over a
    // few thousand cells is a dozen dependent, cache-missing loads
    // EACH -- so the lookup, not the kernel, was most of the cost
    // of both the solver's neighbour pass and the surface mesher.
    // One probe into a power-of-two table replaces it.
    std::vector<int64_t> probe_key_;
    std::vector<uint32_t> probe_slot_;
    uint32_t probe_mask_ = 0;
    void build_index();
    // The index into cell_key_ for this cell, or 0xFFFFFFFF.
    uint32_t find_cell(int64_t key) const;

    // Flattened neighbour lists: `nbr_` indexed by `nbr_start_`.
    std::vector<uint32_t> nbr_;
    std::vector<uint32_t> nbr_start_, nbr_count_;

    std::vector<uint32_t> scratch_;
    float accumulator_ = 0.0f;
};

}  // namespace wr
