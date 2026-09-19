// Warren -- collision.
//
// PORTAL-AWARE FROM THE BOTTOM UP. A ray that meets an aperture does
// not stop: it is warped and carries on out of the other end, and the
// hit it eventually reports carries the accumulated transform so the
// caller can put a decal, a spark or a bullet hole in the right place.
// A body that crosses an aperture is moved AND RESIZED by the ratio of
// the two ends.
//
// Bolting that on top of a physics engine that does not know about
// portals means every query has a special case and half of them are
// forgotten. Here it is one function -- `trace` -- and everything else
// calls it.
#pragma once

#include <memory>
#include <vector>

#include "core/object.h"
#include "physics/shapes.h"

namespace wr {

class Node3D;
class Portal3D;
class Mesh;
class DynamicsWorld;

struct ColliderId {
    uint32_t index = 0;
    uint32_t generation = 0;
    bool valid() const { return generation != 0; }
    bool operator==(const ColliderId &o) const {
        return index == o.index && generation == o.generation;
    }
    // Packed, for scripts: a handle is a number there, and packing the
    // generation in keeps the use-after-free check that makes these
    // handles worth having.
    int64_t packed() const {
        return int64_t(index) | (int64_t(generation) << 32);
    }
    static ColliderId unpack(int64_t v) {
        ColliderId id;
        id.index = uint32_t(v & 0xFFFFFFFF);
        id.generation = uint32_t(uint64_t(v) >> 32);
        return id;
    }
};

struct RayHit {
    bool hit = false;
    Vec3 position;
    Vec3 normal;
    // Total distance travelled, portals included, so a weapon's range
    // means the same on both sides of one.
    float distance = 0.0f;
    // The direction the last leg was travelling, which is not the
    // direction it started in if it went through a portal.
    Vec3 direction;
    ColliderId collider;
    Node3D *node = nullptr;
    int portals_crossed = 0;
    // Everything the path went through, composed. Multiply a source-
    // side transform by this to get its destination-side twin.
    Transform3D total_warp = Transform3D::identity();
};

struct SweepHit {
    bool hit = false;
    // 0..1 along the motion.
    float fraction = 1.0f;
    Vec3 position;
    Vec3 normal;
    ColliderId collider;
    Node3D *node = nullptr;
};

struct Collider {
    Shape shape;
    Transform3D transform;
    uint32_t layer = 1;
    Node3D *owner = nullptr;
    bool is_static = true;
    bool is_trigger = false;
    // Mesh colliders index this world's mesh table.
    int32_t mesh = -1;
    AABB bounds;
};

// An Object, so a script can raycast, trace through portals and add
// colliders without a C++ shim for each.
class PhysicsWorld : public Object {
    WR_CLASS(PhysicsWorld, Object)

public:
    PhysicsWorld();
    ~PhysicsWorld() override;

    // --- the world -------------------------------------------------------
    // A static triangle mesh: the level. The mesh is copied into a BVH
    // and the source may be released afterwards.
    ColliderId add_mesh(const Mesh &mesh, const Transform3D &transform,
                        uint32_t layer = 1, Node3D *owner = nullptr);
    ColliderId add_shape(const Shape &shape, const Transform3D &transform,
                         uint32_t layer = 1, Node3D *owner = nullptr,
                         bool is_static = true);
    void set_transform(ColliderId id, const Transform3D &t);
    void set_shape(ColliderId id, const Shape &s);
    void remove(ColliderId id);
    Collider *get(ColliderId id);
    const Collider *get(ColliderId id) const;
    size_t collider_count() const { return live_; }

    // --- portals ------------------------------------------------------------
    // Registered separately from colliders: a portal is not a solid,
    // it is a rule about how space connects.
    void add_portal(Portal3D *p);
    void remove_portal(Portal3D *p);
    const std::vector<Portal3D *> &portals() const { return portals_; }
    void set_max_portal_hops(int n) { max_hops_ = n < 0 ? 0 : n; }
    int max_portal_hops() const { return max_hops_; }

    // --- queries ---------------------------------------------------------------
    // THE ONE THAT KNOWS ABOUT PORTALS. A segment from `from` to `to`
    // that meets an aperture continues out of its partner, up to
    // `max_portal_hops` times.
    RayHit trace(const Vec3 &from, const Vec3 &to, uint32_t mask = 0xFFFFFFFF,
                 const Node3D *ignore = nullptr) const;
    // The same without the portals, for when a caller really does want
    // a straight line -- a shadow query, a broad phase.
    RayHit raycast(const Vec3 &from, const Vec3 &to, uint32_t mask = 0xFFFFFFFF,
                   const Node3D *ignore = nullptr) const;

    // Move a shape along `motion` and stop where it first touches.
    SweepHit sweep(const Shape &shape, const Transform3D &from, const Vec3 &motion,
                   uint32_t mask = 0xFFFFFFFF, const Node3D *ignore = nullptr) const;

    // Push a shape out of anything it is already inside. Returns how
    // many contacts were resolved and writes the correction.
    int depenetrate(const Shape &shape, const Transform3D &at, uint32_t mask,
                    const Node3D *ignore, Vec3 *out_correction,
                    Vec3 *out_deepest_normal = nullptr) const;

    // Everything whose bounds touch `box`.
    void overlap(const AABB &box, uint32_t mask, std::vector<ColliderId> &out) const;

    // ONE TRIANGLE OF THE WORLD, ALREADY IN WORLD SPACE.
    //
    // The solver needs the level as triangles, not as colliders: a
    // box resting on a floor is in contact with two or three of
    // them and each is a separate manifold with its own warm start.
    // Handing back the collider and making the caller dig the mesh
    // out means the mesh table has to be public, and then nothing
    // owns it.
    //
    // `index` is the triangle's index within its mesh, which is
    // what makes a contact on it identifiable from step to step.
    struct WorldTriangle {
        Vec3 v[3];
        ColliderId collider;
        Node3D *node = nullptr;
        uint32_t index = 0;
    };
    void query_triangles(const AABB &box, uint32_t mask,
                         std::vector<WorldTriangle> &out) const;

    // --- crossings ----------------------------------------------------------------
    // Did a segment cross a portal? Returns it, or null, with where.
    Portal3D *crossing(const Vec3 &from, const Vec3 &to, float *out_t = nullptr) const;

    // IS THIS POINT IN A DOORWAY?
    //
    // A portal is a hole in the picture, and the level is one merged
    // triangle mesh with no hole cut in it. Without this, a body walks
    // up to a portal on a wall, is stopped by the wall, and stands
    // there scratching at a doorway it can see straight through.
    //
    // So contacts that land inside an open aperture are discarded, and
    // the wall stops being solid exactly where the portal is. It is
    // the same trick as cutting the hole, done per contact instead of
    // per triangle, and it costs one plane test per portal.
    bool inside_aperture(const Vec3 &world_point, float margin = 0.0f) const;
    // THE SAME, WITH ONE EXCEPTION FOR THE FLOOR.
    //
    // A portal set into a wall has its rectangle reaching down to the
    // ground, and if it is even a centimetre low the rectangle
    // overlaps the floor. Judging by position alone then removes the
    // floor at the threshold, and the player walks up to the doorway
    // and drops through the doorstep -- which reads as the portal
    // teleporting them into the ground.
    //
    // So: A WALL-MOUNTED PORTAL NEVER REMOVES A FLOOR. Anything else
    // inside the rectangle goes, including the underside of the wall
    // itself, which is buried in the floor and would otherwise be the
    // nearest surface to escape towards -- ejecting the body downwards
    // for exactly the same visible result.
    bool inside_aperture(const Vec3 &world_point, const Vec3 &surface_normal,
                         float margin = 0.0f) const;
    // THE SAME, ASKED BY SOMETHING OF A PARTICULAR SIZE.
    //
    // A portal is only a hole for a body that fits through it. Told
    // nothing about the asker, the wall stops being solid for
    // everything -- so a body too large to get through walks up to
    // an aperture it cannot use, finds no wall there either, and
    // stands inside the hole. With an unequal pair that is the
    // common case rather than the exotic one, because making
    // yourself too big to get back is exactly the mistake the
    // mechanic invites.
    //
    // The shape doing the query already knows its own size, which
    // is why this takes the shape rather than a number threaded
    // down from the caller.
    bool inside_aperture(const Vec3 &world_point, const Vec3 &surface_normal,
                         float margin, const Shape *asker) const;
    // Does this shape fit through that aperture. Static because it
    // is a statement about the two of them and nothing else.
    static bool admits_shape(const Portal3D &p, const Shape &s);
    // How far from vertical a portal may lean and still count as
    // wall-mounted, and how flat a surface must be to count as floor.
    float aperture_upright = 0.7f;
    // HOW MUCH OF THE BOTTOM OF AN UPRIGHT APERTURE IS DOORSTEP.
    //
    // A wall-mounted portal must not remove the floor it stands
    // on, or a body walking through drops through the threshold.
    // But "do not remove upward-facing surfaces" is far too broad
    // a way to say that: it keeps solid ANY horizontal surface
    // inside the opening, at any height -- so a ledge, a skirting
    // board, or the top edge of a decorative panel part-way up
    // the wall stays put and becomes an invisible bar across the
    // hole. A body walks up to a portal it plainly fits and is
    // stopped by nothing it can see.
    //
    // The floor is at the BOTTOM of the opening, so only the
    // bottom of the opening is protected. Anything higher than
    // this is geometry the portal has cut through and it goes.
    float aperture_doorstep = 0.3f;

    // THE RIM OF A HOLE IS SOLID.
    //
    // An aperture works by taking the geometry away: contacts
    // inside the rectangle are dropped, so a body can go through
    // a wall that is otherwise there. What that leaves is a hole
    // with no SIDES. A wall is half a metre thick and the hole
    // through it is a short tunnel whose walls stop you moving
    // sideways while you are in it; delete the contacts and the
    // tunnel is gone with them.
    //
    // It shows up worst on a floor portal. You walk onto the
    // hole, you start to fall, and you are still walking -- so
    // you drift out from under the floor before the point that
    // decides you went through has crossed the plane. You end up
    // beneath the level, never having been teleported, which
    // reads as the portal simply not working.
    //
    // So the rim is stated as a constraint: a body whose extent
    // spans an aperture's plane is IN THE MOUTH of it, and is
    // held within the opening until it is out the other side or
    // back the way it came. It is only ever pushed sideways --
    // never along the normal -- so it is still free to go
    // through, or to back out.
    //
    // Returns the corrected centre, which is the one given if
    // the body is not in any aperture's mouth.
    Vec3 hold_in_aperture(const Vec3 &centre, const Vec3 &axis, float half,
                          float radius) const;
    // How thick a wall a portal can be cut through. A contact within
    // this distance of an aperture's plane AND inside its rectangle is
    // discarded. The default covers any ordinary wall; a thicker one
    // needs geometry with a real hole in it, because at some point
    // "the wall is not there" stops being a good description.
    float aperture_thickness = 1.0f;
    // AND THE HOLE IS SLIGHTLY SMALLER THAN THE APERTURE.
    //
    // The floor a portal stands on touches the rectangle's bottom
    // edge exactly. Treat the edge as part of the hole and the floor
    // at the threshold stops being solid, so a body walking through
    // drops through the doorstep -- which looks like the portal
    // teleporting it downwards and is very hard to read as anything
    // else. A few centimetres of solid rim costs nothing and cannot
    // be seen.
    float aperture_edge = 0.03f;

    // --- for scripts ----------------------------------------------------------------
    // The same queries, returning a Dictionary: "hit", "position",
    // "normal", "distance", "portals", "node".
    Dict trace_dict(const Vec3 &from, const Vec3 &to, int64_t mask) const;
    // DOES THIS SEGMENT GO THROUGH A PORTAL, and if so what does
    // that do to it.
    //
    // A body is not the only thing that crosses one. A
    // third-person camera sits at the end of a boom behind the
    // player, and when the player walks through a doorway the
    // boom is what should follow them through it -- the camera
    // stays on the near side looking through the hole until the
    // boom's own tail passes the plane, and only then comes out
    // the other end. Teleporting the camera when the BODY crosses
    // instead makes a doorway into a cut.
    //
    // Returns hit, the fraction along the segment, the warp, and
    // the scale it carries.
    Dict crossing_dict(const Vec3 &from, const Vec3 &to) const;
    Dict raycast_dict(const Vec3 &from, const Vec3 &to, int64_t mask) const;
    // Convenience constructors for the common shapes.
    // THE LEVEL, for a script. add_mesh above takes a reference and
    // a Node3D owner, neither of which crosses the reflection; this
    // is the same call shaped so a game written in Python can make
    // the thing it just built collide.
    int64_t add_mesh_id(Mesh *mesh, const Transform3D &at, int64_t layer);
    int64_t add_sphere(const Vec3 &at, float radius, int64_t layer);
    int64_t add_box(const Transform3D &at, const Vec3 &half, int64_t layer);
    int64_t add_capsule(const Transform3D &at, float radius, float height,
                        int64_t layer);
    void remove_id(int64_t packed) { remove(ColliderId::unpack(packed)); }
    void set_transform_id(int64_t packed, const Transform3D &t) {
        set_transform(ColliderId::unpack(packed), t);
    }

    // --- the moving half ----------------------------------------------------------
    //
    // Collision answers queries; dynamics moves things. They are
    // separate classes because most of a game only needs the first
    // -- a character controller, a raycast, a trigger volume -- and
    // one lives inside the other because a rigid body has to ask
    // the static world what it is standing on. The engine steps it
    // once per physics tick.
    DynamicsWorld &dynamics();

    // --- diagnostics --------------------------------------------------------------
    struct Stats {
        uint32_t traces = 0;
        uint32_t sweeps = 0;
        uint32_t triangle_tests = 0;
        uint32_t portal_hops = 0;
    };
    mutable Stats stats;
    std::string report() const;

private:
    struct Slot {
        Collider collider;
        uint32_t generation = 0;
        bool live = false;
    };
    // Nearest contact between a moving shape and one collider.
    bool sweep_one(const Shape &shape, const Transform3D &from, const Vec3 &motion,
                   const Collider &c, float *out_fraction, Vec3 *out_normal,
                   Vec3 *out_point) const;
    // `search` is how far beyond the shape the caller needs a usable
    // distance for. A sweep advances by the gap it is told about, so
    // if the query only looks as far as the shape's own surface it is
    // told nothing and cannot advance at all -- the shape then falls
    // through anything it was not already touching.
    bool contact_one(const Shape &shape, const Transform3D &at, const Collider &c,
                     Vec3 *out_normal, float *out_depth, float search = 0.0f) const;
    void refresh_bounds(Collider &c);

    std::vector<Slot> slots_;
    std::vector<uint32_t> free_;
    std::vector<TriangleMesh> meshes_;
    std::vector<Portal3D *> portals_;
    std::unique_ptr<DynamicsWorld> dynamics_;
    size_t live_ = 0;
    int max_hops_ = 4;
};

}  // namespace wr
