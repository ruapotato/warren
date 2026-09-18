#include "nav/crowd.h"

#include <algorithm>
#include <cmath>

namespace wr::nav {

namespace {

constexpr float kEps = 1e-5f;

float det2(const Vec2 &a, const Vec2 &b) { return a.x * b.y - a.y * b.x; }
Vec2 flat(const Vec3 &v) { return Vec2(v.x, v.z); }
Vec3 lift(const Vec2 &v, float y) { return Vec3(v.x, y, v.y); }

// ONE CONSTRAINT along the boundary line, subject to the speed
// circle and to every constraint already accepted. This is the
// one-dimensional step of the program: the feasible set on a line,
// clipped by the others, is an interval, and the answer is whichever
// end of it is nearest what was wanted.
bool solve_line(const std::vector<OrcaLine> &lines, size_t k, float max_speed,
                const Vec2 &preferred, bool optimise_direction, Vec2 *result) {
    const OrcaLine &L = lines[k];
    float along = dot(L.point, L.direction);
    float disc = along * along + max_speed * max_speed - L.point.length_sq();
    if (disc < 0.0f) return false;  // the line misses the speed circle

    float root = std::sqrt(disc);
    float t_lo = -along - root;
    float t_hi = -along + root;

    for (size_t i = 0; i < k; ++i) {
        float den = det2(L.direction, lines[i].direction);
        float num = det2(lines[i].direction, L.point - lines[i].point);
        if (std::fabs(den) <= kEps) {
            // Parallel. Either this line lies entirely on the
            // allowed side of that one, or there is no answer here.
            if (num < 0.0f) return false;
            continue;
        }
        float t = num / den;
        if (den >= 0.0f)
            t_hi = std::min(t_hi, t);
        else
            t_lo = std::max(t_lo, t);
        if (t_lo > t_hi) return false;
    }

    if (optimise_direction) {
        *result = L.point + L.direction * (dot(preferred, L.direction) > 0.0f
                                               ? t_hi
                                               : t_lo);
    } else {
        float t = dot(L.direction, preferred - L.point);
        *result = L.point + L.direction * std::clamp(t, t_lo, t_hi);
    }
    return true;
}

// The whole program: start from the preferred velocity and, each
// time a constraint is violated, slide to the nearest point on that
// constraint's boundary that also satisfies everything before it.
// Returns the index of the first constraint that could not be
// satisfied, or lines.size() when all were.
size_t solve_all(const std::vector<OrcaLine> &lines, float max_speed,
                 const Vec2 &preferred, bool optimise_direction, Vec2 *result) {
    if (optimise_direction) {
        *result = preferred * max_speed;
    } else if (preferred.length_sq() > max_speed * max_speed) {
        *result = preferred.normalized() * max_speed;
    } else {
        *result = preferred;
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        if (det2(lines[i].direction, lines[i].point - *result) <= 0.0f) continue;
        Vec2 previous = *result;
        if (!solve_line(lines, i, max_speed, preferred, optimise_direction,
                        result)) {
            *result = previous;
            return i;
        }
    }
    return lines.size();
}

// NO SAFE VELOCITY EXISTS. This happens in a crush, and refusing to
// move is the worst possible answer -- a jam that never clears.
// Instead, minimise the worst violation: for each unsatisfiable
// constraint, project all the earlier ones onto it and solve again
// along it. The body ends up moving in the direction that intrudes
// least, so a packed crowd squeezes through instead of locking.
void relax(const std::vector<OrcaLine> &lines, size_t from, float max_speed,
           Vec2 *result) {
    float worst = 0.0f;
    std::vector<OrcaLine> projected;
    for (size_t i = from; i < lines.size(); ++i) {
        if (det2(lines[i].direction, lines[i].point - *result) <= worst)
            continue;
        projected.clear();
        for (size_t j = 0; j < i; ++j) {
            OrcaLine line;
            float den = det2(lines[i].direction, lines[j].direction);
            if (std::fabs(den) <= kEps) {
                if (dot(lines[i].direction, lines[j].direction) > 0.0f)
                    continue;  // same way; j adds nothing
                line.point = (lines[i].point + lines[j].point) * 0.5f;
            } else {
                float t = det2(lines[j].direction,
                               lines[i].point - lines[j].point) /
                          den;
                line.point = lines[i].point + lines[i].direction * t;
            }
            line.direction = (lines[j].direction - lines[i].direction).normalized();
            projected.push_back(line);
        }
        Vec2 previous = *result;
        Vec2 push(-lines[i].direction.y, lines[i].direction.x);
        if (solve_all(projected, max_speed, push, true, result) <
            projected.size())
            *result = previous;
        worst = det2(lines[i].direction, lines[i].point - *result);
    }
}

}  // namespace

void orca_constraints(const Vec2 &position, const Vec2 &velocity, float radius,
                      const std::vector<Vec2> &n_pos,
                      const std::vector<Vec2> &n_vel,
                      const std::vector<float> &n_radius, float time_horizon,
                      float dt, std::vector<OrcaLine> *lines) {
    lines->clear();
    const float inv_horizon = time_horizon > 0.0f ? 1.0f / time_horizon : 1.0f;
    const float inv_dt = dt > 0.0f ? 1.0f / dt : 60.0f;

    for (size_t i = 0; i < n_pos.size(); ++i) {
        const Vec2 rel_p = n_pos[i] - position;
        const Vec2 rel_v = velocity - n_vel[i];
        const float dist_sq = rel_p.length_sq();
        const float r = radius + n_radius[i];
        const float r_sq = r * r;

        OrcaLine line;
        Vec2 u;

        if (dist_sq > r_sq) {
            // Not touching. The forbidden velocities form a cone
            // truncated by a circle -- the circle is "they collide
            // eventually", the legs are "they collide sooner". `w`
            // is how far the current relative velocity sits into it.
            const Vec2 w = rel_v - rel_p * inv_horizon;
            const float w_len_sq = w.length_sq();
            const float d = dot(w, rel_p);

            if (d < 0.0f && d * d > r_sq * w_len_sq) {
                // Nearest escape is across the circular cap.
                const float w_len = std::sqrt(w_len_sq);
                const Vec2 unit_w = w / w_len;
                line.direction = Vec2(unit_w.y, -unit_w.x);
                u = unit_w * (r * inv_horizon - w_len);
            } else {
                // Nearest escape is across one of the cone's legs.
                const float leg = std::sqrt(dist_sq - r_sq);
                if (det2(rel_p, w) > 0.0f) {
                    line.direction = Vec2(rel_p.x * leg - rel_p.y * r,
                                          rel_p.x * r + rel_p.y * leg) /
                                     dist_sq;
                } else {
                    line.direction = -Vec2(rel_p.x * leg + rel_p.y * r,
                                           -rel_p.x * r + rel_p.y * leg) /
                                     dist_sq;
                }
                u = line.direction * dot(rel_v, line.direction) - rel_v;
            }
        } else {
            // Already overlapping, which happens after a spawn or a
            // shove. Push apart over one step rather than over the
            // horizon, so the overlap resolves now.
            const Vec2 w = rel_v - rel_p * inv_dt;
            const float w_len = w.length();
            if (w_len < kEps) continue;
            const Vec2 unit_w = w / w_len;
            line.direction = Vec2(unit_w.y, -unit_w.x);
            u = unit_w * (r * inv_dt - w_len);
        }

        // HALF THE CORRECTION, and this is the whole idea. The
        // neighbour is running the same arithmetic and will take the
        // other half, so the pair resolve exactly without either
        // knowing what the other decided.
        line.point = velocity + u * 0.5f;
        lines->push_back(line);
    }
}

bool orca_solve(const std::vector<OrcaLine> &lines, float max_speed,
                const Vec2 &preferred, Vec2 *out) {
    size_t failed = solve_all(lines, max_speed, preferred, false, out);
    if (failed == lines.size()) return true;
    relax(lines, failed, max_speed, out);
    return false;
}

// ----------------------------------------------------------- the crowd

uint32_t Crowd::add(const CrowdAgent &agent) {
    CrowdAgent a = agent;
    a.id = next_id_++;
    a.leg = 1;
    agents_.push_back(a);
    return a.id;
}

void Crowd::remove(uint32_t id) {
    for (size_t i = 0; i < agents_.size(); ++i) {
        if (agents_[i].id != id) continue;
        agents_.erase(agents_.begin() + long(i));
        return;
    }
}

CrowdAgent *Crowd::find(uint32_t id) {
    for (CrowdAgent &a : agents_)
        if (a.id == id) return &a;
    return nullptr;
}
const CrowdAgent *Crowd::find(uint32_t id) const {
    for (const CrowdAgent &a : agents_)
        if (a.id == id) return &a;
    return nullptr;
}
void Crowd::clear() { agents_.clear(); }

bool Crowd::set_target(uint32_t id, const Vec3 &target) {
    CrowdAgent *a = find(id);
    if (!a || !mesh_) return false;
    // Asking for the same place again is not a request to re-plan.
    // Games call this every frame; re-pathing every frame for thirty
    // bodies is the single easiest way to spend a whole budget on
    // nothing.
    if (a->has_target && (a->target - target).length_sq() < 0.04f &&
        !a->path.empty())
        return true;

    a->target = target;
    a->has_target = true;
    a->arrived = false;
    a->leg = 1;
    a->on_link = false;
    a->best_progress = 1e30f;
    a->stuck_for = 0.0f;
    return mesh_->find_path(a->position, target, &a->path, &a->path_partial,
                            a->filter);
}

void Crowd::stop(uint32_t id) {
    CrowdAgent *a = find(id);
    if (!a) return;
    a->has_target = false;
    a->path.clear();
    a->desired = Vec3();
}

void Crowd::follow_paths(float dt) {
    int replanned = 0;
    const size_t n = agents_.size();
    for (size_t k = 0; k < n; ++k) {
        // Round robin, so the replan budget does not always fall on
        // whoever happens to be first in the array.
        CrowdAgent &a = agents_[(replan_cursor_ + k) % n];
        a.desired = Vec3();
        a.on_link = false;
        a.link = 0xffff;
        if (!a.has_target || a.path.empty()) continue;

        // WHEN NOTHING IS HAPPENING, TRY SOMETHING ELSE.
        //
        // A body can end up wedged: it rounded a corner too early,
        // or a crowd closed in front of it, or its route was
        // invalidated by a door shutting. It is still following a
        // perfectly good path and going nowhere, and from the
        // outside it looks broken rather than thwarted.
        //
        // Measuring it is easy -- moving at a crawl while claiming
        // to be going somewhere -- and a fresh path from where the
        // body actually is almost always clears it, because the
        // corner it cut is now behind it.
        if (a.on_link) {
            // Mid-ladder is not stuck, however slowly it climbs.
            a.stuck_for = 0.0f;
        } else if (a.leg < a.path.size()) {
            Vec3 d = a.path[a.leg].position - a.position;
            d.y = 0.0f;
            const float left = d.length();
            // A quarter of a metre of real progress resets the
            // clock. Less than that over a second and a half, and
            // whatever the body is doing, it is not arriving.
            if (left < a.best_progress - 0.25f) {
                a.best_progress = left;
                a.stuck_for = 0.0f;
            } else {
                a.stuck_for += dt;
            }
        } else {
            a.stuck_for = 0.0f;
        }
        if (a.stuck_for > 1.5f && replanned < replans_per_step) {
            a.stuck_for = 0.0f;
            a.best_progress = 1e30f;
            if (mesh_ && mesh_->find_path(a.position, a.target, &a.path,
                                          &a.path_partial, a.filter)) {
                a.leg = 1;
                a.best_progress = 1e30f;
                ++replanned;
            }
        }

        // Near enough counts, when the caller said how near.
        if (a.goal_radius > 0.0f) {
            Vec3 d = a.target - a.position;
            d.y = 0.0f;
            if (d.length() <= a.goal_radius) {
                a.arrived = true;
                a.has_target = false;
                a.path.clear();
                continue;
            }
        }

        // Advance past every corner already reached. More than one
        // can fall inside the radius at once on a tight zig-zag, and
        // stopping at the first leaves the body circling it.
        while (a.leg < a.path.size()) {
            const PathPoint &p = a.path[a.leg];
            Vec3 d = p.position - a.position;
            d.y = 0.0f;
            // A link leg never advances on proximity. Getting
            // within a fraction of a ladder's top and calling that
            // arrived leaves the body in mid-air beside the roof,
            // off the mesh, where nothing can move it again. The
            // explicit handling below lands it on the far end.
            bool link_leg = a.path[a.leg - 1].flags == kPathLink;
            float reach = link_leg ? 0.0f
                                   : (a.leg + 1 == a.path.size() ? arrive_radius
                                                                 : corner_radius);
            if (d.length() > reach) break;
            ++a.leg;
            // A new leg is a new thing to get closer to.
            a.best_progress = 1e30f;
        }
        if (a.leg >= a.path.size()) {
            a.arrived = true;
            a.has_target = false;
            a.path.clear();
            continue;
        }

        const PathPoint &here = a.path[a.leg - 1];
        const PathPoint &next = a.path[a.leg];

        if (here.flags == kPathLink) {
            // Crossing a ladder, a drop, a vault. The route is the
            // link, not the ground, so avoidance is off for the
            // duration -- two bodies on one ladder is the game's
            // problem to prevent, and having them shove each other
            // off it in mid-air is not an improvement.
            a.on_link = true;
            a.link = here.link;
            Vec3 d = next.position - a.position;
            float len = d.length();
            // A LINK IS ONE MOVEMENT. Within a step of the far end,
            // land on it exactly rather than creeping up on it: the
            // far end is a point known to be on the mesh, and any
            // fraction short of it is not.
            if (len <= a.max_speed * dt * 1.5f + 1e-3f) {
                a.position = next.position;
                a.velocity = Vec3();
                a.on_link = false;
                a.link = 0xffff;
                ++a.leg;
            } else {
                a.desired = d * (a.max_speed / len);
            }
            continue;
        }

        Vec3 d = next.position - a.position;
        d.y = 0.0f;
        float len = d.length();

        // Slow into the final point, so a body stops rather than
        // overshooting and coming back.
        float speed = a.max_speed;
        if (a.leg + 1 == a.path.size())
            speed = std::min(speed, std::max(len * 2.0f, 0.1f));
        a.desired = len > kEps ? d * (speed / len) : Vec3();

        // Shoved off the route far enough that the route is no
        // longer the route. Re-plan, within the budget.
        if (replan_distance > 0.0f && replanned < replans_per_step) {
            Vec3 from = here.position, to = next.position;
            Vec3 seg = to - from;
            seg.y = 0.0f;
            float seg_len = seg.length_sq();
            float off;
            if (seg_len < 1e-6f) {
                off = (a.position - from).length();
            } else {
                float t = std::clamp(((a.position.x - from.x) * seg.x +
                                      (a.position.z - from.z) * seg.z) /
                                         seg_len,
                                     0.0f, 1.0f);
                Vec3 on = from + seg * t;
                off = std::sqrt((a.position.x - on.x) * (a.position.x - on.x) +
                                (a.position.z - on.z) * (a.position.z - on.z));
            }
            if (off > replan_distance) {
                if (mesh_ &&
                    mesh_->find_path(a.position, a.target, &a.path,
                                     &a.path_partial, a.filter)) {
                    a.leg = 1;
                    ++replanned;
                }
            }
        }
    }
    replan_cursor_ = n ? (replan_cursor_ + 1) % n : 0;
    stats_.replanned = replanned;
}

void Crowd::rebuild_grid() {
    grid_head_.clear();
    grid_next_.assign(agents_.size(), -1);
    if (agents_.empty()) return;

    AABB b;
    float widest = 0.5f;
    for (const CrowdAgent &a : agents_) {
        b.expand(a.position);
        widest = std::max(widest, a.radius);
    }
    // Cells a little larger than the widest body plus its look-ahead
    // means a neighbour search touches nine cells and no more.
    grid_cell_ = std::max(widest * 4.0f, 1.0f);
    grid_min_ = b.min;
    grid_w_ = std::max(1, int(b.size().x / grid_cell_) + 1);
    grid_d_ = std::max(1, int(b.size().z / grid_cell_) + 1);
    if (int64_t(grid_w_) * int64_t(grid_d_) > 1 << 20) {
        grid_w_ = grid_d_ = 0;  // implausible spread; fall back to a scan
        return;
    }
    grid_head_.assign(size_t(grid_w_) * size_t(grid_d_), -1);
    for (size_t i = 0; i < agents_.size(); ++i) {
        int x = std::clamp(int((agents_[i].position.x - grid_min_.x) / grid_cell_),
                           0, grid_w_ - 1);
        int z = std::clamp(int((agents_[i].position.z - grid_min_.z) / grid_cell_),
                           0, grid_d_ - 1);
        size_t c = size_t(z) * size_t(grid_w_) + size_t(x);
        grid_next_[i] = grid_head_[c];
        grid_head_[c] = int32_t(i);
    }
}

void Crowd::neighbours_of(size_t index, float range,
                          std::vector<size_t> *out) const {
    out->clear();
    const CrowdAgent &me = agents_[index];
    const float r2 = range * range;
    auto consider = [&](size_t j) {
        if (j == index) return;
        const CrowdAgent &o = agents_[j];
        if ((me.mask & o.layer) == 0) return;
        if (o.on_link) return;  // not on the ground; not in the way
        // Different storeys are not neighbours. Without this, bodies
        // on a roof dodge bodies in the street below them.
        if (std::fabs(o.position.y - me.position.y) > me.height * 0.8f) return;
        float dx = o.position.x - me.position.x;
        float dz = o.position.z - me.position.z;
        if (dx * dx + dz * dz > r2) return;
        out->push_back(j);
    };
    if (grid_w_ == 0) {
        for (size_t j = 0; j < agents_.size(); ++j) consider(j);
        return;
    }
    int x = std::clamp(int((me.position.x - grid_min_.x) / grid_cell_), 0,
                       grid_w_ - 1);
    int z = std::clamp(int((me.position.z - grid_min_.z) / grid_cell_), 0,
                       grid_d_ - 1);
    int reach = std::max(1, int(range / grid_cell_) + 1);
    for (int dz = -reach; dz <= reach; ++dz) {
        for (int dx = -reach; dx <= reach; ++dx) {
            int cx = x + dx, cz = z + dz;
            if (cx < 0 || cz < 0 || cx >= grid_w_ || cz >= grid_d_) continue;
            for (int32_t j = grid_head_[size_t(cz) * size_t(grid_w_) + size_t(cx)];
                 j >= 0; j = grid_next_[size_t(j)])
                consider(size_t(j));
        }
    }
}

void Crowd::avoid(float dt) {
    rebuild_grid();
    stats_.avoiding = 0;
    stats_.infeasible = 0;

    std::vector<size_t> near;
    std::vector<Vec2> n_pos, n_vel;
    std::vector<float> n_rad;
    std::vector<OrcaLine> lines;
    // Velocities are read from the previous step for every agent, so
    // the order they are processed in cannot matter. Writing into a
    // separate array is what makes that true.
    std::vector<Vec3> next(agents_.size());

    for (size_t i = 0; i < agents_.size(); ++i) {
        CrowdAgent &a = agents_[i];
        a.deflection = 0.0f;
        if (!a.avoidance || a.on_link) {
            next[i] = a.desired;
            continue;
        }
        float range = a.max_speed * a.time_horizon + a.radius * 4.0f;
        neighbours_of(i, range, &near);
        if (near.empty()) {
            next[i] = a.desired;
            continue;
        }
        ++stats_.avoiding;

        n_pos.clear();
        n_vel.clear();
        n_rad.clear();
        for (size_t j : near) {
            n_pos.push_back(flat(agents_[j].position));
            n_vel.push_back(flat(agents_[j].velocity));
            n_rad.push_back(agents_[j].radius);
        }
        orca_constraints(flat(a.position), flat(a.velocity), a.radius, n_pos,
                         n_vel, n_rad, a.time_horizon, dt, &lines);

        Vec2 solved;
        if (!orca_solve(lines, a.max_speed, flat(a.desired), &solved))
            ++stats_.infeasible;
        next[i] = lift(solved, 0.0f);
        a.deflection = (Vec2(a.desired.x, a.desired.z) - solved).length();
    }
    for (size_t i = 0; i < agents_.size(); ++i) {
        if (agents_[i].on_link) continue;
        agents_[i].desired = next[i];
    }
}

void Crowd::integrate(float dt) {
    for (CrowdAgent &a : agents_) {
        // Accelerate toward the chosen velocity rather than snapping
        // to it. Snapping is what makes avoidance read as a glitch:
        // a body that reverses in one frame did not decide anything,
        // it teleported.
        Vec3 dv = a.desired - a.velocity;
        if (!a.on_link) dv.y = 0.0f;
        float limit = a.max_accel * dt;
        float len = dv.length();
        if (len > limit && len > kEps) dv = dv * (limit / len);
        a.velocity = a.velocity + dv;
        if (!a.on_link) a.velocity.y = 0.0f;

        float speed = a.velocity.length();
        if (speed > a.max_speed && speed > kEps)
            a.velocity = a.velocity * (a.max_speed / speed);

        Vec3 want = a.position + a.velocity * dt;

        if (a.on_link || !mesh_) {
            a.position = want;
            continue;
        }

        // STAY ON THE MESH, and slide rather than stick.
        //
        // Avoidance knows about other bodies and nothing at all
        // about walls, so a body dodging into a corner is dodging
        // into a wall. Stopping it dead there would be correct and
        // would look terrible: something walking at a wall at a
        // shallow angle should carry on along it.
        //
        // PROJECTION IS THE SLIDE. The nearest point on the mesh to
        // a position just outside it is the perpendicular foot on
        // the boundary -- which is the motion with the into-the-wall
        // part removed, exactly. Doing it this way rather than by
        // reflecting off a wall normal also survives the case that
        // broke the first attempt: a body already standing exactly
        // on the boundary, where a ray leaves immediately and there
        // is no normal to be had.
        //
        // The ray is still worth casting, as the guard against
        // going through a thin wall in one step. Projection alone
        // would happily put a body on whichever side of a wall it
        // ended up nearer.
        // OFF THE MESH ENTIRELY is a state to get out of, not one
        // to reason from. A body can arrive here after being
        // dropped into the level, after a bake changed under it, or
        // after a bug; whatever the cause, refusing to move it
        // leaves it standing in the air for the rest of the game.
        // Put it back first and move it afterwards.
        if (mesh_->find_poly(a.position, Vec3(0.5f, a.height, 0.5f),
                             a.filter) == nav::kNoPoly) {
            Vec3 back;
            if (mesh_->nearest_point(a.position,
                                     Vec3(8.0f, a.height * 2.0f, 8.0f), &back,
                                     nullptr, a.filter)) {
                a.position = back;
                want = back + a.velocity * dt;
            }
        }

        const float step = (want - a.position).length();
        Vec3 on;
        const bool projected = mesh_->nearest_point(
            want, Vec3(1.0f, a.height, 1.0f), &on, nullptr, a.filter);
        Vec3 hit;
        if (mesh_->raycast(a.position, want, &hit, a.filter)) {
            if (projected) want = on;
        } else if (projected &&
                   (on - a.position).length() <= step * 2.0f + 1e-3f) {
            // Slid along the wall. Accepting the projection only
            // when it is within a couple of steps of where the body
            // already is means it cannot have crossed anything: a
            // body moving four centimetres a frame cannot arrive on
            // the far side of a wall.
            //
            // The bound also has to tolerate a body that is a hair
            // OUTSIDE the mesh, which happens however carefully the
            // projection is done, and which used to wedge it: the
            // ray from an outside point fails at once, and refusing
            // to move without a successful ray is refusing to move
            // at all.
            want = on;
        } else {
            want = hit;  // genuinely into a corner
        }

        Vec3 moved = want - a.position;
        a.position = want;
        // WHAT ACTUALLY HAPPENED BECOMES THE VELOCITY. Where nothing
        // was in the way this changes nothing, because the body went
        // exactly where it meant to. Where a wall stopped it, this
        // is what stops it pressing: the component into the wall is
        // gone, so next step's avoidance is solved from a velocity
        // the body really has.
        moved.y = 0.0f;
        a.velocity = moved * (1.0f / dt);
    }
}

void Crowd::step(float dt) {
    if (dt <= 0.0f) return;
    stats_ = Stats();
    stats_.agents = int(agents_.size());
    follow_paths(dt);
    avoid(dt);
    integrate(dt);
}

}  // namespace wr::nav
