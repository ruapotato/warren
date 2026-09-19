#include "world.h"

#include <algorithm>
#include <cstdio>

#include "core/log.h"
#include "physics/dynamics.h"
#include "render/mesh.h"
#include "scene/portal.h"

namespace wr {

PhysicsWorld::PhysicsWorld() = default;
PhysicsWorld::~PhysicsWorld() = default;

// ------------------------------------------------------------- colliders

void PhysicsWorld::refresh_bounds(Collider &c) {
    if (c.shape.type == ShapeType::Mesh && c.mesh >= 0 &&
        c.mesh < int32_t(meshes_.size())) {
        c.bounds = meshes_[size_t(c.mesh)].bounds().transformed(c.transform);
    } else {
        c.bounds = c.shape.world_bounds(c.transform);
    }
}

ColliderId PhysicsWorld::add_mesh(const Mesh &mesh, const Transform3D &transform,
                                  uint32_t layer, Node3D *owner) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        WR_WARN("physics: a collider was asked for from an empty mesh");
        return {};
    }
    std::vector<Vec3> positions;
    positions.reserve(mesh.vertices.size());
    for (const Vertex &v : mesh.vertices) positions.push_back(v.position);

    meshes_.emplace_back();
    meshes_.back().build(std::move(positions), mesh.indices);
    int32_t index = int32_t(meshes_.size()) - 1;

    ColliderId id = add_shape(Shape::triangle_mesh(index), transform, layer, owner,
                              true);
    if (Collider *c = get(id)) {
        c->mesh = index;
        refresh_bounds(*c);
    }
    return id;
}

ColliderId PhysicsWorld::add_shape(const Shape &shape, const Transform3D &transform,
                                   uint32_t layer, Node3D *owner, bool is_static) {
    uint32_t index;
    if (!free_.empty()) {
        index = free_.back();
        free_.pop_back();
    } else {
        index = uint32_t(slots_.size());
        slots_.push_back({});
    }
    Slot &s = slots_[index];
    s.live = true;
    if (s.generation == 0) s.generation = 1;
    s.collider = Collider();
    s.collider.shape = shape;
    s.collider.transform = transform;
    s.collider.layer = layer;
    s.collider.owner = owner;
    s.collider.is_static = is_static;
    s.collider.mesh = shape.type == ShapeType::Mesh ? shape.mesh : -1;
    refresh_bounds(s.collider);
    live_++;
    ColliderId id;
    id.index = index;
    id.generation = s.generation;
    return id;
}

Collider *PhysicsWorld::get(ColliderId id) {
    if (!id.valid() || id.index >= slots_.size()) return nullptr;
    Slot &s = slots_[id.index];
    return (s.live && s.generation == id.generation) ? &s.collider : nullptr;
}

const Collider *PhysicsWorld::get(ColliderId id) const {
    return const_cast<PhysicsWorld *>(this)->get(id);
}

void PhysicsWorld::set_transform(ColliderId id, const Transform3D &t) {
    if (Collider *c = get(id)) {
        c->transform = t;
        refresh_bounds(*c);
    }
}

void PhysicsWorld::set_shape(ColliderId id, const Shape &s) {
    if (Collider *c = get(id)) {
        c->shape = s;
        refresh_bounds(*c);
    }
}

void PhysicsWorld::remove(ColliderId id) {
    if (!id.valid() || id.index >= slots_.size()) return;
    Slot &s = slots_[id.index];
    if (!s.live || s.generation != id.generation) return;
    s.live = false;
    s.generation++;
    if (s.generation == 0) s.generation = 1;
    free_.push_back(id.index);
    live_--;
}

void PhysicsWorld::add_portal(Portal3D *p) {
    if (!p) return;
    if (std::find(portals_.begin(), portals_.end(), p) == portals_.end())
        portals_.push_back(p);
}

void PhysicsWorld::remove_portal(Portal3D *p) {
    portals_.erase(std::remove(portals_.begin(), portals_.end(), p), portals_.end());
}

// ------------------------------------------------------------- raycasting

RayHit PhysicsWorld::raycast(const Vec3 &from, const Vec3 &to, uint32_t mask,
                             const Node3D *ignore) const {
    RayHit hit;
    Vec3 delta = to - from;
    float length = delta.length();
    if (length < 1e-7f) return hit;
    Vec3 dir = delta / length;
    hit.direction = dir;

    float best = length;
    for (const Slot &s : slots_) {
        if (!s.live) continue;
        const Collider &c = s.collider;
        if (!(c.layer & mask)) continue;
        if (ignore && c.owner == ignore) continue;
        float box_t = 0.0f;
        if (!c.bounds.intersects_ray(from, dir, &box_t) || box_t > best) continue;

        if (c.shape.type == ShapeType::Mesh) {
            const TriangleMesh &m = meshes_[size_t(c.mesh)];
            // Into the mesh's own space, so the BVH never has to move.
            Transform3D inv = c.transform.inverse();
            Vec3 lo = inv.xform(from);
            Vec3 ld = inv.basis.xform(dir);
            float scale = ld.length();
            if (scale < 1e-9f) continue;
            ld /= scale;
            float t = 0.0f;
            Vec3 n;
            stats.triangle_tests++;
            if (m.raycast(lo, ld, best * scale, &t, &n) >= 0) {
                float world_t = t / scale;
                if (world_t < best) {
                    best = world_t;
                    hit.hit = true;
                    hit.position = from + dir * world_t;
                    hit.normal = c.transform.basis.inverse().transposed()
                                     .xform(n).normalized();
                    hit.collider = {uint32_t(&s - slots_.data()), s.generation};
                    hit.node = c.owner;
                }
            }
            continue;
        }

        // Primitives: a sweep of a zero-radius sphere is a ray, so the
        // same code serves both and there is one implementation to get
        // right.
        float fraction = 1.0f;
        Vec3 n, p;
        Shape point = Shape::sphere(0.0f);
        Transform3D at(Basis(), from);
        if (sweep_one(point, at, dir * best, c, &fraction, &n, &p)) {
            float world_t = fraction * best;
            if (world_t < best) {
                best = world_t;
                hit.hit = true;
                hit.position = p;
                hit.normal = n;
                hit.collider = {uint32_t(&s - slots_.data()), s.generation};
                hit.node = c.owner;
            }
        }
    }
    hit.distance = hit.hit ? best : length;
    return hit;
}

Portal3D *PhysicsWorld::crossing(const Vec3 &from, const Vec3 &to,
                                 float *out_t) const {
    Portal3D *best = nullptr;
    float best_t = 2.0f;
    for (Portal3D *p : portals_) {
        if (!p || !p->active || !p->linked()) continue;
        float t;
        if (p->crossed(from, to, &t) && t < best_t) {
            best_t = t;
            best = p;
        }
    }
    if (best && out_t) *out_t = best_t;
    return best;
}

bool PhysicsWorld::inside_aperture(const Vec3 &point, float margin) const {
    for (Portal3D *p : portals_) {
        if (!p || !p->active || !p->linked()) continue;
        // Near the plane and within the rectangle. The slab is
        // deliberately generous: a wall has thickness and the whole of
        // it has to stop being solid, not just its surface.
        float d = std::fabs(p->plane().distance_to(point));
        if (d > aperture_thickness) continue;
        if (p->within_aperture(point, margin)) return true;
    }
    return false;
}

bool PhysicsWorld::inside_aperture(const Vec3 &point, const Vec3 &surface_normal,
                                   float margin) const {
    return inside_aperture(point, surface_normal, margin, nullptr);
}

bool PhysicsWorld::inside_aperture(const Vec3 &point, const Vec3 &surface_normal,
                                   float margin, const Shape *asker) const {
    for (Portal3D *p : portals_) {
        if (!p || !p->active || !p->linked()) continue;
        if (std::fabs(p->plane().distance_to(point)) > aperture_thickness) continue;
        if (!p->within_aperture(point, margin)) continue;
        // The one exception: see the header.
        const bool portal_is_upright =
            std::fabs(dot(p->normal(), Vec3::up())) < aperture_upright;
        const bool surface_is_floor = dot(surface_normal, Vec3::up()) > aperture_upright;
        if (portal_is_upright && surface_is_floor) continue;
        // AND ONLY IF THE ASKER FITS. A hole too small to get
        // through is a wall, and it has to be a wall to the
        // COLLISION as well as to the traversal -- otherwise a body
        // that is refused the crossing is refused it while standing
        // in a gap where the wall used to be.
        if (asker && !admits_shape(*p, *asker)) continue;
        return true;
    }
    return false;
}

bool PhysicsWorld::admits_shape(const Portal3D &p, const Shape &s) {
    switch (s.type) {
        case ShapeType::Capsule:
            return p.admits(s.radius, s.height + s.radius * 2.0f);
        case ShapeType::Sphere:
            return p.admits(s.radius, s.radius * 2.0f);
        case ShapeType::Box: {
            // A box turns, so what matters is the smallest way
            // through it and the largest way across it. Using the
            // half-diagonal would be exact and would refuse a crate
            // that plainly fits when held square, which is the
            // wrong way to be wrong in a puzzle.
            const Vec3 h = s.half_extents;
            const float girth = std::max(h.x, h.z);
            return p.admits(girth, h.y * 2.0f);
        }
        default:
            return true;
    }
}

// THE PORTAL-AWARE TRACE.
//
// Walk the segment; if it meets an aperture before it meets anything
// solid, warp the remainder and carry on. The accumulated transform
// rides along so the caller can map anything from the start of the
// path to the end of it.
RayHit PhysicsWorld::trace(const Vec3 &from, const Vec3 &to, uint32_t mask,
                           const Node3D *ignore) const {
    stats.traces++;
    RayHit result;
    Vec3 a = from, b = to;
    Transform3D warp = Transform3D::identity();
    float travelled = 0.0f;

    for (int hop = 0; hop <= max_hops_; hop++) {
        float portal_t = 0.0f;
        Portal3D *p = crossing(a, b, &portal_t);
        RayHit solid = raycast(a, b, mask, hop == 0 ? ignore : nullptr);
        // THE HOLE IN THE WALL APPLIES TO RAYS TOO. Without this a
        // shot fired through a doorway stops in the door frame, which
        // is the single most obvious way a portal can feel fake.
        if (solid.hit &&
            inside_aperture(solid.position, solid.normal, -aperture_edge))
            solid.hit = false;

        float segment = (b - a).length();
        // The nearer of the two decides what happens.
        float solid_t = solid.hit ? (solid.distance / std::max(segment, 1e-7f)) : 2.0f;
        if (!p || solid_t <= portal_t) {
            result = solid;
            result.distance = travelled + (solid.hit ? solid.distance : segment);
            result.portals_crossed = hop;
            result.total_warp = warp;
            if (!solid.hit) result.direction = (b - a).normalized();
            return result;
        }

        // Through the aperture. The remainder of the segment is
        // measured from the crossing point so nothing is travelled
        // twice.
        stats.portal_hops++;
        Vec3 at = lerp(a, b, portal_t);
        travelled += (at - a).length();
        Transform3D step = Portal3D::warp(p, p->link());
        warp = step * warp;
        // A hair past the exit plane, or the next test finds the same
        // aperture again from behind and the ray stalls.
        Vec3 exit = step.xform(at);
        Vec3 exit_dir = step.basis.xform(b - at);
        Vec3 nudge = p->link()->normal() * 1e-3f;
        a = exit + nudge;
        b = exit + exit_dir;
    }

    // Out of hops: report the end of the last leg rather than nothing,
    // so a caller sees a miss and not a wild value.
    result.hit = false;
    result.position = b;
    result.direction = (b - a).length_sq() > 0 ? (b - a).normalized() : Vec3();
    result.distance = travelled + (b - a).length();
    result.portals_crossed = max_hops_;
    result.total_warp = warp;
    return result;
}

// ---------------------------------------------------------------- sweeps

// Conservative advancement: step the shape forward to the first time
// of impact, re-measure, repeat. Robust for the shapes here and, more
// to the point, it uses the same distance function as depenetration,
// so a body cannot pass a test in one and fail it in the other.
bool PhysicsWorld::sweep_one(const Shape &shape, const Transform3D &from,
                             const Vec3 &motion, const Collider &c,
                             float *out_fraction, Vec3 *out_normal,
                             Vec3 *out_point) const {
    const float length = motion.length();
    if (length < 1e-9f) return false;
    const Vec3 dir = motion / length;

    float travelled = 0.0f;
    for (int iter = 0; iter < 32; iter++) {
        Transform3D at = from;
        at.origin = from.origin + dir * travelled;
        Vec3 normal;
        float depth = 0.0f;
        // Ask about the whole of what is left of the motion, so the
        // gap that comes back is one we can actually step by.
        contact_one(shape, at, c, &normal, &depth, length - travelled);
        // This collider has nothing to say about where the shape is.
        if (depth <= -1e29f) return false;

        // HOW FAST THE GAP IS CLOSING, and the reason a character can
        // walk at all.
        //
        // A body standing on a floor is TOUCHING it, so a test that
        // only asks "are we in contact?" reports an impact for every
        // motion including walking along it -- and the slide that
        // follows removes nothing, so the body stands still for ever.
        // Only motion INTO a surface is an impact; motion along it or
        // away from it is not.
        const float closing = -dot(dir, normal);
        if (closing <= 1e-4f) return false;

        if (depth > 0.0f) {
            // Already overlapping and still heading in. Stop here and
            // let the depenetration pass push it out.
            *out_fraction = clampf(travelled / length, 0.0f, 1.0f);
            *out_normal = normal;
            *out_point = at.origin - normal * depth;
            return true;
        }

        const float gap = -depth;
        if (gap <= 1e-4f) {
            *out_fraction = clampf(travelled / length, 0.0f, 1.0f);
            *out_normal = normal;
            *out_point = at.origin;
            return true;
        }
        // Conservative advancement: the gap cannot close faster than
        // this, so stepping by it can never tunnel.
        travelled += std::max(gap / closing, 1e-4f);
        if (travelled >= length) return false;
    }
    return false;
}

// Signed distance and normal between a moving primitive and a
// collider. Returns true when they overlap; `depth` is the
// penetration when they do and the NEGATIVE separation when they do
// not, so one number serves both.
bool PhysicsWorld::contact_one(const Shape &shape, const Transform3D &at,
                               const Collider &c, Vec3 *out_normal,
                               float *out_depth, float search) const {
    // A contact inside an open aperture is not a contact: that part of
    // the surface is a doorway. Meshes filter per triangle below,
    // because one triangle of a wall may be in the doorway and the
    // next one beside it; a primitive is small enough to judge whole.
    // With the surface's own normal, which is what separates the wall
    // the portal is cut into from the floor it stands on.
    auto in_doorway = [&](const Vec3 &point, const Vec3 &normal) {
        // WITH THE MOVING SHAPE, so a portal too small for it is
        // still a wall. The shape is the only thing here that knows
        // how big the asker is.
        return !portals_.empty() &&
               inside_aperture(point, normal, -aperture_edge, &shape);
    };
    // EVERY EARLY RETURN SETS BOTH OUTPUTS. A path that leaves the
    // normal untouched hands the sweep loop a garbage direction, and
    // the sweep then either misses everything or reports a hit at a
    // wild position -- which is much harder to recognise as
    // uninitialised memory than a crash would be.
    *out_normal = Vec3::up();
    *out_depth = -1e30f;
    // No contact information at all, as distinct from "not touching".
    const float kNoContact = -1e29f;
    // The moving shape as a segment plus a radius, which covers a
    // sphere (a degenerate segment) and a capsule.
    float r = shape.radius;
    Vec3 up = at.basis.y().normalized();
    float half = shape.type == ShapeType::Capsule ? shape.height * 0.5f : 0.0f;
    Vec3 p0 = at.origin - up * half;
    Vec3 p1 = at.origin + up * half;

    switch (c.shape.type) {
        case ShapeType::Mesh: {
            const TriangleMesh &m = meshes_[size_t(c.mesh)];
            Transform3D inv = c.transform.inverse();
            float mesh_scale = c.transform.basis.uniform_scale();
            if (mesh_scale < 1e-6f) return false;
            Vec3 l0 = inv.xform(p0), l1 = inv.xform(p1);
            float lr = r / mesh_scale;
            AABB box;
            box.expand(l0);
            box.expand(l1);
            box = box.grown(lr + search / mesh_scale + 1e-3f);

            std::vector<uint32_t> tris;
            m.query(box, tris);
            float best_depth = -1e30f;
            Vec3 best_normal = Vec3::up();
            for (uint32_t tri : tris) {
                Vec3 t[3];
                m.triangle(tri, t);
                // Closest point between the capsule's axis and the
                // triangle, sampled at both ends and the midpoint --
                // exact for a sphere, and close enough for a capsule
                // that the depenetration loop converges.
                Vec3 best_on_axis = l0;
                Vec3 best_on_tri = closest_point_on_triangle(l0, t[0], t[1], t[2]);
                float best_d = (best_on_axis - best_on_tri).length();
                for (float s : {0.5f, 1.0f}) {
                    Vec3 a = lerp(l0, l1, s);
                    Vec3 q = closest_point_on_triangle(a, t[0], t[1], t[2]);
                    float d = (a - q).length();
                    if (d < best_d) {
                        best_d = d;
                        best_on_axis = a;
                        best_on_tri = q;
                    }
                }
                // A contact inside an open aperture is not a contact:
                // that part of the wall is a doorway.
                {
                    // The triangle's own normal decides whether it is
                    // part of the wall the portal is cut into.
                    Vec3 tn = c.transform.basis
                                  .xform(cross(t[1] - t[0], t[2] - t[0]))
                                  .normalized();
                    if (in_doorway(c.transform.xform(best_on_tri), tn)) continue;
                }
                float depth = lr - best_d;
                if (depth > best_depth) {
                    best_depth = depth;
                    Vec3 delta = best_on_axis - best_on_tri;
                    best_normal = delta.length_sq() > 1e-12f
                                      ? delta.normalized()
                                      : cross(t[1] - t[0], t[2] - t[0]).normalized();
                }
            }
            if (tris.empty() || best_depth <= kNoContact) return false;
            *out_normal = c.transform.basis.xform(best_normal).normalized();
            *out_depth = best_depth * mesh_scale;
            return *out_depth > 0.0f;
        }
        case ShapeType::Sphere: {
            Vec3 c1, c2;
            closest_points_on_segments(p0, p1, c.transform.origin,
                                       c.transform.origin, &c1, &c2);
            Vec3 delta = c1 - c2;
            float d = delta.length();
            float total = r + c.shape.radius * c.transform.basis.uniform_scale();
            Vec3 n = d > 1e-6f ? delta / d : Vec3::up();
            if (in_doorway(c2, n)) return false;
            *out_normal = n;
            *out_depth = total - d;
            return *out_depth > 0.0f;
        }
        case ShapeType::Capsule: {
            float ch = c.shape.height * 0.5f;
            Vec3 cup = c.transform.basis.y().normalized();
            Vec3 q0 = c.transform.origin - cup * ch;
            Vec3 q1 = c.transform.origin + cup * ch;
            Vec3 c1, c2;
            closest_points_on_segments(p0, p1, q0, q1, &c1, &c2);
            Vec3 delta = c1 - c2;
            float d = delta.length();
            float total = r + c.shape.radius * c.transform.basis.uniform_scale();
            Vec3 n = d > 1e-6f ? delta / d : Vec3::up();
            if (in_doorway(c2, n)) return false;
            *out_normal = n;
            *out_depth = total - d;
            return *out_depth > 0.0f;
        }
        case ShapeType::Box: {
            // Into the box's space, clamp, back out.
            Transform3D inv = c.transform.inverse();
            float s = c.transform.basis.uniform_scale();
            Vec3 l0 = inv.xform(p0), l1 = inv.xform(p1);
            float best_d = 1e30f;
            Vec3 best_axis, best_box;
            for (float f = 0.0f; f <= 1.0f; f += 0.5f) {
                Vec3 a = lerp(l0, l1, f);
                Vec3 q(clampf(a.x, -c.shape.half_extents.x, c.shape.half_extents.x),
                       clampf(a.y, -c.shape.half_extents.y, c.shape.half_extents.y),
                       clampf(a.z, -c.shape.half_extents.z, c.shape.half_extents.z));
                float d = (a - q).length();
                if (d < best_d) {
                    best_d = d;
                    best_axis = a;
                    best_box = q;
                }
            }
            Vec3 delta = best_axis - best_box;
            Vec3 n;
            if (delta.length_sq() > 1e-12f) {
                n = delta.normalized();
            } else {
                // Centre inside the box: push out along the nearest face.
                Vec3 d(c.shape.half_extents.x - std::fabs(best_axis.x),
                       c.shape.half_extents.y - std::fabs(best_axis.y),
                       c.shape.half_extents.z - std::fabs(best_axis.z));
                int axis = d.min_axis();
                n = Vec3();
                n[axis] = best_axis[axis] >= 0.0f ? 1.0f : -1.0f;
                best_d = -d[axis];
            }
            Vec3 wn = c.transform.basis.xform(n).normalized();
            if (in_doorway(c.transform.xform(best_box), wn)) return false;
            *out_normal = wn;
            *out_depth = r - best_d * s;
            return *out_depth > 0.0f;
        }
    }
    return false;
}

SweepHit PhysicsWorld::sweep(const Shape &shape, const Transform3D &from,
                             const Vec3 &motion, uint32_t mask,
                             const Node3D *ignore) const {
    stats.sweeps++;
    SweepHit hit;
    if (motion.length_sq() < 1e-12f) return hit;

    AABB path = shape.world_bounds(from);
    Transform3D to = from;
    to.origin += motion;
    path.expand(shape.world_bounds(to));
    path = path.grown(0.01f);

    for (const Slot &s : slots_) {
        if (!s.live) continue;
        const Collider &c = s.collider;
        if (!(c.layer & mask)) continue;
        if (c.is_trigger) continue;
        if (ignore && c.owner == ignore) continue;
        if (!c.bounds.intersects(path)) continue;
        float fraction = 1.0f;
        Vec3 n, p;
        if (sweep_one(shape, from, motion, c, &fraction, &n, &p) &&
            fraction < hit.fraction) {
            hit.hit = true;
            hit.fraction = fraction;
            hit.normal = n;
            hit.position = p;
            hit.collider = {uint32_t(&s - slots_.data()), s.generation};
            hit.node = c.owner;
        }
    }
    return hit;
}

int PhysicsWorld::depenetrate(const Shape &shape, const Transform3D &at,
                              uint32_t mask, const Node3D *ignore,
                              Vec3 *out_correction, Vec3 *out_deepest) const {
    Vec3 correction;
    Vec3 deepest_normal;
    float deepest = 0.0f;
    int contacts = 0;
    AABB box = shape.world_bounds(at).grown(0.02f);

    for (const Slot &s : slots_) {
        if (!s.live) continue;
        const Collider &c = s.collider;
        if (!(c.layer & mask)) continue;
        if (c.is_trigger) continue;
        if (ignore && c.owner == ignore) continue;
        if (!c.bounds.intersects(box)) continue;
        Vec3 n;
        float depth = 0.0f;
        if (!contact_one(shape, at, c, &n, &depth)) continue;
        contacts++;
        // The largest push along each normal rather than the sum: two
        // faces of the same corner would otherwise eject the body at
        // twice the distance.
        float along = dot(correction, n);
        if (depth > along) correction += n * (depth - along);
        if (depth > deepest) {
            deepest = depth;
            deepest_normal = n;
        }
    }
    if (out_correction) *out_correction = correction;
    if (out_deepest) *out_deepest = deepest_normal;
    return contacts;
}

void PhysicsWorld::overlap(const AABB &box, uint32_t mask,
                           std::vector<ColliderId> &out) const {
    for (const Slot &s : slots_) {
        if (!s.live) continue;
        if (!(s.collider.layer & mask)) continue;
        if (!s.collider.bounds.intersects(box)) continue;
        out.push_back({uint32_t(&s - slots_.data()), s.generation});
    }
}

void PhysicsWorld::query_triangles(const AABB &box, uint32_t mask,
                                   std::vector<WorldTriangle> &out) const {
    for (const Slot &slot : slots_) {
        if (!slot.live) continue;
        const Collider &c = slot.collider;
        if (!(c.layer & mask)) continue;
        if (c.shape.type != ShapeType::Mesh) continue;
        if (c.mesh < 0 || size_t(c.mesh) >= meshes_.size()) continue;
        if (!c.bounds.intersects(box)) continue;
        const TriangleMesh &m = meshes_[size_t(c.mesh)];
        // The query is in the mesh's own space; the triangles come
        // back in the world's, because every caller wants them there
        // and doing it here means one transform per triangle rather
        // than one per triangle per caller.
        const Transform3D inv = c.transform.inverse();
        AABB local;
        for (int i = 0; i < 8; i++) {
            const Vec3 corner(((i & 1) ? box.max.x : box.min.x),
                              ((i & 2) ? box.max.y : box.min.y),
                              ((i & 4) ? box.max.z : box.min.z));
            local.expand(inv.xform(corner));
        }
        std::vector<uint32_t> tris;
        m.query(local, tris);
        for (uint32_t ti : tris) {
            WorldTriangle wt;
            Vec3 v[3];
            m.triangle(ti, v);
            for (int k = 0; k < 3; k++) wt.v[k] = c.transform.xform(v[k]);
            wt.collider = ColliderId{uint32_t(&slot - slots_.data()),
                                     slot.generation};
            wt.node = c.owner;
            wt.index = ti;
            out.push_back(wt);
        }
    }
}

// ---------------------------------------------------------- for scripts

static Dict hit_to_dict(const RayHit &h) {
    Dict d;
    d["hit"] = Variant(h.hit);
    d["position"] = Variant(h.position);
    d["normal"] = Variant(h.normal);
    d["direction"] = Variant(h.direction);
    d["distance"] = Variant(double(h.distance));
    d["portals"] = Variant(int64_t(h.portals_crossed));
    d["warp"] = Variant(h.total_warp);
    d["node"] = Variant(static_cast<Object *>(h.node));
    return d;
}

Dict PhysicsWorld::crossing_dict(const Vec3 &from, const Vec3 &to) const {
    Dict d;
    float t = 0.0f;
    Portal3D *p = crossing(from, to, &t);
    Portal3D *q = p ? p->link() : nullptr;
    d["hit"] = Variant(p != nullptr && q != nullptr);
    d["t"] = Variant(double(t));
    d["warp"] = Variant(p && q ? Portal3D::warp(p, q) : Transform3D());
    d["scale"] = Variant(double(p && q ? Portal3D::scale_ratio(p, q) : 1.0f));
    d["portal"] = Variant(static_cast<Object *>(p));
    return d;
}

Dict PhysicsWorld::trace_dict(const Vec3 &from, const Vec3 &to, int64_t mask) const {
    return hit_to_dict(trace(from, to, uint32_t(mask)));
}

Dict PhysicsWorld::raycast_dict(const Vec3 &from, const Vec3 &to,
                                int64_t mask) const {
    return hit_to_dict(raycast(from, to, uint32_t(mask)));
}

int64_t PhysicsWorld::add_mesh_id(Mesh *mesh, const Transform3D &at,
                                  int64_t layer) {
    if (!mesh) return 0;
    return add_mesh(*mesh, at, uint32_t(layer)).packed();
}

int64_t PhysicsWorld::add_sphere(const Vec3 &at, float radius, int64_t layer) {
    return add_shape(Shape::sphere(radius), Transform3D(at), uint32_t(layer)).packed();
}
int64_t PhysicsWorld::add_box(const Transform3D &at, const Vec3 &half,
                              int64_t layer) {
    return add_shape(Shape::box(half), at, uint32_t(layer)).packed();
}
int64_t PhysicsWorld::add_capsule(const Transform3D &at, float radius,
                                  float height, int64_t layer) {
    return add_shape(Shape::capsule(radius, height), at, uint32_t(layer)).packed();
}

static void register_physics_classes() {
    ClassBuilder<PhysicsWorld>()
        .method("portal_crossing", &PhysicsWorld::crossing_dict)
                .args("from_point", "to_point")
        .method("trace", &PhysicsWorld::trace_dict,
                {Variant(int64_t(0xFFFFFFFF))})
        .args("from_point", "to_point", "mask")
        .method("raycast", &PhysicsWorld::raycast_dict,
                {Variant(int64_t(0xFFFFFFFF))})
        .args("from_point", "to_point", "mask")
        .method("add_mesh", &PhysicsWorld::add_mesh_id, {Variant(int64_t(1))})
        .args("mesh", "at", "layer")
        .method("add_sphere", &PhysicsWorld::add_sphere, {Variant(int64_t(1))})
        .args("at", "radius", "layer")
        .method("add_box", &PhysicsWorld::add_box, {Variant(int64_t(1))})
        .args("at", "half_extents", "layer")
        .method("add_capsule", &PhysicsWorld::add_capsule, {Variant(int64_t(1))})
        .args("at", "radius", "height", "layer")
        .method("add_portal", &PhysicsWorld::add_portal).args("portal")
        .method("remove_portal", &PhysicsWorld::remove_portal).args("portal")
        .method("remove_collider", &PhysicsWorld::remove_id).args("collider")
        .method("set_collider_transform", &PhysicsWorld::set_transform_id)
        .args("collider", "transform")
        .method("collider_count", &PhysicsWorld::collider_count)
        .method("report", &PhysicsWorld::report)
        .prop("max_portal_hops", &PhysicsWorld::max_portal_hops,
              &PhysicsWorld::set_max_portal_hops);
}
WR_REGISTER(register_physics_classes)

std::string PhysicsWorld::report() const {
    char b[256];
    std::snprintf(b, sizeof(b),
                  "physics: %zu colliders, %zu meshes, %zu portals | "
                  "%u traces (%u hops), %u sweeps",
                  live_, meshes_.size(), portals_.size(), stats.traces,
                  stats.portal_hops, stats.sweeps);
    return b;
}

}  // namespace wr

namespace wr {
DynamicsWorld &PhysicsWorld::dynamics() {
    if (!dynamics_) dynamics_ = std::make_unique<DynamicsWorld>(this);
    return *dynamics_;
}
}  // namespace wr
