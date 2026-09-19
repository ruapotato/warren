#include "physics/fluid.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "physics/world.h"
#include "scene/portal.h"

namespace wr {
namespace {

constexpr float kPi = 3.14159265358979f;
// Indices at or above this refer to ghosts. Real particle counts
// are far below it and a fluid that reached it would have run out
// of memory long before.
constexpr uint32_t kGhostBase = 0x40000000u;

// POLY6 FOR DENSITY, SPIKY FOR THE GRADIENT, and they are different
// kernels on purpose. Poly6 is smooth and cheap and its gradient
// goes to zero as two particles approach -- which means two
// particles on top of each other feel no force to separate, and the
// fluid clumps into pairs. Spiky's gradient does not vanish, so it
// is used for every force and poly6 only for the density sum.
struct Kernel {
    float h, h2, poly6, spiky;
    explicit Kernel(float radius)
        : h(radius),
          h2(radius * radius),
          poly6(315.0f / (64.0f * kPi * std::pow(radius, 9.0f))),
          spiky(-45.0f / (kPi * std::pow(radius, 6.0f))) {}

    float w(float r2) const {
        if (r2 >= h2) return 0.0f;
        const float d = h2 - r2;
        return poly6 * d * d * d;
    }
    Vec3 grad(const Vec3 &d, float r) const {
        if (r >= h || r < 1e-9f) return Vec3();
        const float c = spiky * (h - r) * (h - r) / r;
        return d * c;
    }
};

}  // namespace

// WHAT ONE PARTICLE WEIGHS, and it is not density times volume.
//
// The obvious answer -- rest_density * spacing^3 -- assumes the
// kernel sums to exactly 1/spacing^3 over a lattice at that
// spacing. It does not: the kernel is truncated at the smoothing
// radius, so the sum depends on how many lattice points fall
// inside it, which depends on the ratio of the two radii. At three
// particle-radii of smoothing the error is five per cent, and five
// per cent of over-density is a fluid that quietly compresses
// until the solver's pushback balances it, settling a column a
// tenth shorter than it should be with nothing obviously wrong.
//
// So the mass is derived from the kernel: sum the kernel over a
// lattice at the rest spacing, and pick the mass that makes that
// sum come out at the rest density exactly. Self-consistent for
// any ratio of radii, which means the two can be tuned for looks
// without silently changing what the liquid weighs.
float Fluid::particle_mass() const {
    const float s = particle_radius * 2.0f;
    const float h = material.smoothing_radius;
    const Kernel k(h);
    const int reach = int(std::ceil(h / std::max(1e-4f, s)));
    float sum = 0.0f;
    for (int x = -reach; x <= reach; x++)
        for (int y = -reach; y <= reach; y++)
            for (int z = -reach; z <= reach; z++) {
                const float r2 = float(x * x + y * y + z * z) * s * s;
                sum += k.w(r2);
            }
    if (sum < 1e-9f) return material.rest_density * s * s * s;
    return material.rest_density / sum;
}

// ---------------------------------------------------------- emitting

uint32_t Fluid::emit(const Vec3 &at, const Vec3 &velocity) {
    if (pos_.size() >= max_particles) return 0xFFFFFFFFu;
    pos_.push_back(at);
    vel_.push_back(velocity);
    predicted_.push_back(at);
    prev_.push_back(at);
    contact_.emplace_back();
    pushed_.emplace_back();
    delta_.emplace_back();
    lambda_.push_back(0.0f);
    denom_.push_back(1.0f);
    density_.push_back(0.0f);
    vorticity_.emplace_back();
    return uint32_t(pos_.size() - 1);
}

int Fluid::fill(const AABB &box, float spacing) {
    // A LATTICE AT THE REST SPACING, not a random scatter. Random
    // placement starts the solver with every particle either
    // overlapping or isolated, and the first half second is spent
    // on an explosion that has nothing to do with the scene.
    if (spacing <= 0.0f) spacing = particle_radius * 2.0f;
    int made = 0;
    const Vec3 size = box.max - box.min;
    const int nx = std::max(1, int(size.x / spacing));
    const int ny = std::max(1, int(size.y / spacing));
    const int nz = std::max(1, int(size.z / spacing));
    for (int x = 0; x < nx; x++)
        for (int y = 0; y < ny; y++)
            for (int z = 0; z < nz; z++) {
                // Half a spacing in from the wall, and staggered by
                // row, which packs closer to the real rest state
                // than a cubic lattice does.
                const float ox = (y % 2) ? spacing * 0.25f : 0.0f;
                const Vec3 p = box.min + Vec3(spacing * (float(x) + 0.5f) + ox,
                                              spacing * (float(y) + 0.5f),
                                              spacing * (float(z) + 0.5f));
                if (emit(p) == 0xFFFFFFFFu) return made;
                made++;
            }
    return made;
}

void Fluid::clear() {
    pos_.clear();
    vel_.clear();
    predicted_.clear();
    prev_.clear();
    contact_.clear();
    pushed_.clear();
    delta_.clear();
    lambda_.clear();
    denom_.clear();
    density_.clear();
    vorticity_.clear();
}

// ------------------------------------------------------------- grid

int64_t Fluid::cell_of(const Vec3 &p) const {
    const float inv = 1.0f / material.smoothing_radius;
    const int64_t x = int64_t(std::floor(p.x * inv));
    const int64_t y = int64_t(std::floor(p.y * inv));
    const int64_t z = int64_t(std::floor(p.z * inv));
    // Three 21-bit fields. A world 2^21 cells across at 12 cm is
    // 250 km, which is not a limit anybody will meet.
    return ((x & 0x1FFFFF) << 42) | ((y & 0x1FFFFF) << 21) | (z & 0x1FFFFF);
}

void Fluid::build_grid() {
    const size_t n = predicted_.size() + ghost_pos_.size();
    keys_.resize(n);
    order_.resize(n);
    for (size_t i = 0; i < predicted_.size(); i++) {
        keys_[i] = cell_of(predicted_[i]);
        order_[i] = uint32_t(i);
    }
    for (size_t g = 0; g < ghost_pos_.size(); g++) {
        const size_t i = predicted_.size() + g;
        keys_[i] = cell_of(ghost_pos_[g]);
        order_[i] = uint32_t(i);
    }
    std::sort(order_.begin(), order_.end(),
              [&](uint32_t a, uint32_t b) { return keys_[a] < keys_[b]; });

    cell_key_.clear();
    cell_start_.clear();
    cell_count_.clear();
    for (size_t i = 0; i < order_.size();) {
        const int64_t k = keys_[order_[i]];
        size_t j = i;
        while (j < order_.size() && keys_[order_[j]] == k) j++;
        cell_key_.push_back(k);
        cell_start_.push_back(uint32_t(i));
        cell_count_.push_back(uint32_t(j - i));
        i = j;
    }
    build_index();
}

void Fluid::build_index() {
    uint32_t cap = 16;
    while (cap < cell_key_.size() * 2) cap <<= 1;
    probe_mask_ = cap - 1;
    probe_key_.assign(cap, INT64_MIN);
    probe_slot_.assign(cap, 0xFFFFFFFFu);
    for (uint32_t i = 0; i < uint32_t(cell_key_.size()); i++) {
        const int64_t k = cell_key_[i];
        uint32_t h = uint32_t((uint64_t(k) * 0x9E3779B97F4A7C15ull) >> 40) &
                     probe_mask_;
        while (probe_slot_[h] != 0xFFFFFFFFu) h = (h + 1) & probe_mask_;
        probe_key_[h] = k;
        probe_slot_[h] = i;
    }
}

uint32_t Fluid::find_cell(int64_t key) const {
    if (probe_key_.empty()) return 0xFFFFFFFFu;
    uint32_t h = uint32_t((uint64_t(key) * 0x9E3779B97F4A7C15ull) >> 40) &
                 probe_mask_;
    while (probe_slot_[h] != 0xFFFFFFFFu) {
        if (probe_key_[h] == key) return probe_slot_[h];
        h = (h + 1) & probe_mask_;
    }
    return 0xFFFFFFFFu;
}

void Fluid::find_neighbours() {
    const float h = material.smoothing_radius;
    const float h2 = h * h;
    const size_t n = predicted_.size();
    nbr_.clear();
    nbr_start_.assign(n, 0);
    nbr_count_.assign(n, 0);
    const float inv = 1.0f / h;

    for (size_t i = 0; i < n; i++) {
        nbr_start_[i] = uint32_t(nbr_.size());
        const Vec3 &pi = predicted_[i];
        const int64_t bx = int64_t(std::floor(pi.x * inv));
        const int64_t by = int64_t(std::floor(pi.y * inv));
        const int64_t bz = int64_t(std::floor(pi.z * inv));
        for (int dx = -1; dx <= 1; dx++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dz = -1; dz <= 1; dz++) {
                    const int64_t k = (((bx + dx) & 0x1FFFFF) << 42) |
                                      (((by + dy) & 0x1FFFFF) << 21) |
                                      ((bz + dz) & 0x1FFFFF);
                    const uint32_t c = find_cell(k);
                    if (c == 0xFFFFFFFFu) continue;
                    for (uint32_t s = 0; s < cell_count_[c]; s++) {
                        const uint32_t j = order_[cell_start_[c] + s];
                        if (j == i) continue;
                        const Vec3 &pj = j < predicted_.size()
                                             ? predicted_[j]
                                             : ghost_pos_[j - predicted_.size()];
                        if ((pj - pi).length_sq() >= h2) continue;
                        // Ghosts are tagged on the way in so the
                        // solver can tell "somebody else's problem"
                        // from "mine".
                        nbr_.push_back(j < predicted_.size()
                                           ? j
                                           : kGhostBase + (j - uint32_t(predicted_.size())));
                    }
                }
        nbr_count_[i] = uint32_t(nbr_.size()) - nbr_start_[i];
    }
    stats.neighbours = uint32_t(nbr_.size());
}

// ----------------------------------------------------------- ghosts

void Fluid::build_ghosts() {
    ghost_pos_.clear();
    ghost_lambda_.clear();
    ghost_source_.clear();
    if (!world_ || !portal_ghosts) return;
    const auto &portals = world_->portals();
    if (portals.empty()) return;

    const float h = material.smoothing_radius;
    for (Portal3D *p : portals) {
        Portal3D *q = p->link();
        if (!p->active || !q || !q->active) continue;
        // WHAT IS BEHIND P IS WHAT IS IN FRONT OF Q. A particle
        // about to go through P should feel the liquid already on
        // the other side, and that liquid is the liquid standing at
        // Q's mouth.
        const Transform3D warp = Portal3D::warp(q, p);
        const Plane qp = q->plane();
        for (size_t i = 0; i < predicted_.size(); i++) {
            const Vec3 &x = predicted_[i];
            const float d = qp.distance_to(x);
            // In front of Q, within one smoothing radius of it, and
            // inside the opening. Anything else is not visible
            // through the hole.
            if (d < 0.0f || d > h) continue;
            if (!q->within_aperture(x, h)) continue;
            ghost_pos_.push_back(warp.xform(x));
            ghost_lambda_.push_back(0.0f);
            ghost_source_.push_back(uint32_t(i));
        }
    }
    stats.ghosts = uint32_t(ghost_pos_.size());
}

// ------------------------------------------------------ the solver

void Fluid::predict(float dt) {
    prev_.assign(pos_.begin(), pos_.end());
    contact_.assign(pos_.size(), Vec3());
    pushed_.assign(pos_.size(), Vec3());
    // NO PARTICLE MAY CROSS MORE THAN PART OF A SMOOTHING RADIUS
    // IN ONE STEP.
    //
    // Not a safety net -- a condition of the method. Every
    // neighbourhood is found once per step, so a particle that
    // travels further than the radius it sees has moved into a
    // region it has no information about: its density is computed
    // against neighbours it has already left. The solver then
    // corrects the wrong thing and the error compounds. Half a
    // radius keeps it inside the neighbourhood it was measured in,
    // and the only visible cost is that a very fast splash is a
    // little slower than ballistic.
    const float vmax = dt > 0.0f ? 0.5f * material.smoothing_radius / dt : 0.0f;
    for (size_t i = 0; i < pos_.size(); i++) {
        vel_[i] += gravity * dt;
        const float sp = vel_[i].length();
        if (vmax > 0.0f && sp > vmax) vel_[i] = vel_[i] * (vmax / sp);
        predicted_[i] = pos_[i] + vel_[i] * dt;
    }
}

void Fluid::solve_density() {
    const Kernel k(material.smoothing_radius);
    const float rest = material.rest_density;
    const float inv_rest = 1.0f / rest;
    const size_t n = predicted_.size();
    const float spacing = particle_radius * 2.0f;
    const float mass = particle_mass();

    // The kernel value at the rest spacing, for the artificial
    // pressure term below.
    const float w_rest = k.w(spacing * 0.3f * spacing * 0.3f);

    stats.worst_compression = 0.0f;
    for (int iter = 0; iter < solver_iterations; iter++) {
        // --- densities and lambdas
        for (size_t i = 0; i < n; i++) {
            float rho = k.w(0.0f) * mass;
            Vec3 grad_sum;
            float grad_sq = 0.0f;
            const uint32_t s = nbr_start_[i], c = nbr_count_[i];
            for (uint32_t t = 0; t < c; t++) {
                const uint32_t raw = nbr_[s + t];
                const bool ghost = raw >= kGhostBase;
                const Vec3 &pj = ghost ? ghost_pos_[raw - kGhostBase]
                                       : predicted_[raw];
                const Vec3 d = predicted_[i] - pj;
                const float r2 = d.length_sq();
                rho += k.w(r2) * mass;
                const Vec3 g = k.grad(d, std::sqrt(r2)) * (mass * inv_rest);
                grad_sum += g;
                grad_sq += g.length_sq();
            }
            density_[i] = rho;
            float c_i = rho * inv_rest - 1.0f;
            if (iter == 0)
                stats.worst_compression =
                    std::max(stats.worst_compression, c_i);
            // COMPRESSION ONLY. NEVER PULL.
            //
            // A particle at a free surface has half a
            // neighbourhood and reads as under-dense by definition
            // -- a lone droplet reads as 50% under-dense. Letting
            // that produce a negative constraint means the solver
            // tries to fix it by dragging neighbours in, and for a
            // particle with no neighbours at all the denominator
            // is zero too: lambda comes out in the hundreds and
            // the particle leaves the level in one step. It is the
            // single most common way a position-based fluid
            // explodes, and it explodes from the surface inward,
            // so the first frame still looks fine.
            //
            // Clamping to zero makes the constraint one-sided:
            // liquid resists being squeezed and does not resist
            // being pulled apart. Cohesion is the surface term's
            // job, below, where it can be bounded.
            if (c_i < 0.0f) c_i = 0.0f;
            grad_sq += grad_sum.length_sq();
            // AND THE EPSILON IS NOT SMALL. It is a constraint-
            // force-mixing term and it has to be a real fraction of
            // the gradient sum -- the paper's 600 against a sum in
            // the hundred thousands, so under a per cent -- or a
            // particle with two neighbours still divides by nearly
            // nothing.
            const float denom = grad_sq + material.relaxation;
            denom_[i] = denom;
            lambda_[i] = -c_i / denom;
        }
        // Ghosts borrow their source's lambda. They are the same
        // liquid seen through a hole, so they must push back with
        // the same force or the stream is pushed out of the portal.
        for (size_t g = 0; g < ghost_pos_.size(); g++)
            ghost_lambda_[g] = lambda_[ghost_source_[g]];

        // --- position corrections
        for (size_t i = 0; i < n; i++) {
            Vec3 d_pos;
            const uint32_t s = nbr_start_[i], c = nbr_count_[i];
            for (uint32_t t = 0; t < c; t++) {
                const uint32_t raw = nbr_[s + t];
                const bool ghost = raw >= kGhostBase;
                const Vec3 &pj = ghost ? ghost_pos_[raw - kGhostBase]
                                       : predicted_[raw];
                const float lj = ghost ? ghost_lambda_[raw - kGhostBase]
                                       : lambda_[raw];
                const Vec3 d = predicted_[i] - pj;
                const float r = d.length();
                // The tensile-instability term: a small extra push
                // that is strong only at very close range. Without
                // it a free surface beads.
                // SCALED TO LAMBDA, not an absolute number.
                //
                // Lambda is a constraint violation divided by the
                // sum of squared gradients, which for real units
                // and a 10 cm smoothing radius is of order 1e-7.
                // An artificial pressure written as a bare
                // constant sits at 1e-4 and is a THOUSAND times
                // the term it is supposed to nudge -- so the
                // "small cohesion fix" becomes the entire
                // simulation and the fluid detonates. Dividing by
                // the same denominator lambda uses puts the two on
                // the same scale and makes the coefficient mean
                // what the paper says it means.
                float scorr = 0.0f;
                if (w_rest > 1e-20f) {
                    const float ratio = k.w(r * r) / w_rest;
                    scorr = -material.surface_pressure *
                            std::pow(ratio, material.surface_power) / denom_[i];
                }
                d_pos += k.grad(d, r) * (lambda_[i] + lj + scorr);
            }
            delta_[i] = d_pos * (mass * inv_rest);
        }
        for (size_t i = 0; i < n; i++) predicted_[i] += delta_[i];
        collide_world();
    }

    // The interior, measured after the solve rather than during
    // it. A particle with most of a neighbourhood is one whose
    // density means something.
    double sum = 0.0;
    uint32_t seen = 0;
    const uint32_t full = uint32_t(std::max(8, int(nbr_.size() / std::max<size_t>(1, n)) ));
    for (size_t i = 0; i < n; i++) {
        if (nbr_count_[i] < full) continue;
        sum += double(density_[i]);
        seen++;
    }
    stats.interior_count = seen;
    stats.interior_density = seen ? float(sum / double(seen)) / rest : 0.0f;
}

void Fluid::collide_world() {
    if (!world_) return;
    const float r = particle_radius;
    for (size_t i = 0; i < predicted_.size(); i++) {
        // FROM WHERE IT WAS, TO WHERE IT WANTS TO BE -- a ray, not
        // a depenetration.
        //
        // Pushing a particle out after it is already inside is what
        // the character controller does and it is exactly wrong
        // here. `depenetrate` leaves by the NEAREST face, so a
        // particle one millimetre under a floor is helpfully shown
        // the rest of the way through it; and a particle fully
        // inside a twenty-centimetre slab is not reported as
        // touching anything at all. Both are right for a capsule
        // that can never be more than slightly embedded and both
        // sink an entire body of water through a solid floor in
        // about a second, which is how this was found.
        //
        // A particle is a point with a radius and its previous
        // position is always outside, so the honest test is the
        // segment it travelled. It cannot tunnel, it cannot pick
        // the wrong side, and it costs one ray.
        const Vec3 from = prev_[i];
        Vec3 to = predicted_[i];
        Vec3 d = to - from;
        const float len = d.length();
        if (len > 1e-7f) {
            // THE RAY STARTS WHERE THE PARTICLE WAS, and reaches a
            // radius PAST where it wants to be. Not a radius
            // before where it was.
            //
            // Backing the origin off along the path is the obvious
            // way to catch a particle already resting on a
            // surface, and it puts the origin inside that surface
            // -- so the ray misses the near face it is standing
            // on, travels through the slab, and reports the far
            // one. The particle is then placed a radius outside
            // the BOTTOM of the floor it was resting on, where it
            // sits for ever at a height that is arithmetically
            // exact and physically nonsense.
            //
            // Extending the far end does the same job from the
            // outside: the surface is found a radius early and the
            // particle is set down on it with its own radius of
            // clearance.
            const Vec3 dir = d * (1.0f / len);
            RayHit h = world_->raycast(from, to + dir * r, 0xFFFFFFFF);
            // And only a surface it is moving INTO. The normal is
            // reported facing the ray, so a back face and a front
            // face look the same; the direction of travel does
            // not.
            if (h.hit && dot(h.normal, dir) > 0.0f) h.hit = false;
            // A HIT INSIDE AN OPEN APERTURE IS NOT A HIT. The level
            // has no hole cut in it where a portal is, so without
            // this the liquid piles up on the wall it is supposed
            // to be pouring through.
            if (h.hit && !world_->inside_aperture(h.position, h.normal,
                                                  -world_->aperture_edge)) {
                // NOT counted as a push. A particle that was
                // going to travel five centimetres into a wall
                // and instead stopped one centimetre in front of
                // it really is moving at one centimetre a step --
                // that is what being stopped means. Only a
                // correction that moves a particle OUT of
                // somewhere it should never have been is spurious,
                // and those are below.
                predicted_[i] = h.position + h.normal * r;
                contact_[i] = h.normal;
                continue;
            }
        }
        // Resting, or barely moving: the depenetration is right
        // here, because the particle is at most slightly embedded
        // and the nearest face is the one it came in by.
        Vec3 correction, normal;
        Transform3D at;
        at.origin = predicted_[i];
        const int hits = world_->depenetrate(Shape::sphere(r), at, 0xFFFFFFFF,
                                             nullptr, &correction, &normal);
        // AND A CORRECTION BIGGER THAN THE PARTICLE IS A LIE.
        //
        // `depenetrate` leaves by the nearest face. For a particle
        // that has ended up well inside a thick slab the nearest
        // face is whichever it is closest to, which is as likely
        // to be the far side as the near one -- so the fix for
        // being slightly buried is to be shown the rest of the way
        // through. Anything deeper than the particle's own
        // diameter is not a graze, and the only position known to
        // be good is the one it started the step at.
        if (hits > 0 && correction.length() <= r * 2.0f) {
            predicted_[i] += correction;
            pushed_[i] += correction;
            contact_[i] = normal;
        } else if (hits > 0) {
            pushed_[i] += prev_[i] - predicted_[i];
            predicted_[i] = prev_[i];
            contact_[i] = normal;
        }
    }
}

void Fluid::finish(float dt) {
    const float inv_dt = dt > 0.0f ? 1.0f / dt : 0.0f;
    const Kernel k(material.smoothing_radius);
    const size_t n = pos_.size();
    const float mass = particle_mass();

    // MINUS WHAT THE WALLS DID. A correction that takes a
    // particle out of a surface is not motion, and turning it into
    // velocity is how one particle ends up pinned at the speed
    // limit in an otherwise still tank: it is pushed a centimetre
    // out of a corner, reads that as sixty centimetres a second,
    // flies, is caught, is pushed out again. Over a whole pool it
    // is a low simmer that never settles.
    for (size_t i = 0; i < n; i++)
        vel_[i] = (predicted_[i] - pos_[i] - pushed_[i]) * inv_dt;

    // WHAT THE WALL DID TO THE VELOCITY. Deriving it from the
    // positions alone gives a particle that was stopped by a floor
    // a velocity of zero in every direction, so a splash that
    // should run along the ground stops dead at it. Splitting the
    // velocity at the recorded normal keeps the sliding part and
    // takes only the part that went into the wall.
    for (size_t i = 0; i < n; i++) {
        const Vec3 &nrm = contact_[i];
        if (nrm.length_sq() < 1e-8f) continue;
        const float into = dot(vel_[i], nrm);
        if (into >= 0.0f) continue;
        const Vec3 tangent = vel_[i] - nrm * into;
        vel_[i] = tangent * (1.0f - material.friction) -
                  nrm * (into * material.restitution);
    }

    // --- vorticity: put the swirl back
    //
    // AND IT IS AN SPH SUM, WHICH MEANS m/rho PER TERM. Leaving
    // that factor out was the whole of a blowup that looked like a
    // solver bug: the spiky gradient in real units is of order
    // 1e5, so an unweighted curl comes out in the hundred
    // thousands, the confinement force is thousands of metres per
    // second squared, and the fluid detonates on the third step
    // after two perfectly reasonable-looking ones. The density
    // solve was innocent -- it is a feedback loop through
    // velocity, so it needs a few steps to become visible.
    if (material.vorticity > 0.0f) {
        for (size_t i = 0; i < n; i++) {
            Vec3 w;
            const uint32_t s = nbr_start_[i], c = nbr_count_[i];
            for (uint32_t t = 0; t < c; t++) {
                const uint32_t raw = nbr_[s + t];
                if (raw >= kGhostBase) continue;   // no velocity of its own
                const Vec3 d = predicted_[i] - predicted_[raw];
                const float wj = mass / std::max(1.0f, density_[raw]);
                w += cross(vel_[raw] - vel_[i], k.grad(d, d.length())) * wj;
            }
            vorticity_[i] = w;
        }
        for (size_t i = 0; i < n; i++) {
            // The gradient of |vorticity|, which points at the
            // centre of the eddy; the force is across it.
            Vec3 grad;
            const uint32_t s = nbr_start_[i], c = nbr_count_[i];
            for (uint32_t t = 0; t < c; t++) {
                const uint32_t raw = nbr_[s + t];
                if (raw >= kGhostBase) continue;
                const Vec3 d = predicted_[i] - predicted_[raw];
                const float len = vorticity_[raw].length();
                const float wj = mass / std::max(1.0f, density_[raw]);
                grad += k.grad(d, d.length()) * (len * wj);
            }
            const float g = grad.length();
            if (g < 1e-6f) continue;
            Vec3 push = cross(grad * (1.0f / g), vorticity_[i]) *
                        (material.vorticity * dt);
            // AND A CEILING ON IT. Confinement is a cosmetic term
            // -- it puts back swirl the solver's own damping took
            // out -- and a cosmetic term must never be able to
            // dominate the step. Half a metre per second per step
            // is far more than it should ever want and far less
            // than an instability needs.
            const float pl = push.length();
            if (pl > 0.5f) push = push * (0.5f / pl);
            vel_[i] += push;
        }
    }

    // --- XSPH viscosity: agree with the neighbours
    if (material.viscosity > 0.0f) {
        std::vector<Vec3> smoothed(n);
        for (size_t i = 0; i < n; i++) {
            Vec3 acc;
            const uint32_t s = nbr_start_[i], c = nbr_count_[i];
            for (uint32_t t = 0; t < c; t++) {
                const uint32_t raw = nbr_[s + t];
                if (raw >= kGhostBase) continue;
                const Vec3 d = predicted_[i] - predicted_[raw];
                acc += (vel_[raw] - vel_[i]) * (k.w(d.length_sq()) * mass /
                                                std::max(1.0f, density_[raw]));
            }
            smoothed[i] = acc;
        }
        for (size_t i = 0; i < n; i++)
            vel_[i] += smoothed[i] * material.viscosity;
    }

    for (size_t i = 0; i < n; i++) pos_[i] = predicted_[i];

    // --- what has landed
    const float limit = settle_speed * settle_speed;
    const float inv = 1.0f / std::max(1e-4f, settle_spacing);
    for (size_t i = 0; i < n; i++) {
        if (contact_[i].length_sq() < 1e-8f) continue;
        if (vel_[i].length_sq() > limit) continue;
        const int64_t key =
            ((int64_t(std::floor(pos_[i].x * inv)) & 0x1FFFFF) << 42) |
            ((int64_t(std::floor(pos_[i].y * inv)) & 0x1FFFFF) << 21) |
            (int64_t(std::floor(pos_[i].z * inv)) & 0x1FFFFF);
        const auto it = std::lower_bound(settled_seen_.begin(),
                                         settled_seen_.end(), key);
        if (it != settled_seen_.end() && *it == key) continue;
        settled_seen_.insert(it, key);
        settled_pos_.push_back(pos_[i]);
        settled_nrm_.push_back(contact_[i].normalized());
    }
    // A CEILING ON THE MEMORY OF IT. The set of places liquid has
    // ever settled grows for as long as the level is running, and
    // a hose left on would fill it. Past this the oldest are
    // forgotten, which means a very old patch can be reported
    // again -- harmless, and much better than unbounded.
    if (settled_seen_.size() > 20000) {
        settled_seen_.erase(settled_seen_.begin(),
                            settled_seen_.begin() + 10000);
    }
}

size_t Fluid::drain_settled(std::vector<Vec3> *positions,
                            std::vector<Vec3> *normals) {
    const size_t n = settled_pos_.size();
    if (positions) positions->swap(settled_pos_);
    if (normals) normals->swap(settled_nrm_);
    settled_pos_.clear();
    settled_nrm_.clear();
    return n;
}

// --------------------------------------------------------- sampling

float Fluid::density_at(const Vec3 &p) const {
    if (cell_key_.empty()) return 0.0f;
    const Kernel k(material.smoothing_radius);
    const float mass = particle_mass();
    const float inv = 1.0f / material.smoothing_radius;
    const int64_t bx = int64_t(std::floor(p.x * inv));
    const int64_t by = int64_t(std::floor(p.y * inv));
    const int64_t bz = int64_t(std::floor(p.z * inv));
    float rho = 0.0f;
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dz = -1; dz <= 1; dz++) {
                const int64_t key = (((bx + dx) & 0x1FFFFF) << 42) |
                                    (((by + dy) & 0x1FFFFF) << 21) |
                                    ((bz + dz) & 0x1FFFFF);
                const uint32_t c = find_cell(key);
                if (c == 0xFFFFFFFFu) continue;
                for (uint32_t s = 0; s < cell_count_[c]; s++) {
                    const uint32_t j = order_[cell_start_[c] + s];
                    if (j >= pos_.size()) continue;   // ghost
                    rho += k.w((pos_[j] - p).length_sq()) * mass;
                }
            }
    return rho;
}

Vec3 Fluid::density_gradient_at(const Vec3 &p) const {
    if (cell_key_.empty()) return Vec3();
    const Kernel k(material.smoothing_radius);
    const float mass = particle_mass();
    const float inv = 1.0f / material.smoothing_radius;
    const int64_t bx = int64_t(std::floor(p.x * inv));
    const int64_t by = int64_t(std::floor(p.y * inv));
    const int64_t bz = int64_t(std::floor(p.z * inv));
    Vec3 g;
    for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dz = -1; dz <= 1; dz++) {
                const int64_t key = (((bx + dx) & 0x1FFFFF) << 42) |
                                    (((by + dy) & 0x1FFFFF) << 21) |
                                    ((bz + dz) & 0x1FFFFF);
                const uint32_t c = find_cell(key);
                if (c == 0xFFFFFFFFu) continue;
                for (uint32_t s = 0; s < cell_count_[c]; s++) {
                    const uint32_t j = order_[cell_start_[c] + s];
                    if (j >= pos_.size()) continue;
                    const Vec3 d = p - pos_[j];
                    g += k.grad(d, d.length()) * mass;
                }
            }
    return g;
}

bool Fluid::occupied(const AABB &box) const {
    if (cell_key_.empty()) return false;
    const float h = material.smoothing_radius;
    const float inv = 1.0f / h;
    // Grown by a smoothing radius: a particle just outside the box
    // still puts density inside it.
    const AABB grown = box.grown(h);
    const int64_t x0 = int64_t(std::floor(grown.min.x * inv));
    const int64_t y0 = int64_t(std::floor(grown.min.y * inv));
    const int64_t z0 = int64_t(std::floor(grown.min.z * inv));
    const int64_t x1 = int64_t(std::floor(grown.max.x * inv));
    const int64_t y1 = int64_t(std::floor(grown.max.y * inv));
    const int64_t z1 = int64_t(std::floor(grown.max.z * inv));
    for (int64_t x = x0; x <= x1; x++)
        for (int64_t y = y0; y <= y1; y++)
            for (int64_t z = z0; z <= z1; z++) {
                const int64_t key = ((x & 0x1FFFFF) << 42) |
                                    ((y & 0x1FFFFF) << 21) | (z & 0x1FFFFF);
                if (find_cell(key) != 0xFFFFFFFFu) return true;
            }
    return false;
}

AABB Fluid::bounds() const {
    AABB b;
    for (const Vec3 &p : pos_) b.expand(p);
    return b.grown(material.smoothing_radius);
}

// -------------------------------------------------------------- step

void Fluid::step(float dt) {
    if (pos_.empty()) {
        stats = Stats{};
        return;
    }
    // Split, so one long frame does not move every particle a metre
    // and turn the liquid into spray.
    int steps = std::max(1, int(std::ceil(dt / max_step)));
    steps = std::min(steps, 4);
    const float h = dt / float(steps);

    for (int s = 0; s < steps; s++) {
        std::vector<Vec3> before = pos_;
        predict(h);
        build_ghosts();
        build_grid();
        find_neighbours();
        solve_density();
        finish(h);

        // --- through the hole
        if (world_ && !world_->portals().empty()) {
            for (size_t i = 0; i < pos_.size(); i++) {
                float t = 0.0f;
                Portal3D *p = world_->crossing(before[i], pos_[i], &t);
                if (!p || !p->link()) continue;
                const Transform3D warp = Portal3D::warp(p, p->link());
                pos_[i] = warp.xform(pos_[i]);
                vel_[i] = warp.basis.xform(vel_[i]);
                predicted_[i] = pos_[i];
                stats.portal_crossings++;
            }
        }

        // --- the ones that got away
        if (kill_below_y > -1e30f) {
            for (size_t i = 0; i < pos_.size();) {
                if (pos_[i].y >= kill_below_y) {
                    i++;
                    continue;
                }
                const size_t last = pos_.size() - 1;
                pos_[i] = pos_[last];
                vel_[i] = vel_[last];
                predicted_[i] = predicted_[last];
                prev_[i] = prev_[last];
                contact_[i] = contact_[last];
                pushed_[i] = pushed_[last];
                pos_.pop_back();
                vel_.pop_back();
                predicted_.pop_back();
                prev_.pop_back();
                contact_.pop_back();
                pushed_.pop_back();
                delta_.pop_back();
                lambda_.pop_back();
                denom_.pop_back();
                density_.pop_back();
                vorticity_.pop_back();
                stats.killed++;
            }
        }
    }
    stats.particles = uint32_t(pos_.size());
}

}  // namespace wr
