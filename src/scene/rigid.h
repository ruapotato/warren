// Warren -- rigid bodies and liquids, as nodes.
//
// The solvers live in physics/. These are the handles a scene puts
// on them: a node whose transform IS a body's, and a node that owns
// a body of liquid and turns it into a surface you can see.
#pragma once

#include <memory>

#include "physics/dynamics.h"
#include "physics/fluid.h"
#include "scene/nodes.h"

namespace wr {

class PhysicsWorld;
class Mesh;

// A THING THAT FALLS OVER.
//
// The node's transform is the body's: the solver writes it every
// physics tick and reading it back mid-frame gives where the body
// actually is. Setting it moves the body, which is what a game
// wants for a respawn and wants to avoid doing every frame.
//
// It carries its own scale through portals, like CharacterBody3D
// does -- a cube through a small hole into a large one comes out
// large, and the node scales with it so the mesh, the light and
// anything else parented to it come along.
class RigidBody3D : public Node3D {
    WR_CLASS(RigidBody3D, Node3D)

public:
    RigidBody3D() = default;
    ~RigidBody3D() override;

    // --- shape, at size 1 --------------------------------------------
    // Set before `spawn`. A shape changed afterwards needs `respawn`.
    int shape_kind = 2;                       // 0 sphere, 1 capsule, 2 box
    float radius = 0.25f;
    float height = 0.5f;
    Vec3 half_extents{0.25f, 0.25f, 0.25f};

    float mass = 10.0f;
    float friction = 0.7f;
    float restitution = 0.0f;
    float linear_damping = 0.05f;
    float angular_damping = 0.08f;
    float gravity_scale = 1.0f;
    int64_t layer = 1;
    int64_t collision_mask = 0xFFFFFFFF;
    // Kinematic bodies are moved by the game and push dynamics out
    // of the way without being pushed back.
    bool kinematic = false;

    // Join the world. Idempotent; call again after changing a shape.
    void spawn(PhysicsWorld *world);
    void release();
    bool alive() const { return body_.valid(); }

    // --- state ---------------------------------------------------------
    Vec3 get_velocity() const;
    void set_velocity(const Vec3 &v);
    Vec3 get_angular_velocity() const;
    void set_angular_velocity(const Vec3 &v);
    void apply_impulse(const Vec3 &j);
    void apply_impulse_at(const Vec3 &j, const Vec3 &world_point);
    void apply_force(const Vec3 &f);
    void apply_torque(const Vec3 &t);
    void wake();
    bool sleeping() const;
    float body_scale() const;

    // PUT IT SOMEWHERE, and mean it.
    //
    // Setting the node's transform does nothing: the solver owns
    // the body and writes the node back every tick, so the move
    // lasts until the next one. A respawn, a teleport, a thing
    // locked to the player's hands -- all of them need to move the
    // BODY, and all of them want the velocity cleared with it or
    // the object arrives carrying whatever it was doing before.
    void teleport(const Transform3D &to);
    // Kinematic bodies are driven by the game and shove dynamics
    // out of the way without being shoved back. Switching at
    // runtime is what picking something up is: it stops being
    // simulated and starts being carried.
    void set_kinematic(bool on);
    bool is_kinematic() const;
    // RESIZE IT, keeping its mass sensible.
    //
    // The scale rides in the solver because portals change it, and
    // a game that wants a barrel three times the size wants the
    // same field -- so there is one notion of how big a body is
    // and not two that can disagree. Mass goes as the cube, which
    // is why a big one is hard to shift.
    void set_body_scale(float value);
    // True for one tick after it came through an aperture.
    bool warped() const;

    // CARRIED, NOT HELD. A cube picked up is not welded to the
    // player's hand -- it is pulled toward a point, so it bumps
    // into door frames and can be knocked out of the way, which is
    // the whole feel of carrying something in a game like this.
    // Call every tick with where the hand is.
    void carry_to(const Vec3 &target, float strength = 18.0f,
                  float damping = 4.0f);

    void on_ready() override;
    void on_physics(float dt) override;
    void on_exit_tree() override;

private:
    PhysicsWorld *world_ = nullptr;
    BodyId body_;
    Shape build_shape() const;
};

// A BODY OF LIQUID, AND ITS SURFACE.
//
// Drawing a fluid as particles makes it look like particles. What
// makes it read as liquid is an isosurface, and the engine already
// has a dual contourer that will build one out of any field -- so
// the node asks the solver how dense it is at a point and hands
// that to the mesher.
//
// The surface is rebuilt at its own rate. Contouring is by far the
// most expensive thing here and a liquid does not need a new
// surface every frame to look right; twenty a second is plenty and
// leaves the solver the budget.
class Fluid3D : public Node3D {
    WR_CLASS(Fluid3D, Node3D)

public:
    Fluid3D();
    ~Fluid3D() override;

    void attach(PhysicsWorld *world);
    Fluid *fluid() { return fluid_.get(); }

    // --- the liquid -----------------------------------------------------
    float particle_radius = 0.045f;
    float smoothing_radius = 0.135f;
    float rest_density = 1000.0f;
    float viscosity = 0.02f;
    float vorticity = 0.15f;
    float surface_friction = 0.15f;
    int solver_iterations = 6;
    int64_t max_particles = 20000;
    Color colour{0.25f, 0.55f, 0.95f, 1.0f};
    // Where anything that escapes is forgotten.
    float kill_below_y = -200.0f;

    // --- the surface ------------------------------------------------------
    bool draw_surface = true;
    // Metres per contour cell. Roughly the particle diameter is the
    // sweet spot: finer costs the cube of the difference and shows
    // the particles, coarser rounds the liquid off.
    float surface_cell = 0.09f;
    // WHERE THE SURFACE IS, as a fraction of the rest density. Low
    // and the liquid is a fog that reaches too far; high and thin
    // streams disappear because no point in them is dense enough.
    float surface_level = 0.45f;
    float surface_hz = 20.0f;
    Ref<Material> surface_material;

    // --- putting liquid in ------------------------------------------------
    int64_t emit(const Vec3 &at, const Vec3 &velocity);
    int64_t fill_box(const Vec3 &from, const Vec3 &to);
    void clear();
    int64_t particle_count() const;
    // WHERE GEL HAS LANDED SINCE YOU LAST ASKED, as a list of
    // {"position", "normal"}. Drained: each landing comes back
    // once. See Fluid::drain_settled -- the alternative is
    // pulling every particle across the bridge every frame,
    // which costs more than simulating them.
    Array take_settled();
    // How many chunks the surface was built out of last time. The
    // cost of drawing the liquid, in one number.
    int64_t surface_chunks() const { return int64_t(surface_chunks_); }
    // A hose. Called every tick while something is pouring.
    void spray(const Vec3 &at, const Vec3 &direction, float rate, float speed,
               float spread, float dt);

    void on_ready() override;
    void on_physics(float dt) override;

private:
    void rebuild_surface();

    PhysicsWorld *world_ = nullptr;
    std::unique_ptr<Fluid> fluid_;
    MeshInstance3D *surface_node_ = nullptr;
    Ref<Mesh> surface_mesh_;
    float surface_due_ = 0.0f;
    float emit_debt_ = 0.0f;
    uint32_t surface_chunks_ = 0;
    uint32_t rng_ = 0x2545F491u;
    float frand();
};

}  // namespace wr
