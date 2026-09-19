#include "physics/dynamics.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "physics/world.h"
#include "scene/portal.h"

namespace wr {
namespace {

// Two unit vectors perpendicular to `n` and to each other. Friction
// needs a basis in the contact plane and does not care which one, as
// long as it is not degenerate -- so branch on the component of n
// that is smallest, which is the one guaranteed not to be parallel.
void tangent_basis(const Vec3 &n, Vec3 *t0, Vec3 *t1) {
    const Vec3 a = std::fabs(n.x) < 0.9f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
    *t0 = cross(n, a).normalized();
    *t1 = cross(n, *t0);
}

// Rotate an orientation by an angular velocity. The exact form
// rather than q += 0.5*w*q*dt, because the cheap one loses
// normalisation fast enough to be visible on a spinning cube.
Quat integrate_spin(const Quat &q, const Vec3 &w, float dt) {
    const float len = w.length();
    if (len < 1e-7f) return q;
    const Quat dq = Quat::from_axis_angle(w * (1.0f / len), len * dt);
    return (dq * q).normalized();
}

}  // namespace

// --------------------------------------------------------------- bodies

BodyId DynamicsWorld::add(const Shape &shape, const Transform3D &at, float mass,
                          BodyKind kind, Node3D *owner) {
    uint32_t index;
    if (!free_.empty()) {
        index = free_.back();
        free_.pop_back();
    } else {
        index = uint32_t(slots_.size());
        slots_.emplace_back();
    }
    Slot &s = slots_[index];
    s.live = true;
    if (s.generation == 0) s.generation = 1;
    s.body = RigidBody{};
    s.body.shape = shape;
    s.body.position = at.origin;
    s.body.orientation = at.basis.orthonormalized().to_quat();
    s.body.scale = at.basis.uniform_scale();
    if (s.body.scale < 1e-4f) s.body.scale = 1.0f;
    s.body.kind = kind;
    s.body.mass = mass;
    s.body.owner = owner;
    live_++;
    BodyId id{index, s.generation};
    refresh_mass(id);
    if (world_) {
        s.body.proxy = world_->add_shape(s.body.sized(), s.body.pose(),
                                         s.body.layer, owner,
                                         /*is_static=*/false);
    }
    return id;
}

void DynamicsWorld::remove(BodyId id) {
    if (!id.valid() || id.index >= slots_.size()) return;
    Slot &s = slots_[id.index];
    if (!s.live || s.generation != id.generation) return;
    if (world_ && s.body.proxy.valid()) world_->remove(s.body.proxy);
    s.body.proxy = ColliderId{};
    s.live = false;
    s.generation++;
    if (s.generation == 0) s.generation = 1;
    free_.push_back(id.index);
    live_--;
    // Anything remembering a contact with it must forget: reusing
    // the slot would otherwise inherit the old body's warm start.
    for (auto it = pairs_.begin(); it != pairs_.end();) {
        if (it->second.a == id.index || it->second.b == id.index)
            it = pairs_.erase(it);
        else
            ++it;
    }
}

RigidBody *DynamicsWorld::get(BodyId id) {
    if (!id.valid() || id.index >= slots_.size()) return nullptr;
    Slot &s = slots_[id.index];
    return (s.live && s.generation == id.generation) ? &s.body : nullptr;
}
const RigidBody *DynamicsWorld::get(BodyId id) const {
    return const_cast<DynamicsWorld *>(this)->get(id);
}

void DynamicsWorld::each(void (*fn)(RigidBody &, void *), void *user) {
    for (Slot &s : slots_)
        if (s.live) fn(s.body, user);
}

void DynamicsWorld::refresh_mass(BodyId id) {
    RigidBody *b = get(id);
    if (!b) return;
    if (b->kind != BodyKind::Dynamic || b->mass <= 0.0f) {
        b->inv_mass = 0.0f;
        b->inv_inertia_local = Basis(Vec3(), Vec3(), Vec3());
        b->inv_inertia_world = b->inv_inertia_local;
        return;
    }
    b->inv_mass = 1.0f / b->mass;
    // The inertia of the shape AT ITS CURRENT SIZE. A cube that came
    // through a portal twice its size is eight times the mass's
    // worth of inertia, and using the unscaled tensor makes a large
    // cube spin like a small one.
    const Basis I = inertia_of(b->sized(), b->mass);
    Basis inv;
    for (int i = 0; i < 3; i++) {
        const float d = I.col[i][i];
        inv.col[i] = Vec3();
        inv.col[i][i] = d > 1e-9f ? 1.0f / d : 0.0f;
    }
    b->inv_inertia_local = inv;
    b->inv_inertia_world = inv;
}

// ----------------------------------------------------------- the step

float DynamicsWorld::advance(float real_dt) {
    accumulator_ += real_dt;
    int n = 0;
    stats.substeps = 0;
    while (accumulator_ >= fixed_step && n < max_steps) {
        step(fixed_step);
        accumulator_ -= fixed_step;
        n++;
        stats.substeps++;
    }
    // BEHIND, AND NOT CATCHING UP. Throw the backlog away rather
    // than carrying it: a frame that took a second is not a reason
    // to simulate a second next frame, it is a reason to admit the
    // second is gone.
    if (accumulator_ > fixed_step * float(max_steps)) accumulator_ = 0.0f;
    stats.leftover = accumulator_;
    return accumulator_ / fixed_step;
}

void DynamicsWorld::begin_frame() {
    in_frame_ = true;
    clear_frame_flags();
}

void DynamicsWorld::end_frame() { in_frame_ = false; }

void DynamicsWorld::clear_frame_flags() {
    for (Slot &s : slots_)
        if (s.live) s.body.warped = false;
}

void DynamicsWorld::step(float dt) {
    stats.manifolds = 0;
    stats.contacts = 0;
    stats.warm_started = 0;
    stats.bodies_awake = 0;
    // Only when nobody is bracketing the frame. See begin_frame.
    if (!in_frame_) clear_frame_flags();
    integrate_velocities(dt);
    collect_pairs();
    prepare(dt);
    warm_start();
    solve_velocities(velocity_iterations, true);
    integrate_positions(dt);
    // RELAX: THE SAME SOLVE AGAIN WITH THE BIAS TURNED OFF.
    //
    // The bias is a lie told to the solver to make it fix overlap
    // and stop fast bodies -- and a lie that adds energy. A stack
    // solved with it and then left alone breathes: it compresses a
    // few millimetres, the bias pushes back harder than gravity
    // pulled, and the tower rings like a spring for ever and never
    // falls asleep.
    //
    // Running the contacts again with a target of zero, against the
    // same accumulated impulses, takes that energy back out without
    // undoing the correction -- the positions have already moved.
    // It is the cheapest two iterations in the solver and it is the
    // difference between a stack that settles and one that hums.
    solve_velocities(relax_iterations, false);
    apply_restitution();
    cross_portals();
    settle(dt);
    sync_proxies();
    for (Slot &s : slots_) {
        if (!s.live) continue;
        s.body.force = Vec3();
        s.body.torque = Vec3();
    }
}

void DynamicsWorld::integrate_velocities(float dt) {
    for (Slot &s : slots_) {
        if (!s.live) continue;
        RigidBody &b = s.body;
        if (b.kind != BodyKind::Dynamic || b.sleeping) continue;
        stats.bodies_awake++;
        // The world-space inverse inertia: R * I^-1 * R^T. Rebuilt
        // every step because the body has turned since the last one.
        const Basis R(b.orientation);
        b.inv_inertia_world = R * b.inv_inertia_local * R.transposed();

        b.linear_velocity += (gravity * b.gravity_scale + b.force * b.inv_mass) * dt;
        b.angular_velocity += b.inv_inertia_world.xform(b.torque) * dt;
        // Exponential damping, which is stable at any dt. The naive
        // v *= (1 - d*dt) goes negative and flips the body's
        // direction the moment d*dt exceeds one.
        b.linear_velocity *= 1.0f / (1.0f + b.linear_damping * dt);
        b.angular_velocity *= 1.0f / (1.0f + b.angular_damping * dt);
    }
}

void DynamicsWorld::collect_pairs() {
    for (auto &kv : pairs_) kv.second.touched = false;
    order_.clear();
    if (!world_) return;

    // Body against the static world, and body against body. Brute
    // force on the second: a test chamber has tens of dynamic
    // bodies, not thousands, and a broad phase that is not needed
    // is a broad phase that is wrong in some corner nobody visits.
    std::vector<PhysicsWorld::WorldTriangle> tris;
    std::vector<ColliderId> hits;
    for (uint32_t i = 0; i < slots_.size(); i++) {
        Slot &si = slots_[i];
        if (!si.live) continue;
        RigidBody &a = si.body;
        if (a.kind != BodyKind::Dynamic || a.sleeping) continue;
        const Shape sa = a.sized();
        const Transform3D ta = a.pose();
        // THE MARGIN HAS TO COVER THE MOTION, or the speculative
        // contact is never generated for the body that needs it.
        //
        // A body at 60 m/s moves half a metre per step. Looking
        // four centimetres ahead means that on one step it is half
        // a metre above the floor and on the next it is half a
        // metre below, and it was never within four centimetres of
        // anything. Looking a step's worth of travel ahead finds
        // the floor while there is still time to stop at it --
        // which, with the bias above, is exactly what happens.
        const float margin =
            speculative_margin + a.linear_velocity.length() * fixed_step;
        AABB box = sa.world_bounds(ta).grown(margin + 0.02f);
        AABB swept = box;
        swept.expand(box.min + a.linear_velocity * fixed_step);
        swept.expand(box.max + a.linear_velocity * fixed_step);

        tris.clear();
        world_->query_triangles(swept, a.mask, tris);
        for (const auto &wt : tris) {
            Manifold m;
            if (!collide_triangle(sa, ta, wt.v, &m, margin)) continue;
            // A CONTACT INSIDE AN OPEN APERTURE IS NOT A CONTACT.
            // The level is one mesh with no hole cut in it, so
            // without this a crate pushed at a portal is stopped
            // by the wall it can see straight through.
            //
            // FILTERED PER CONTACT POINT, NOT PER TRIANGLE. The
            // first version tested the triangle's centroid, which
            // is wrong the moment a triangle is bigger than the
            // hole -- and level geometry is made of triangles far
            // bigger than a hole. A wall panel's face is TWO
            // triangles; one centroid falls outside the aperture,
            // so half of that panel stays solid and an object
            // thrown at a three-metre opening bounces off thin
            // air in front of it. The player never saw it because
            // the character controller filters by the closest
            // point on the triangle, which is precise.
            if (!world_->portals().empty()) {
                int kept = 0;
                for (int c = 0; c < m.count; c++) {
                    if (world_->inside_aperture(m.points[c].position, m.normal,
                                                -world_->aperture_edge, &sa))
                        continue;
                    m.points[kept++] = m.points[c];
                }
                m.count = kept;
                if (kept == 0) continue;
            }
            PairKey key{i, wt.collider.index, wt.index};
            Pair &p = pairs_[key];
            // Carry last step's impulses onto the points that
            // survived, matched by feature id. This is the warm
            // start and it is most of why a stack stands up.
            Manifold old = p.manifold;
            const bool fresh = !p.touched && old.count == 0;
            (void)fresh;
            for (int c = 0; c < m.count; c++) {
                for (int o = 0; o < old.count; o++) {
                    if (old.points[o].id != m.points[c].id) continue;
                    m.points[c].normal_impulse = old.points[o].normal_impulse;
                    m.points[c].tangent_impulse[0] =
                        old.points[o].tangent_impulse[0];
                    m.points[c].tangent_impulse[1] =
                        old.points[o].tangent_impulse[1];
                    m.points[c].approach = old.points[o].approach;
                    stats.warm_started++;
                    break;
                }
            }
            p.key = key;
            p.manifold = m;
            p.a = i;
            p.b = kNoBody;
            p.touched = true;
            p.friction = a.friction;
            p.restitution = a.restitution;
            order_.push_back(key);
            stats.manifolds++;
            stats.contacts += uint32_t(m.count);
        }

        // Static and kinematic primitive colliders -- a button, a
        // platform, a door. Meshes were handled above.
        hits.clear();
        world_->overlap(swept, a.mask, hits);
        for (ColliderId cid : hits) {
            const Collider *c = world_->get(cid);
            if (!c || c->shape.type == ShapeType::Mesh || c->is_trigger) continue;
            // A NON-STATIC COLLIDER IS ANOTHER BODY'S PROXY. It is
            // handled by the body-against-body pass below; picking
            // it up here as well would have every dynamic object
            // colliding with an immovable copy of itself.
            if (!c->is_static) continue;
            Manifold m;
            if (!collide(sa, ta, c->shape, c->transform, &m, margin)) continue;
            PairKey key{i, 0x80000000u | cid.index, 0};
            Pair &p = pairs_[key];
            Manifold old = p.manifold;
            for (int k = 0; k < m.count; k++)
                for (int o = 0; o < old.count; o++)
                    if (old.points[o].id == m.points[k].id) {
                        m.points[k].normal_impulse = old.points[o].normal_impulse;
                        m.points[k].tangent_impulse[0] =
                            old.points[o].tangent_impulse[0];
                        m.points[k].tangent_impulse[1] =
                            old.points[o].tangent_impulse[1];
                        m.points[k].approach = old.points[o].approach;
                        stats.warm_started++;
                        break;
                    }
            p.key = key;
            p.manifold = m;
            p.a = i;
            p.b = kNoBody;
            p.touched = true;
            p.friction = a.friction;
            p.restitution = a.restitution;
            order_.push_back(key);
            stats.manifolds++;
            stats.contacts += uint32_t(m.count);
        }

        // Body against body -- see the loop below. Nothing here.
        if (false)
        for (uint32_t j = i + 1; j < slots_.size(); j++) {
            Slot &sj = slots_[j];
            if (!sj.live) continue;
            RigidBody &b = sj.body;
            if (a.sleeping && b.sleeping) continue;
            if (!(a.mask & b.layer) || !(b.mask & a.layer)) continue;
            if (a.inv_mass == 0.0f && b.inv_mass == 0.0f) continue;
            const Shape sb = b.sized();
            const Transform3D tb = b.pose();
            if (!swept.intersects(sb.world_bounds(tb).grown(margin))) continue;
            Manifold m;
            if (!collide(sa, ta, sb, tb, &m, margin)) continue;
            PairKey key{i, j, 0xFFFFFFFFu};
            Pair &p = pairs_[key];
            Manifold old = p.manifold;
            for (int k = 0; k < m.count; k++)
                for (int o = 0; o < old.count; o++)
                    if (old.points[o].id == m.points[k].id) {
                        m.points[k].normal_impulse = old.points[o].normal_impulse;
                        m.points[k].tangent_impulse[0] =
                            old.points[o].tangent_impulse[0];
                        m.points[k].tangent_impulse[1] =
                            old.points[o].tangent_impulse[1];
                        m.points[k].approach = old.points[o].approach;
                        stats.warm_started++;
                        break;
                    }
            p.key = key;
            p.manifold = m;
            p.a = i;
            p.b = j;
            p.touched = true;
            // The geometric mean for friction and the max for
            // restitution: rubber on ice should be slippery and
            // rubber on anything should bounce.
            p.friction = std::sqrt(std::max(0.0f, a.friction * b.friction));
            p.restitution = std::max(a.restitution, b.restitution);
            order_.push_back(key);
            stats.manifolds++;
            stats.contacts += uint32_t(m.count);
            // Something landing on a sleeper wakes it.
            if (b.sleeping && !a.sleeping) b.wake();
            if (a.sleeping && !b.sleeping) a.wake();
        }
    }

    // BODY AGAINST BODY, IN ITS OWN LOOP, and the reason is a bug
    // that looks exactly like broken collision.
    //
    // The static pass above skips sleeping bodies, which is right:
    // a body that is not moving has nothing new to say about the
    // floor. Folding the body-body test into the same loop makes
    // that skip apply to the FIRST index of every pair -- so the
    // moment the bottom box of a stack falls asleep, the pair
    // (bottom, next) stops being generated at all and the box above
    // it drops straight through. One box a second, from the bottom
    // up, and every one of them lands looking like a collision
    // failure rather than a scheduling one.
    //
    // A pair is skipped only when BOTH ends are asleep, which is the
    // only case where nothing can have changed.
    for (uint32_t i = 0; i < slots_.size(); i++) {
        if (!slots_[i].live) continue;
        RigidBody &a = slots_[i].body;
        if (a.kind != BodyKind::Dynamic) continue;
        const Shape sa = a.sized();
        const Transform3D ta = a.pose();
        const float ma = speculative_margin + a.linear_velocity.length() * fixed_step;
        const AABB ba = sa.world_bounds(ta).grown(ma + 0.02f);
        for (uint32_t j = i + 1; j < slots_.size(); j++) {
            if (!slots_[j].live) continue;
            RigidBody &b = slots_[j].body;
            if (a.sleeping && b.sleeping) continue;
            if (!(a.mask & b.layer) || !(b.mask & a.layer)) continue;
            if (a.inv_mass == 0.0f && b.inv_mass == 0.0f) continue;
            const Shape sb = b.sized();
            const Transform3D tb = b.pose();
            const float mb =
                speculative_margin + b.linear_velocity.length() * fixed_step;
            const float margin = std::max(ma, mb);
            if (!ba.intersects(sb.world_bounds(tb).grown(margin + 0.02f)))
                continue;
            Manifold m;
            if (!collide(sa, ta, sb, tb, &m, margin)) continue;
            PairKey key{i, j, 0xFFFFFFFFu};
            Pair &p = pairs_[key];
            const Manifold old = p.manifold;
            for (int k = 0; k < m.count; k++)
                for (int o = 0; o < old.count; o++)
                    if (old.points[o].id == m.points[k].id) {
                        m.points[k].normal_impulse = old.points[o].normal_impulse;
                        m.points[k].tangent_impulse[0] =
                            old.points[o].tangent_impulse[0];
                        m.points[k].tangent_impulse[1] =
                            old.points[o].tangent_impulse[1];
                        m.points[k].approach = old.points[o].approach;
                        stats.warm_started++;
                        break;
                    }
            p.key = key;
            p.manifold = m;
            p.a = i;
            p.b = j;
            p.touched = true;
            // The geometric mean for friction and the max for
            // restitution: rubber on ice should be slippery and
            // rubber on anything should bounce.
            p.friction = std::sqrt(std::max(0.0f, a.friction * b.friction));
            p.restitution = std::max(a.restitution, b.restitution);
            order_.push_back(key);
            stats.manifolds++;
            stats.contacts += uint32_t(m.count);
            if (b.sleeping && !a.sleeping) b.wake();
            if (a.sleeping && !b.sleeping) a.wake();
        }
    }

    for (auto it = pairs_.begin(); it != pairs_.end();) {
        if (!it->second.touched)
            it = pairs_.erase(it);
        else
            ++it;
    }
}

void DynamicsWorld::prepare(float dt) {
    const float inv_dt = dt > 0.0f ? 1.0f / dt : 0.0f;
    // The spring, once per step. Hertz is capped at a quarter of
    // the step rate: a spring stiffer than the solver can resolve
    // is not stiffer, it is unstable.
    const float hz = std::min(contact_hertz, 0.25f * inv_dt);
    const float omega = 2.0f * 3.14159265f * hz;
    const float cterm = dt * omega * (2.0f * contact_damping + dt * omega);
    const float bias_rate = omega / (2.0f * contact_damping + dt * omega);
    const float soft_mass = cterm / (1.0f + cterm);
    const float soft_impulse = 1.0f / (1.0f + cterm);
    for (const PairKey &key : order_) {
        auto it = pairs_.find(key);
        if (it == pairs_.end()) continue;
        Pair &p = it->second;
        RigidBody *a = &slots_[p.a].body;
        RigidBody *b = p.b == kNoBody ? nullptr : &slots_[p.b].body;
        const Vec3 n = p.manifold.normal;
        tangent_basis(n, &p.tangent[0], &p.tangent[1]);

        for (int c = 0; c < p.manifold.count; c++) {
            Contact &ct = p.manifold.points[c];
            p.ra[c] = ct.position - a->position;
            p.rb[c] = b ? ct.position - b->position : Vec3();

            auto effective_mass = [&](const Vec3 &dir) {
                float k = a->inv_mass;
                const Vec3 rna = cross(p.ra[c], dir);
                k += dot(rna, a->inv_inertia_world.xform(rna));
                if (b) {
                    k += b->inv_mass;
                    const Vec3 rnb = cross(p.rb[c], dir);
                    k += dot(rnb, b->inv_inertia_world.xform(rnb));
                }
                return k > 1e-9f ? 1.0f / k : 0.0f;
            };
            p.normal_mass[c] = effective_mass(n);
            p.tangent_mass[c][0] = effective_mass(p.tangent[0]);
            p.tangent_mass[c][1] = effective_mass(p.tangent[1]);

            // THE TARGET NORMAL VELOCITY, and the sign of it is the
            // whole of speculative contact.
            //
            // `sep` positive means the surfaces have not met yet.
            // The body is then ALLOWED TO APPROACH, but only fast
            // enough to close the gap in one step -- so it arrives
            // exactly at the surface and no further. That is what
            // stops a body at 60 m/s from crossing a 5 cm floor
            // between two steps, and it costs nothing when nothing
            // is moving fast.
            //
            // Getting this sign backwards -- a target of +sep/dt
            // rather than -sep/dt -- turns every near-contact into
            // a spring that fires the bodies apart at metres per
            // second. A stack explodes, a box on a slope is thrown
            // off it, and a single box on a floor still looks
            // perfectly fine, because it settles to sep = 0 where
            // the sign does not matter. It is a one-character bug
            // that passes the obvious test.
            //
            // Penetration gets a target of zero: the overlap is
            // taken out by the position pass, which does not feed
            // energy back into the velocities.
            // `sep` is positive when the surfaces have not met.
            const float sep = -ct.depth;
            if (sep > 0.0f) {
                // Speculative: it may approach exactly fast enough
                // to arrive, and no faster. That is what stops a
                // body at 60 m/s from crossing a 5 cm floor between
                // two steps. A hard constraint -- there is no
                // overlap to be soft about.
                p.bias[c] = sep * inv_dt;
                p.mass_scale[c] = 1.0f;
                p.impulse_scale[c] = 0.0f;
            } else {
                // Penetrating: the spring pushes out, beyond the
                // slop only, and capped so a body spawned inside a
                // wall walks out rather than being fired across
                // the room.
                p.bias[c] = std::max(bias_rate * (sep + slop),
                                     -max_bias_velocity);
                p.mass_scale[c] = soft_mass;
                p.impulse_scale[c] = soft_impulse;
            }

            // Restitution, off the remembered approach speed. See
            // Contact::approach -- measuring it here, at the moment
            // the surfaces meet, finds nothing to reflect because
            // the speculative constraint has already taken it out.
            //
            // Only above a threshold, or a resting body bounces on
            // its own settling velocity and never sleeps.
            Vec3 rel = a->linear_velocity + cross(a->angular_velocity, p.ra[c]);
            if (b) rel -= b->linear_velocity + cross(b->angular_velocity, p.rb[c]);
            const float vn = dot(rel, n);
            if (vn < ct.approach) ct.approach = vn;
            // NOT USED BY THE MAIN SOLVE. See apply_restitution --
            // mixing a bounce into the same constraint as the
            // speculative bias produces a body that hovers: the
            // one term wants it to fall the remaining gap and the
            // other wants it to leave, and they balance a
            // centimetre or two off the ground, where it then goes
            // to sleep.
            p.restitution_bias[c] = 0.0f;
        }
    }
}

void DynamicsWorld::warm_start() {
    for (const PairKey &key : order_) {
        auto it = pairs_.find(key);
        if (it == pairs_.end()) continue;
        Pair &p = it->second;
        RigidBody *a = &slots_[p.a].body;
        RigidBody *b = p.b == kNoBody ? nullptr : &slots_[p.b].body;
        const Vec3 n = p.manifold.normal;
        for (int c = 0; c < p.manifold.count; c++) {
            const Contact &ct = p.manifold.points[c];
            const Vec3 j = n * ct.normal_impulse +
                           p.tangent[0] * ct.tangent_impulse[0] +
                           p.tangent[1] * ct.tangent_impulse[1];
            a->linear_velocity += j * a->inv_mass;
            a->angular_velocity += a->inv_inertia_world.xform(cross(p.ra[c], j));
            if (b) {
                b->linear_velocity -= j * b->inv_mass;
                b->angular_velocity -=
                    b->inv_inertia_world.xform(cross(p.rb[c], j));
            }
        }
    }
}

void DynamicsWorld::solve_velocities(int iterations, bool use_bias) {
    for (int iter = 0; iter < iterations; iter++) {
        for (const PairKey &key : order_) {
            auto it = pairs_.find(key);
            if (it == pairs_.end()) continue;
            Pair &p = it->second;
            RigidBody *a = &slots_[p.a].body;
            RigidBody *b = p.b == kNoBody ? nullptr : &slots_[p.b].body;
            const Vec3 n = p.manifold.normal;

            // FRICTION FIRST, against last iteration's normal
            // impulse. Solving the normal first and friction after
            // means friction is always one iteration behind the
            // force it is bounded by, which shows up as a box
            // creeping down a slope it should hold on.
            for (int c = 0; c < p.manifold.count; c++) {
                Contact &ct = p.manifold.points[c];
                const float bound = p.friction * ct.normal_impulse;
                for (int k = 0; k < 2; k++) {
                    Vec3 rel =
                        a->linear_velocity + cross(a->angular_velocity, p.ra[c]);
                    if (b)
                        rel -= b->linear_velocity +
                               cross(b->angular_velocity, p.rb[c]);
                    const float vt = dot(rel, p.tangent[k]);
                    float lambda = -vt * p.tangent_mass[c][k];
                    const float old = ct.tangent_impulse[k];
                    ct.tangent_impulse[k] =
                        clampf(old + lambda, -bound, bound);
                    lambda = ct.tangent_impulse[k] - old;
                    const Vec3 j = p.tangent[k] * lambda;
                    a->linear_velocity += j * a->inv_mass;
                    a->angular_velocity +=
                        a->inv_inertia_world.xform(cross(p.ra[c], j));
                    if (b) {
                        b->linear_velocity -= j * b->inv_mass;
                        b->angular_velocity -=
                            b->inv_inertia_world.xform(cross(p.rb[c], j));
                    }
                }
            }

            for (int c = 0; c < p.manifold.count; c++) {
                Contact &ct = p.manifold.points[c];
                Vec3 rel =
                    a->linear_velocity + cross(a->angular_velocity, p.ra[c]);
                if (b)
                    rel -= b->linear_velocity + cross(b->angular_velocity, p.rb[c]);
                const float vn = dot(rel, n);
                const float bias = use_bias ? p.bias[c] : 0.0f;
                const float ms = use_bias ? p.mass_scale[c] : 1.0f;
                const float is = use_bias ? p.impulse_scale[c] : 0.0f;
                float lambda = -p.normal_mass[c] * ms * (vn + bias) -
                               is * ct.normal_impulse;
                // ACCUMULATED AND CLAMPED, not clamped per
                // iteration. A contact may pull in one iteration as
                // long as the total it has applied stays positive;
                // clamping the increment instead makes the solver
                // unable to correct an overshoot and a stack
                // vibrates.
                const float old = ct.normal_impulse;
                ct.normal_impulse = std::max(0.0f, old + lambda);
                lambda = ct.normal_impulse - old;
                const Vec3 j = n * lambda;
                a->linear_velocity += j * a->inv_mass;
                a->angular_velocity +=
                    a->inv_inertia_world.xform(cross(p.ra[c], j));
                if (b) {
                    b->linear_velocity -= j * b->inv_mass;
                    b->angular_velocity -=
                        b->inv_inertia_world.xform(cross(p.rb[c], j));
                }
            }
        }
    }
}

void DynamicsWorld::apply_restitution() {
    // A SEPARATE RELAXATION, AFTER THE CONTACTS ARE SOLVED.
    //
    // Restitution and speculative contacts are in direct conflict.
    // The speculative constraint exists to remove exactly the
    // approach velocity that restitution wants to reflect, so a
    // bounce computed at the moment of touching finds nothing left
    // and a ball dropped from any height lands dead.
    //
    // The answer is the one Box2D arrived at: remember how fast the
    // contact was closing while it was still a gap, solve the
    // contacts normally, and then do one more pass that asks for a
    // separating velocity of e times that remembered speed. It runs
    // against the same accumulated impulse, so it can only ever add
    // push, never pull.
    //
    // Only contacts that actually carried force this step, because a
    // speculative contact that never closed did not happen.
    for (const PairKey &key : order_) {
        auto it = pairs_.find(key);
        if (it == pairs_.end()) continue;
        Pair &p = it->second;
        if (p.restitution <= 0.0f) continue;
        RigidBody *a = &slots_[p.a].body;
        RigidBody *b = p.b == kNoBody ? nullptr : &slots_[p.b].body;
        const Vec3 n = p.manifold.normal;
        for (int c = 0; c < p.manifold.count; c++) {
            Contact &ct = p.manifold.points[c];
            if (ct.approach > -1.0f || ct.normal_impulse <= 0.0f) continue;
            Vec3 rel = a->linear_velocity + cross(a->angular_velocity, p.ra[c]);
            if (b) rel -= b->linear_velocity + cross(b->angular_velocity, p.rb[c]);
            const float vn = dot(rel, n);
            float lambda =
                -p.normal_mass[c] * (vn + p.restitution * ct.approach);
            const float old = ct.normal_impulse;
            ct.normal_impulse = std::max(0.0f, old + lambda);
            lambda = ct.normal_impulse - old;
            const Vec3 j = n * lambda;
            a->linear_velocity += j * a->inv_mass;
            a->angular_velocity += a->inv_inertia_world.xform(cross(p.ra[c], j));
            if (b) {
                b->linear_velocity -= j * b->inv_mass;
                b->angular_velocity -=
                    b->inv_inertia_world.xform(cross(p.rb[c], j));
            }
            // SPENT. Kept, and the ball bounces once a step for
            // ever off a speed it had two seconds ago.
            ct.approach = 0.0f;
        }
    }
}

void DynamicsWorld::integrate_positions(float dt) {
    for (Slot &s : slots_) {
        if (!s.live) continue;
        RigidBody &b = s.body;
        if (b.kind != BodyKind::Dynamic || b.sleeping) continue;
        b.prev_position = b.position;
        b.position += b.linear_velocity * dt;
        b.orientation = integrate_spin(b.orientation, b.angular_velocity, dt);
    }
}

// ONE CORRECTION SCHEME, NOT TWO.
//
// There was a separate pseudo-position pass here. It is gone, and
// the reason is worth leaving where the next person to reach for
// one will find it.
//
// A position pass and a speculative bias FIGHT. The position pass
// pushes a resting box out until it is a slop clear of the floor;
// the speculative bias then reads that clearance as a gap and
// permits the box to fall back into it at exactly gap / dt. The box
// does not move -- the two cancel in position -- but its velocity
// never reaches zero, so it never sleeps, and a stack of five sits
// humming at a sixth of a metre per second for ever. Every height
// is correct, which is what makes it hard to see.
//
// What replaced it is the soft contact in prepare(): overlap is
// corrected by a spring inside the velocity solve, and the energy
// that spring adds is taken straight back out by the relax pass.
// One mechanism, one place to tune, and a stack that goes to sleep.

void DynamicsWorld::cross_portals() {
    if (!world_ || world_->portals().empty()) return;
    for (Slot &s : slots_) {
        if (!s.live) continue;
        RigidBody &b = s.body;
        if (b.kind != BodyKind::Dynamic) continue;
        // The centre's path over this step. A body is through when
        // its CENTRE is, which is the same rule the renderer and the
        // character controller use -- three different rules for when
        // something has gone through a door is three chances for the
        // picture and the physics to disagree.
        float t = 0.0f;
        Portal3D *p = world_->crossing(b.prev_position, b.position, &t);
        if (!p || !p->link()) continue;
        // Same gate as the character's. A crate too big for the
        // hole bounces off the wall it is cut into.
        if (!PhysicsWorld::admits_shape(*p, b.sized())) continue;
        const Transform3D warp = Portal3D::warp(p, p->link());
        const float ratio = warp.basis.uniform_scale();

        b.position = warp.xform(b.position);
        b.orientation =
            (warp.basis.orthonormalized().to_quat() * b.orientation).normalized();
        // ROTATED, NOT JUST MOVED, and scaled by the size ratio.
        //
        // "Speedy thing goes in, speedy thing comes out" is a
        // statement about the velocity being carried through the
        // basis change, and if the far end is twice the size the
        // thing that comes out is twice as big and travelling twice
        // as fast -- otherwise a big portal is a brake.
        // SPEED, OR SELF-SIMILARITY. See portals_preserve_speed --
        // the warp's basis carries the size ratio, so putting the
        // velocity through it scales the speed and putting it
        // through the rotation alone does not.
        b.linear_velocity =
            portals_preserve_speed
                ? warp.basis.orthonormalized().xform(b.linear_velocity)
                : warp.basis.xform(b.linear_velocity);
        // THE SKATER. Angular velocity is a pseudovector, so it
        // takes the rotation and not the scale; the 1/k^2 is
        // conservation of angular momentum for a body whose mass
        // does not change with its size.
        Vec3 spin = warp.basis.orthonormalized().xform(b.angular_velocity);
        if (ratio > 1e-6f)
            spin = spin * std::pow(1.0f / ratio, spin_conservation);
        const float sl = spin.length();
        if (sl > max_spin) spin = spin * (max_spin / sl);
        b.angular_velocity = spin;
        b.scale *= ratio;
        // Mass follows the volume, so a small crate is a light
        // crate. This is the half of "denser and smaller" that is
        // given up, deliberately: keeping the mass makes a shrunk
        // object immovable and there is nothing to do with it.
        b.mass = b.mass * ratio * ratio * ratio;
        b.warped = true;
        b.crossings++;
        b.last_warp = warp;
        b.warp_from = p;
        b.warp_to = p->link();
        // SO THE NEXT STEP'S SEGMENT STARTS WHERE THE BODY IS. A
        // previous position left on the far side of the map is a
        // segment that crosses half the level, and it will find an
        // aperture somewhere along it.
        b.prev_position = b.position;
        b.wake();
        // The inertia is a function of the size, so it has to be
        // rebuilt or a cube that grew keeps the spin of the cube it
        // was.
        refresh_mass(BodyId{uint32_t(&s - slots_.data()), s.generation});
        // AND EVERY CONTACT IT HAD IS MEANINGLESS NOW. Warm starting
        // from the impulses it was applying on the other side of the
        // map is how a cube comes out of a portal and is flung.
        for (auto it = pairs_.begin(); it != pairs_.end();) {
            const uint32_t idx = uint32_t(&s - slots_.data());
            if (it->second.a == idx || it->second.b == idx)
                it = pairs_.erase(it);
            else
                ++it;
        }
        stats.portal_crossings++;
    }
}

void DynamicsWorld::sync_proxies() {
    if (!world_) return;
    for (Slot &s : slots_) {
        if (!s.live || !s.body.proxy.valid()) continue;
        Collider *c = world_->get(s.body.proxy);
        if (!c) continue;
        c->transform = s.body.pose();
        // The SIZE too, because portals change it and a proxy at
        // the old size is a crate you can shoot where it was.
        c->shape = s.body.sized();
        c->layer = s.body.layer;
        world_->set_transform(s.body.proxy, c->transform);
    }
}

void DynamicsWorld::settle(float dt) {
    for (Slot &s : slots_) {
        if (!s.live) continue;
        RigidBody &b = s.body;
        if (b.kind != BodyKind::Dynamic || b.sleeping) continue;
        const bool slow = b.linear_velocity.length() < sleep_linear &&
                          b.angular_velocity.length() < sleep_angular;
        if (!slow) {
            b.sleep_timer = 0.0f;
            continue;
        }
        b.sleep_timer += dt;
        if (b.sleep_timer >= sleep_after) {
            b.sleeping = true;
            b.linear_velocity = Vec3();
            b.angular_velocity = Vec3();
        }
    }
}

}  // namespace wr
