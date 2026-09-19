#include "scene/rigid.h"

#include <algorithm>
#include <cmath>

#include "core/jobs.h"
#include "core/log.h"
#include "physics/world.h"
#include "procgen/surface.h"
#include "render/mesh.h"
#include "scene/portal.h"

namespace wr {

// ----------------------------------------------------------- RigidBody3D

RigidBody3D::~RigidBody3D() { release(); }

Shape RigidBody3D::build_shape() const {
    switch (shape_kind) {
        case 0:
            return Shape::sphere(radius);
        case 1:
            return Shape::capsule(radius, height);
        default:
            return Shape::box(half_extents);
    }
}

void RigidBody3D::spawn(PhysicsWorld *world) {
    if (!world) return;
    release();
    world_ = world;
    Transform3D at = global_transform();
    body_ = world->dynamics().add(build_shape(), at, mass,
                                  kinematic ? BodyKind::Kinematic
                                            : BodyKind::Dynamic,
                                  this);
    if (RigidBody *b = world->dynamics().get(body_)) {
        b->friction = friction;
        b->restitution = restitution;
        b->linear_damping = linear_damping;
        b->angular_damping = angular_damping;
        b->gravity_scale = gravity_scale;
        b->layer = uint32_t(layer);
        b->mask = uint32_t(collision_mask);
        world->dynamics().refresh_mass(body_);
    }
}

void RigidBody3D::release() {
    if (world_ && body_.valid()) world_->dynamics().remove(body_);
    body_ = BodyId{};
    world_ = nullptr;
}

void RigidBody3D::on_ready() { set_physics_processing(true); }
void RigidBody3D::on_exit_tree() { release(); }

void RigidBody3D::on_physics(float) {
    if (!world_) return;
    RigidBody *b = world_->dynamics().get(body_);
    if (!b) return;
    // THE SOLVER OWNS THE TRANSFORM. Reading it back rather than
    // driving it is the whole point of a rigid body, and the scale
    // comes back too -- a cube that went through a portal into a
    // bigger one is bigger, and everything parented to it should
    // come along.
    if (b->kind == BodyKind::Kinematic) {
        // Except for a kinematic one, which the game drives. Its
        // velocity is derived so it can still shove dynamics about.
        const Transform3D t = global_transform();
        b->position = t.origin;
        b->orientation = t.basis.orthonormalized().to_quat();
        return;
    }
    set_global_transform(b->render_transform());
}

Vec3 RigidBody3D::get_velocity() const {
    if (!world_) return Vec3();
    const RigidBody *b =
        const_cast<PhysicsWorld *>(world_)->dynamics().get(body_);
    return b ? b->linear_velocity : Vec3();
}
void RigidBody3D::set_velocity(const Vec3 &v) {
    if (!world_) return;
    if (RigidBody *b = world_->dynamics().get(body_)) {
        b->linear_velocity = v;
        b->wake();
    }
}
Vec3 RigidBody3D::get_angular_velocity() const {
    if (!world_) return Vec3();
    const RigidBody *b =
        const_cast<PhysicsWorld *>(world_)->dynamics().get(body_);
    return b ? b->angular_velocity : Vec3();
}
void RigidBody3D::set_angular_velocity(const Vec3 &v) {
    if (!world_) return;
    if (RigidBody *b = world_->dynamics().get(body_)) {
        b->angular_velocity = v;
        b->wake();
    }
}
void RigidBody3D::apply_impulse(const Vec3 &j) {
    if (!world_) return;
    if (RigidBody *b = world_->dynamics().get(body_)) {
        b->wake();
        b->apply_impulse(j);
    }
}
void RigidBody3D::apply_impulse_at(const Vec3 &j, const Vec3 &at) {
    if (!world_) return;
    if (RigidBody *b = world_->dynamics().get(body_)) {
        b->wake();
        b->apply_impulse_at(j, at);
    }
}
void RigidBody3D::apply_force(const Vec3 &f) {
    if (!world_) return;
    if (RigidBody *b = world_->dynamics().get(body_)) {
        b->wake();
        b->apply_force(f);
    }
}
void RigidBody3D::apply_torque(const Vec3 &t) {
    if (!world_) return;
    if (RigidBody *b = world_->dynamics().get(body_)) {
        b->wake();
        b->torque += t;
    }
}
void RigidBody3D::wake() {
    if (!world_) return;
    if (RigidBody *b = world_->dynamics().get(body_)) b->wake();
}
bool RigidBody3D::sleeping() const {
    if (!world_) return false;
    const RigidBody *b =
        const_cast<PhysicsWorld *>(world_)->dynamics().get(body_);
    return b && b->sleeping;
}
float RigidBody3D::body_scale() const {
    if (!world_) return 1.0f;
    const RigidBody *b =
        const_cast<PhysicsWorld *>(world_)->dynamics().get(body_);
    return b ? b->scale : 1.0f;
}
bool RigidBody3D::warped() const {
    if (!world_) return false;
    const RigidBody *b =
        const_cast<PhysicsWorld *>(world_)->dynamics().get(body_);
    return b && b->warped;
}
Portal3D *RigidBody3D::warp_from() const {
    if (!world_) return nullptr;
    const RigidBody *b =
        const_cast<PhysicsWorld *>(world_)->dynamics().get(body_);
    return b ? b->warp_from : nullptr;
}
Portal3D *RigidBody3D::warp_to() const {
    if (!world_) return nullptr;
    const RigidBody *b =
        const_cast<PhysicsWorld *>(world_)->dynamics().get(body_);
    return b ? b->warp_to : nullptr;
}

void RigidBody3D::teleport(const Transform3D &to) {
    if (!world_) {
        set_global_transform(to);
        return;
    }
    RigidBody *b = world_->dynamics().get(body_);
    if (!b) {
        set_global_transform(to);
        return;
    }
    b->position = to.origin;
    b->prev_position = to.origin;
    b->orientation = to.basis.orthonormalized().to_quat();
    b->linear_velocity = Vec3();
    b->angular_velocity = Vec3();
    b->wake();
    set_global_transform(to);
}

void RigidBody3D::set_kinematic(bool on) {
    kinematic = on;
    if (!world_) return;
    RigidBody *b = world_->dynamics().get(body_);
    if (!b) return;
    b->kind = on ? BodyKind::Kinematic : BodyKind::Dynamic;
    b->linear_velocity = Vec3();
    b->angular_velocity = Vec3();
    // The inverse mass is zero for a kinematic body and has to be
    // put back when it stops being one, or a dropped object has
    // infinite mass and cannot be moved by anything.
    world_->dynamics().refresh_mass(body_);
    b->wake();
}

bool RigidBody3D::is_kinematic() const {
    if (!world_) return kinematic;
    const RigidBody *b =
        const_cast<PhysicsWorld *>(world_)->dynamics().get(body_);
    return b ? b->kind == BodyKind::Kinematic : kinematic;
}

void RigidBody3D::set_layer(int64_t value) {
    layer = value;
    if (!world_) return;
    if (RigidBody *b = world_->dynamics().get(body_))
        b->layer = uint32_t(value);
}

void RigidBody3D::set_body_scale(float value) {
    if (!world_) return;
    RigidBody *b = world_->dynamics().get(body_);
    if (!b) return;
    b->scale = std::max(0.01f, value);
    // Mass as the cube of the size. A barrel three times as tall
    // is twenty-seven times the barrel, and it should take
    // twenty-seven times the shove -- otherwise a giant one skates
    // about like a balloon and the size stops meaning anything.
    b->mass = mass * b->scale * b->scale * b->scale;
    world_->dynamics().refresh_mass(body_);
    b->wake();
}

void RigidBody3D::carry_to(const Vec3 &target, float strength, float damping) {
    if (!world_) return;
    RigidBody *b = world_->dynamics().get(body_);
    if (!b) return;
    b->wake();
    // A CRITICALLY DAMPED PULL, not a teleport.
    //
    // Welding the cube to the hand makes it pass through door
    // frames and lets a player push it into a wall and through it.
    // A spring lets the world win: the cube lags round a corner,
    // knocks against the frame, and can be shouldered aside -- and
    // it still arrives.
    //
    // Gravity is cancelled while carried, or the spring has to be
    // stiff enough to fight it and stiff is what makes it feel
    // welded again.
    const Vec3 to = target - b->position;
    // A spring toward the hand, damped by the body's own velocity,
    // so it arrives without ringing. Gravity is cancelled while
    // carried -- otherwise the spring has to be stiff enough to
    // fight it, and stiff is what makes it feel welded again.
    const Vec3 want = to * strength - b->linear_velocity * damping;
    b->linear_velocity += want * (1.0f / std::max(1.0f, damping + 1.0f));
    // AND A SPEED LIMIT ON IT. A cube snatched from across the room
    // would otherwise arrive at fifty metres a second and take the
    // player's legs off, and a cube pinned against a wall builds
    // up whatever the spring can give it and fires when it comes
    // free.
    const float sp = b->linear_velocity.length();
    if (sp > 12.0f) b->linear_velocity = b->linear_velocity * (12.0f / sp);
    b->force -= world_->dynamics().gravity * (b->mass * b->gravity_scale);
}

void RigidBody3D::align_to(const Quat &target, float strength, float damping) {
    if (!world_) return;
    RigidBody *b = world_->dynamics().get(body_);
    if (!b) return;
    b->wake();
    // THE SHORT WAY ROUND. q and -q name the same orientation, and
    // the difference between them is the difference between
    // turning ten degrees and turning three hundred and fifty.
    Quat want = target.normalized();
    if (dot(want, b->orientation) < 0.0f) want = -want;
    // The error as a world-frame rotation, then as a rotation
    // vector -- axis times angle -- which is the units angular
    // velocity is already in.
    const Quat e = (want * b->orientation.inverse()).normalized();
    const Vec3 axis{e.x, e.y, e.z};
    const float s = axis.length();
    Vec3 rot;
    if (s > EPS) rot = axis * (2.0f * std::atan2(s, e.w) / s);
    const Vec3 push = rot * strength - b->angular_velocity * damping;
    b->angular_velocity += push * (1.0f / std::max(1.0f, damping + 1.0f));
    const float sp = b->angular_velocity.length();
    if (sp > 20.0f) b->angular_velocity = b->angular_velocity * (20.0f / sp);
}

// -------------------------------------------------------------- Fluid3D

Fluid3D::Fluid3D() = default;
Fluid3D::~Fluid3D() = default;

float Fluid3D::frand() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return float(rng_ & 0xFFFFFF) / float(0xFFFFFF);
}

void Fluid3D::attach(PhysicsWorld *world) {
    world_ = world;
    fluid_ = std::make_unique<Fluid>(world);
    fluid_->particle_radius = particle_radius;
    fluid_->solver_iterations = solver_iterations;
    fluid_->max_particles = uint32_t(std::max<int64_t>(1, max_particles));
    fluid_->kill_below_y = kill_below_y;
    fluid_->material.smoothing_radius = smoothing_radius;
    fluid_->material.rest_density = rest_density;
    fluid_->material.viscosity = viscosity;
    fluid_->material.vorticity = vorticity;
    fluid_->material.friction = surface_friction;
    fluid_->material.colour = colour;
}

void Fluid3D::on_ready() {
    set_physics_processing(true);
    if (!fluid_ && world_) attach(world_);
    if (!surface_node_) {
        auto *n = new MeshInstance3D();
        n->set_name("Surface");
        // The surface is rebuilt in world space, so the node must
        // not add a transform of its own on top of it.
        n->add_to_group("no-navigation");
        surface_node_ = n;
        add_child(n);
    }
}

int64_t Fluid3D::emit(const Vec3 &at, const Vec3 &velocity) {
    if (!fluid_) return -1;
    const uint32_t id = fluid_->emit(at, velocity);
    return id == 0xFFFFFFFFu ? -1 : int64_t(id);
}

int64_t Fluid3D::fill_box(const Vec3 &from, const Vec3 &to) {
    if (!fluid_) return 0;
    AABB b;
    b.expand(from);
    b.expand(to);
    return fluid_->fill(b);
}

void Fluid3D::clear() {
    if (fluid_) fluid_->clear();
}

int64_t Fluid3D::particle_count() const {
    return fluid_ ? int64_t(fluid_->count()) : 0;
}

Array Fluid3D::take_settled() {
    Array out;
    if (!fluid_) return out;
    std::vector<Vec3> ps, ns;
    fluid_->drain_settled(&ps, &ns);
    for (size_t i = 0; i < ps.size(); i++) {
        Dict d;
        d["position"] = Variant(ps[i]);
        d["normal"] = Variant(i < ns.size() ? ns[i] : Vec3(0, 1, 0));
        out.push_back(Variant(d));
    }
    return out;
}

void Fluid3D::spray(const Vec3 &at, const Vec3 &direction, float rate,
                    float speed, float spread, float dt) {
    if (!fluid_) return;
    // THE DEBT IS THE POINT. A hose at thirty particles a second
    // and a frame of a sixtieth owes half a particle; dropping the
    // fraction emits fifteen a second instead and the rate silently
    // depends on the frame rate.
    emit_debt_ += rate * dt;
    Vec3 d = direction;
    const float len = d.length();
    if (len < 1e-6f) return;
    d = d * (1.0f / len);
    Vec3 up = std::fabs(d.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    const Vec3 sx = cross(d, up).normalized();
    const Vec3 sy = cross(d, sx);
    while (emit_debt_ >= 1.0f) {
        emit_debt_ -= 1.0f;
        const Vec3 jitter = sx * ((frand() - 0.5f) * spread) +
                            sy * ((frand() - 0.5f) * spread);
        const Vec3 nozzle = at + jitter * 0.5f;
        fluid_->emit(nozzle, (d + jitter) * speed);
    }
}

void Fluid3D::on_physics(float dt) {
    if (!fluid_) {
        if (world_) attach(world_);
        if (!fluid_) return;
    }
    fluid_->step(dt);
    if (!draw_surface || !surface_node_) return;
    surface_due_ -= dt;
    if (surface_due_ > 0.0f) return;
    surface_due_ = surface_hz > 0.0f ? 1.0f / surface_hz : 0.0f;
    rebuild_surface();
}

namespace {
// The liquid, as a field the contourer can read: negative inside.
class FluidField : public gen::Field {
public:
    FluidField(const Fluid *f, float level) : f_(f), level_(level) {}
    gen::Sample sample(const Vec3 &p) const override {
        gen::Sample s;
        // SCALED INTO SOMETHING LIKE METRES. The contourer wants a
        // roughly linear crossing so it can interpolate where the
        // surface is; a raw density runs to a thousand and crosses
        // in a couple of centimetres, which puts every vertex hard
        // against one end of its cell. Dividing by the level makes
        // the field about one at the surface and the interpolation
        // behave.
        const float d = f_->density_at(p);
        s.distance = (level_ - d) / std::max(1.0f, level_);
        s.material = 1;
        return s;
    }
    Vec3 gradient(const Vec3 &p, float) const override {
        const Vec3 g = f_->density_gradient_at(p);
        const float len = g.length();
        // The field is (level - density), so its gradient is minus
        // the density's; and the contourer wants it normalised.
        return len > 1e-6f ? g * (-1.0f / len) : Vec3(0, 1, 0);
    }

private:
    const Fluid *f_;
    float level_;
};
}  // namespace

void Fluid3D::rebuild_surface() {
    if (!fluid_ || fluid_->count() == 0) {
        if (surface_node_) surface_node_->mesh = Ref<Mesh>();
        return;
    }
    // CHUNKED, AND THE EMPTY CHUNKS ARE SKIPPED.
    //
    // Contouring costs the cube of the side, and a body of liquid's
    // bounding box is mostly empty -- a stream poured across a room
    // occupies a few per cent of the volume it spans. Meshing the
    // whole box is the obvious thing and it is unaffordable the
    // first time anything splashes: the same puddle that costs
    // three milliseconds in a bucket costs two hundred once it has
    // been thrown down a corridor, for a picture that is almost
    // entirely nothing.
    //
    // So the volume is cut into chunks, each is asked whether it
    // has any liquid in it, and only the ones that do are meshed.
    // They are independent -- the contourer says so -- so they go
    // out to the job system.
    const AABB box = fluid_->bounds();
    const Vec3 size = box.max - box.min;
    const float cell = std::max(0.02f, surface_cell);
    // Eight, not sixteen. A chunk has to be small enough that a
    // thin stream misses most of them: at sixteen cells -- a metre
    // and a half -- a stream poured across a room passes through
    // almost every chunk its bounding box contains and the skip
    // saves nothing. Halving the side is eight times as many
    // chunks and about a tenth as many of them occupied.
    const int chunk = 8;
    const float span = cell * float(chunk);
    const int nx = std::max(1, int(std::ceil(size.x / span)));
    const int ny = std::max(1, int(std::ceil(size.y / span)));
    const int nz = std::max(1, int(std::ceil(size.z / span)));
    // A ceiling on the chunk count, for the case where liquid has
    // genuinely covered the level. Past it the surface is drawn
    // coarser rather than the frame being dropped.
    const int total = nx * ny * nz;
    float use_cell = cell;
    if (total > 4096) {
        use_cell = cell * std::cbrt(float(total) / 4096.0f);
    }
    const float use_span = use_cell * float(chunk);
    const int cx = std::max(1, int(std::ceil(size.x / use_span)));
    const int cy = std::max(1, int(std::ceil(size.y / use_span)));
    const int cz = std::max(1, int(std::ceil(size.z / use_span)));

    struct Job {
        Vec3 origin;
        gen::ContourResult out;
    };
    std::vector<Job> jobs;
    for (int x = 0; x < cx; x++)
        for (int y = 0; y < cy; y++)
            for (int z = 0; z < cz; z++) {
                Job j;
                j.origin = box.min + Vec3(float(x), float(y), float(z)) * use_span
                           - Vec3(use_cell, use_cell, use_cell);
                const AABB chunk_box(
                    j.origin,
                    j.origin + Vec3(use_span, use_span, use_span) +
                        Vec3(use_cell * 2.0f, use_cell * 2.0f, use_cell * 2.0f));
                if (!fluid_->occupied(chunk_box)) continue;
                jobs.push_back(j);
            }
    if (jobs.empty()) {
        surface_node_->mesh = Ref<Mesh>();
        return;
    }

    FluidField field(fluid_.get(),
                     surface_level * fluid_->material.rest_density);
    Jobs::parallel_for(jobs.size(), 1, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; i++) {
            gen::ContourRequest req;
            req.field = &field;
            req.origin = jobs[i].origin;
            req.cell_size = use_cell;
            // Two cells of overlap, so neighbouring chunks meet
            // rather than leaving a seam of missing surface.
            req.resolution = chunk + 2;
            req.smooth_normals = true;
            gen::contour_field(req, &jobs[i].out);
        }
    });

    if (!surface_mesh_) surface_mesh_ = Ref<Mesh>(new Mesh());
    surface_mesh_->clear();
    AABB whole;
    for (Job &j : jobs) {
        if (j.out.empty || j.out.indices.empty()) continue;
        const uint32_t base = uint32_t(surface_mesh_->vertices.size());
        surface_mesh_->vertices.insert(surface_mesh_->vertices.end(),
                                       j.out.vertices.begin(),
                                       j.out.vertices.end());
        for (uint32_t idx : j.out.indices)
            surface_mesh_->indices.push_back(base + idx);
        whole.expand(j.out.bounds.min);
        whole.expand(j.out.bounds.max);
    }
    if (surface_mesh_->indices.empty()) {
        surface_node_->mesh = Ref<Mesh>();
        return;
    }
    SubMesh sm;
    sm.first_index = 0;
    sm.index_count = uint32_t(surface_mesh_->indices.size());
    sm.material_slot = 0;
    surface_mesh_->submeshes.assign(1, sm);
    surface_mesh_->set_bounds(whole);
    // THE ARRAYS CHANGED. Without this the renderer uploads the
    // first surface and draws it for ever -- the liquid moves in
    // the simulation and stands perfectly still on the screen.
    surface_mesh_->touch();
    surface_node_->mesh = surface_mesh_;
    if (surface_material) surface_node_->materials.assign(1, surface_material);
    surface_chunks_ = uint32_t(jobs.size());
    // The contour is in world space; the node must not move it.
    surface_node_->set_global_transform(Transform3D());
}

// ------------------------------------------------------------ reflection

static void register_rigid_classes() {
    ClassBuilder<RigidBody3D>()
        .field("shape_kind", &RigidBody3D::shape_kind)
        .field("radius", &RigidBody3D::radius)
        .field("height", &RigidBody3D::height)
        .field("half_extents", &RigidBody3D::half_extents)
        .field("mass", &RigidBody3D::mass)
        .field("friction", &RigidBody3D::friction)
        .field("restitution", &RigidBody3D::restitution)
        .field("linear_damping", &RigidBody3D::linear_damping)
        .field("angular_damping", &RigidBody3D::angular_damping)
        .field("gravity_scale", &RigidBody3D::gravity_scale)
        .field("layer", &RigidBody3D::layer)
        .field("collision_mask", &RigidBody3D::collision_mask)
        .field("kinematic", &RigidBody3D::kinematic)
        .method("spawn", &RigidBody3D::spawn).args("world")
        .method("release", &RigidBody3D::release)
        .method("alive", &RigidBody3D::alive)
        .prop("velocity", &RigidBody3D::get_velocity, &RigidBody3D::set_velocity)
        .prop("angular_velocity", &RigidBody3D::get_angular_velocity,
              &RigidBody3D::set_angular_velocity)
        .method("apply_impulse", &RigidBody3D::apply_impulse).args("impulse")
        .method("apply_impulse_at", &RigidBody3D::apply_impulse_at)
        .args("impulse", "world_point")
        .method("apply_force", &RigidBody3D::apply_force).args("force")
        .method("apply_torque", &RigidBody3D::apply_torque).args("torque")
        .method("wake", &RigidBody3D::wake)
        .method("is_sleeping", &RigidBody3D::sleeping)
        .method("body_scale", &RigidBody3D::body_scale)
        .method("teleport", &RigidBody3D::teleport).args("to")
        .method("set_kinematic", &RigidBody3D::set_kinematic).args("on")
        .method("is_kinematic", &RigidBody3D::is_kinematic)
        .method("set_body_scale", &RigidBody3D::set_body_scale).args("value")
        .method("set_layer", &RigidBody3D::set_layer).args("value")
        .method("warped", &RigidBody3D::warped)
        .method("carry_to", &RigidBody3D::carry_to,
                {Variant(18.0), Variant(4.0)})
        .args("target", "strength", "damping")
        .method("align_to", &RigidBody3D::align_to,
                {Variant(12.0), Variant(3.0)})
        .args("target", "strength", "damping")
        .method("warp_from", &RigidBody3D::warp_from)
        .method("warp_to", &RigidBody3D::warp_to);

    ClassBuilder<Fluid3D>()
        .field("particle_radius", &Fluid3D::particle_radius)
        .field("smoothing_radius", &Fluid3D::smoothing_radius)
        .field("rest_density", &Fluid3D::rest_density)
        .field("viscosity", &Fluid3D::viscosity)
        .field("vorticity", &Fluid3D::vorticity)
        .field("surface_friction", &Fluid3D::surface_friction)
        .field("solver_iterations", &Fluid3D::solver_iterations)
        .field("max_particles", &Fluid3D::max_particles)
        .field("colour", &Fluid3D::colour)
        .field("kill_below_y", &Fluid3D::kill_below_y)
        .field("draw_surface", &Fluid3D::draw_surface)
        .field("surface_cell", &Fluid3D::surface_cell)
        .field("surface_level", &Fluid3D::surface_level)
        .field("surface_hz", &Fluid3D::surface_hz)
        .field("surface_material", &Fluid3D::surface_material)
        .method("attach", &Fluid3D::attach).args("world")
        .method("emit", &Fluid3D::emit).args("at", "velocity")
        .method("fill_box", &Fluid3D::fill_box).args("from", "to")
        .method("clear", &Fluid3D::clear)
        .method("particle_count", &Fluid3D::particle_count)
        .method("surface_chunks", &Fluid3D::surface_chunks)
        .method("take_settled", &Fluid3D::take_settled)
        .method("spray", &Fluid3D::spray)
        .args("at", "direction", "rate", "speed", "spread", "dt");
}
WR_REGISTER(register_rigid_classes)

}  // namespace wr
