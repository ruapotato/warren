// Warren -- rigid bodies.
//
// WHAT THIS IS FOR. A portal game is a physics game: the cube you
// carry, the cube you drop through a hole in the floor to catch
// yourself with, the momentum you keep when you come out of a wall
// travelling sideways. None of that is possible against a collision
// library that only answers queries, which is what Warren had.
//
// THE SOLVER IS SEQUENTIAL IMPULSES with warm starting -- the same
// family as Box2D and Bullet's default. Chosen over the alternatives
// for one reason: it is the one that holds a STACK still. A crate
// resting on a crate resting on the floor is the thing every toy
// solver gets wrong, and it is the first thing anybody puts in a
// test chamber.
//
// The three parts that make that true, none of which is optional:
//
//   MANIFOLDS, not single points. See manifold.h -- a box held at
//   one point is a box on a pivot.
//
//   WARM STARTING. Last step's impulse is applied again before
//   solving, so the solver starts from the answer it had rather
//   than from zero. Without it a stack sags visibly under its own
//   weight and springs back, because ten iterations from zero
//   cannot find the force a tower needs and ten from last frame
//   can.
//
//   SPLIT POSITION CORRECTION. Pushing overlap out through the
//   velocity solver adds energy -- a dropped crate that lands
//   overlapping is launched back up by the correction. The
//   correction is solved separately and does not feed the
//   velocities.
//
// PORTALS ARE PART OF IT, not layered on top. A body crossing an
// aperture has its transform, its linear velocity, its angular
// velocity AND its size carried through, and while it straddles the
// plane it collides with the geometry on both sides. Doing that
// from outside the solver means a cube that is half through a
// portal is resting on nothing.
#pragma once

#include <unordered_map>
#include <vector>

#include "core/math/transform.h"
#include "physics/manifold.h"
#include "physics/shapes.h"

namespace wr {

class PhysicsWorld;
class Node3D;
class Portal3D;

struct BodyId {
    uint32_t index = 0;
    uint32_t generation = 0;
    bool valid() const { return generation != 0; }
    bool operator==(const BodyId &o) const {
        return index == o.index && generation == o.generation;
    }
    int64_t packed() const {
        return int64_t(index) | (int64_t(generation) << 32);
    }
    static BodyId unpack(int64_t v) {
        BodyId id;
        id.index = uint32_t(v & 0xFFFFFFFF);
        id.generation = uint32_t(uint64_t(v) >> 32);
        return id;
    }
};

enum class BodyKind : uint8_t {
    // Moved by the solver.
    Dynamic,
    // Moved by the game, and pushes dynamics out of the way without
    // being pushed back. A lift, a door, a moving platform.
    Kinematic,
};

struct RigidBody {
    Vec3 position;
    // Where it was at the start of the step. Kept rather than
    // reconstructed from the velocity: a body whose velocity
    // changed mid-step -- which is every body that hit something --
    // was never where v * dt says it was, and a portal test on that
    // guess triggers on apertures the body came nowhere near.
    Vec3 prev_position;
    Quat orientation;
    Vec3 linear_velocity;
    Vec3 angular_velocity;
    // Cleared at the end of every step, like every other engine, so
    // a force applied once is applied for one step.
    Vec3 force;
    Vec3 torque;

    Shape shape;
    BodyKind kind = BodyKind::Dynamic;
    float mass = 1.0f;
    float inv_mass = 1.0f;
    Basis inv_inertia_local;          // diagonal
    Basis inv_inertia_world;          // recomputed per step

    float restitution = 0.0f;
    float friction = 0.7f;
    // A little of both, always. Zero damping is not "realistic", it
    // is a body that never stops -- real air and real bearings take
    // energy out, and without any the sleep test never fires.
    float linear_damping = 0.05f;
    float angular_damping = 0.08f;
    float gravity_scale = 1.0f;

    uint32_t layer = 1;
    uint32_t mask = 0xFFFFFFFF;
    Node3D *owner = nullptr;

    // Carried through portals, and multiplied into the shape. The
    // renderer and the game read it to draw the body at the size the
    // physics believes it is.
    float scale = 1.0f;

    bool sleeping = false;
    float sleep_timer = 0.0f;
    // Set for one step after the body came through an aperture, so a
    // game can cut a camera or play a sound without guessing.
    bool warped = false;
    Transform3D last_warp = Transform3D::identity();

    Transform3D transform() const {
        Transform3D t;
        t.basis = Basis(orientation) * scale;
        t.origin = position;
        return t;
    }
    // The shape at its current size. The solver wants this often
    // enough that recomputing it inline everywhere is a mistake
    // waiting to be made in one place and not another.
    Shape sized() const { return shape.scaled(scale); }

    void apply_force(const Vec3 &f) { force += f; }
    void apply_force_at(const Vec3 &f, const Vec3 &world_point) {
        force += f;
        torque += cross(world_point - position, f);
    }
    void apply_impulse(const Vec3 &j) { linear_velocity += j * inv_mass; }
    void apply_impulse_at(const Vec3 &j, const Vec3 &world_point) {
        linear_velocity += j * inv_mass;
        angular_velocity +=
            inv_inertia_world.xform(cross(world_point - position, j));
    }
    void wake() {
        sleeping = false;
        sleep_timer = 0.0f;
    }
};

class DynamicsWorld {
public:
    explicit DynamicsWorld(PhysicsWorld *world) : world_(world) {}

    BodyId add(const Shape &shape, const Transform3D &at, float mass,
               BodyKind kind = BodyKind::Dynamic, Node3D *owner = nullptr);
    void remove(BodyId id);
    RigidBody *get(BodyId id);
    const RigidBody *get(BodyId id) const;
    size_t body_count() const { return live_; }
    // Every live body, for a renderer or a debug draw.
    void each(void (*fn)(RigidBody &, void *), void *user);

    // Recompute the inertia after mass or shape changed.
    void refresh_mass(BodyId id);

    // ONE STEP, AND IT IS FIXED. A solver fed a variable dt changes
    // stiffness every frame -- a stack that stands at 60 fps sags at
    // 30 and explodes at 5, and the bug is reported as "physics is
    // broken on my machine". `advance` takes real time, runs whole
    // steps of `fixed_step`, and hands back how much is left over so
    // a game can interpolate.
    void step(float dt);
    float advance(float real_dt);

    Vec3 gravity{0.0f, -9.81f, 0.0f};

    // WHAT A RESIZING PORTAL CONSERVES, and it is a choice rather
    // than a fact, because a portal that changes your size is not
    // a physical process. Three self-consistent answers exist and
    // only one of them is a good game:
    //
    //   SELF-SIMILAR. World velocity scales with size, so nothing
    //   changes in your own frame -- you did not shrink, the room
    //   grew. Honest, and completely inert: shrinking has no
    //   consequence you can feel, so a size machine gives you
    //   nothing to do with it.
    //
    //   MOMENTUM WITH MASS CONSERVED. p = mv, mass unchanged, so
    //   world velocity is unchanged and a tenth-size body covers
    //   ten times its own length per second. This is the reading
    //   of "a less dense material becomes denser and smaller",
    //   and its problem is the mass: a shrunk crate still weighs
    //   what it did and cannot be pushed, which is the opposite
    //   of what a player expects of a small thing.
    //
    //   MOMENTUM WITH DENSITY CONSERVED. Mass falls as the cube,
    //   so conserving p sends velocity up as 1/k^3. Shrink by ten
    //   and you leave at a thousand times the speed. Not a game.
    //
    // What is used: SPEED IS PRESERVED and mass follows the
    // volume. It is the plain generalisation of what an ordinary
    // equal-sized portal does -- speedy thing goes in, speedy
    // thing comes out -- and it gives the effect the second rule
    // was wanted for, because a metre per second means something
    // very different to a body a tenth of a metre tall. Small is
    // fast, big is ponderous, and a small crate is still light.
    //
    // Set false for the self-similar rule.
    bool portals_preserve_speed = true;
    // AND THE SKATER, which is the one piece of real conservation
    // here. Angular momentum L = I*omega, and a body whose mass
    // stays put while its radius shrinks by k has I fall as k^2 --
    // so omega rises as 1/k^2. That is a figure skater pulling
    // their arms in, and it is worth having exactly because it is
    // the one place the intuition is right.
    //
    // Clamped, because 1/k^2 at a tenth scale is a hundredfold and
    // a body spinning at that rate is a solver problem and an
    // unreadable picture.
    float spin_conservation = 2.0f;
    float max_spin = 25.0f;
    float fixed_step = 1.0f / 120.0f;
    // Above this many steps in one frame, give up and let time slip.
    // Otherwise a machine that cannot keep up spends longer and
    // longer catching up and stops responding altogether.
    int max_steps = 8;
    int velocity_iterations = 8;
    // The pass that takes the bias energy back out. See the comment
    // in step(): without it a stack never stops ringing.
    int relax_iterations = 3;
    // How much overlap is tolerated before it is pushed out, and how
    // fast. Some slop is essential: correcting to exactly zero makes
    // resting contacts flicker between touching and not.
    float slop = 0.005f;
    // A CONTACT IS A VERY STIFF SPRING, not a hard constraint.
    //
    // Correcting overlap with a plain Baumgarte term makes the
    // biased solve produce a much larger impulse than the contact
    // actually needs, and the relax pass that follows then has to
    // remove a correspondingly large amount -- which it overshoots,
    // leaving the contact carrying less than the weight above it.
    // A single box is fine. A stack of five sinks into itself over
    // about five seconds, one contact at a time, and every frame of
    // it looks like collision failing rather than the solver being
    // over-corrected.
    //
    // Stating the contact as a spring at a known frequency and
    // damping ratio fixes the magnitude: the solve is scaled so
    // that the impulse it asks for is the impulse a spring that
    // stiff would apply over one step, and relax then removes
    // exactly that and no more. Thirty hertz is the usual choice --
    // stiff enough to look rigid, soft enough to stay stable at a
    // quarter of the step rate.
    float contact_hertz = 30.0f;
    float contact_damping = 10.0f;
    // The fastest overlap is ever pushed out. Without a cap a body
    // that starts inside a wall leaves at whatever speed the depth
    // over one step implies, which is hundreds of metres a second.
    float max_bias_velocity = 4.0f;
    // Contacts are found this far before they touch, so a fast body
    // is stopped at the surface rather than being pushed back out of
    // it after the fact.
    float speculative_margin = 0.04f;
    // Below these, for this long, a body goes to sleep.
    float sleep_linear = 0.05f;
    float sleep_angular = 0.08f;
    float sleep_after = 0.6f;

    // Diagnostics that answer the questions actually asked of a
    // solver: is it finding contacts, and is it converging.
    struct Stats {
        uint32_t bodies_awake = 0;
        uint32_t manifolds = 0;
        uint32_t contacts = 0;
        // HOW MANY CONTACTS KEPT THEIR IMPULSE FROM LAST STEP.
        //
        // Warm starting is most of what makes a stack stand, and it
        // only happens when a regenerated contact is recognised as
        // the same one. If the feature ids are not stable this
        // number sits near zero while everything else looks
        // plausible, and the only symptom is a solver that will not
        // converge -- so it is worth counting rather than assuming.
        uint32_t warm_started = 0;
        uint32_t portal_crossings = 0;
        uint32_t substeps = 0;
        float leftover = 0.0f;
    };
    Stats stats;

private:
    struct Slot {
        RigidBody body;
        uint32_t generation = 0;
        bool live = false;
    };
    // A contact set between one body and one other thing -- another
    // body, or one triangle of the static world.
    struct PairKey {
        uint32_t a = 0, b = 0, extra = 0;
        bool operator==(const PairKey &o) const {
            return a == o.a && b == o.b && extra == o.extra;
        }
    };
    struct PairHash {
        size_t operator()(const PairKey &k) const {
            uint64_t h = k.a * 0x9E3779B97F4A7C15ull;
            h ^= uint64_t(k.b) + 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
            h ^= uint64_t(k.extra) + 0x27D4EB2F165667C5ull + (h << 6) + (h >> 2);
            return size_t(h);
        }
    };
    struct Pair {
        PairKey key;
        Manifold manifold;
        // Which body, and what it is against. `other` is kNoBody for
        // the static world.
        uint32_t a = 0, b = 0;
        // Static contacts remember the surface they came from.
        Vec3 static_normal;
        bool touched = false;
        float friction = 0.7f;
        float restitution = 0.0f;
        // Per-contact solver scratch, rebuilt each step.
        Vec3 ra[4], rb[4];
        float normal_mass[4] = {0, 0, 0, 0};
        float tangent_mass[4][2] = {};
        Vec3 tangent[2];
        float bias[4] = {0, 0, 0, 0};
        // The soft-constraint pair: how much of the requested
        // impulse to apply, and how much of the accumulated one to
        // give back. Both are 1 and 0 for a speculative contact,
        // which is a hard constraint and wants no softening.
        float mass_scale[4] = {1, 1, 1, 1};
        float impulse_scale[4] = {0, 0, 0, 0};
        float restitution_bias[4] = {0, 0, 0, 0};
    };
    static constexpr uint32_t kNoBody = 0xFFFFFFFFu;

    void integrate_velocities(float dt);
    void collect_pairs();
    void prepare(float dt);
    void warm_start();
    void solve_velocities(int iterations, bool use_bias);
    void apply_restitution();
    void integrate_positions(float dt);
    void cross_portals();
    void settle(float dt);

    PhysicsWorld *world_ = nullptr;
    std::vector<Slot> slots_;
    std::vector<uint32_t> free_;
    size_t live_ = 0;
    std::unordered_map<PairKey, Pair, PairHash> pairs_;
    std::vector<PairKey> order_;
    float accumulator_ = 0.0f;
    // Scratch, kept between steps so the step does not allocate.
    std::vector<uint32_t> scratch_tris_;
};

}  // namespace wr
