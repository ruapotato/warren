#include "scene/nav_nodes.h"

#include <algorithm>
#include <cmath>
#include <random>

#include "core/bind.h"
#include "core/log.h"
#include "scene/nodes.h"

namespace wr {

// --------------------------------------------------------- the region

NavRegion3D::NavRegion3D() { set_physics_processing(true); }

void NavRegion3D::set_mesh(nav::NavMesh *m) {
    mesh_ = Ref<nav::NavMesh>(m);
    crowd_.set_navmesh(mesh_.get());
}

void NavRegion3D::gather(Node *n, std::vector<Vec3> *tris) const {
    if (!n) return;
    // OPTING OUT, by group rather than by a flag on MeshInstance3D.
    //
    // Plenty of geometry should not be baked: a decorative grille a
    // body walks through, a glass pane, a volume used for triggers,
    // a skybox shell. A group says so without putting a navigation
    // field on a class that has nothing to do with navigation, and
    // it applies to a whole subtree, which is how such things are
    // usually arranged anyway.
    if (n->in_group("no-navigation")) return;

    if (MeshInstance3D *mi = n->cast_to<MeshInstance3D>()) {
        // Hidden geometry is geometry the player cannot see and
        // cannot walk into. Baking it puts invisible walls in the
        // level, which is a bug that takes a long time to find.
        if (mi->mesh && mi->visible_in_tree()) {
            const Transform3D world = mi->global_transform();
            const Mesh &m = *mi->mesh;
            tris->reserve(tris->size() + m.indices.size());
            for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
                tris->push_back(world * m.vertices[m.indices[i + 0]].position);
                tris->push_back(world * m.vertices[m.indices[i + 1]].position);
                tris->push_back(world * m.vertices[m.indices[i + 2]].position);
            }
        }
    }
    for (const Ref<Node> &c : n->children()) gather(c.get(), tris);
}

bool NavRegion3D::bake(Node *source) {
    std::vector<Vec3> tris;
    gather(source ? source : this, &tris);
    if (tris.empty()) {
        WR_WARN("nav: nothing to bake under '%s'", name().c_str());
        return false;
    }
    AABB bounds;
    for (const Vec3 &v : tris) bounds.expand(v);
    // A little room all round, so the outermost cells have
    // neighbours to be compared against rather than falling off the
    // edge of the volume.
    bounds = bounds.grown(settings.agent.radius * 2.0f + settings.cell_size * 2.0f);
    return bake_within(bounds, source);
}

bool NavRegion3D::bake_within(const AABB &bounds, Node *source) {
    std::vector<Vec3> tris;
    gather(source ? source : this, &tris);
    if (tris.empty()) return false;

    Ref<nav::NavMesh> fresh(new nav::NavMesh());
    nav::BakeStats st;
    if (!fresh->bake(tris, bounds, settings, &st)) {
        WR_ERROR("nav: bake failed for '%s'; keeping the previous mesh",
                 name().c_str());
        return false;
    }
    mesh_ = fresh;
    stats_ = st;
    crowd_.set_navmesh(mesh_.get());
    collect_links(source);

    WR_INFO("nav: '%s' baked %d polys from %zu triangles in %.0f ms "
            "(%d regions, %d holes bridged, %d unreachable pruned)",
            name().c_str(), st.polys, tris.size() / 3, double(st.seconds * 1000),
            st.regions, st.merged_holes, st.pruned);
    return true;
}

void NavRegion3D::collect_links(Node *source) {
    if (!mesh_) return;
    mesh_->clear_links();
    std::vector<Node *> stack{source ? source : this};
    while (!stack.empty()) {
        Node *n = stack.back();
        stack.pop_back();
        if (NavLink3D *l = n->cast_to<NavLink3D>()) mesh_->add_link(l->resolve());
        for (const Ref<Node> &c : n->children()) stack.push_back(c.get());
    }
    mesh_->resolve_links();
}

bool NavRegion3D::nearest_point(const Vec3 &p, Vec3 *out) const {
    if (!mesh_) return false;
    return mesh_->nearest_point(p, Vec3(4.0f, 2.0f, 4.0f), out);
}

bool NavRegion3D::random_point(const Vec3 &near, float radius,
                               Vec3 *out) const {
    if (!mesh_ || mesh_->empty()) return false;
    // Rejection sampling in the disc, falling back to the nearest
    // point on the mesh. Twenty tries is plenty where there is any
    // navmesh at all, and where there is not, the fallback is the
    // honest answer rather than a failure the caller must handle.
    static thread_local std::mt19937 rng{0x5eed};
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    for (int i = 0; i < 20; ++i) {
        Vec3 p = near + Vec3(unit(rng) * radius, 0.0f, unit(rng) * radius);
        uint16_t poly = mesh_->find_poly(p, Vec3(0.5f, 2.0f, 0.5f));
        if (poly == nav::kNoPoly) continue;
        float y = p.y;
        mesh_->height_at(poly, p, &y);
        *out = Vec3(p.x, y, p.z);
        return true;
    }
    return mesh_->nearest_point(near, Vec3(radius, 4.0f, radius), out);
}

Array NavRegion3D::find_path(const Vec3 &from, const Vec3 &to) const {
    Array out;
    if (!mesh_) return out;
    std::vector<nav::PathPoint> path;
    if (!mesh_->find_path(from, to, &path)) return out;
    out.reserve(path.size());
    for (const nav::PathPoint &p : path) out.push_back(Variant(p.position));
    return out;
}

bool NavRegion3D::is_reachable(const Vec3 &from, const Vec3 &to) const {
    if (!mesh_) return false;
    std::vector<nav::PathPoint> path;
    bool partial = true;
    return mesh_->find_path(from, to, &path, &partial) && !partial;
}

void NavRegion3D::on_ready() {
    // NOT A BAKE HERE, a request for one.
    //
    // _ready fires the moment a node enters the tree, and its
    // contract is that the node's own subtree is present -- which
    // it is, and which is not the same as the LEVEL being present.
    // A region added to an empty scene and populated afterwards, or
    // one built alongside its geometry, would bake nothing and warn
    // about it. Waiting for the first tick costs a frame with no
    // navmesh and makes the order things are added in stop
    // mattering, which is the trade worth making.
    //
    // A region handed a mesh, from a file or from a tool, keeps it:
    // baking a town takes a moment, and spending that moment at
    // every level load is a moment nobody asked for.
    want_bake_ = !mesh_ || mesh_->empty();
}

void NavRegion3D::on_physics(float dt) {
    if (want_bake_) {
        want_bake_ = false;
        bake();
    }
    if (active && mesh_) crowd_.step(dt);
}

int NavRegion3D::set_area_in(const AABB &box, int set_bits, int clear_bits) {
    if (!mesh_) return 0;
    return mesh_->set_area_in(box, uint16_t(set_bits), uint16_t(clear_bits));
}

int NavRegion3D::area_at(const Vec3 &p) const {
    return mesh_ ? int(mesh_->area_at(p, Vec3(1.0f, 2.0f, 1.0f))) : 0;
}

Vec3 NavRegion3D::nearest_point_or(const Vec3 &p) const {
    Vec3 out = p;
    nearest_point(p, &out);
    return out;
}

Vec3 NavRegion3D::random_point_or(const Vec3 &near, float radius) const {
    Vec3 out = near;
    random_point(near, radius, &out);
    return out;
}

// ----------------------------------------------------------- the link

nav::NavLink NavLink3D::resolve() const {
    nav::NavLink l;
    l.from = world_start();
    l.to = world_end();
    l.radius = radius;
    l.cost = cost;
    l.bidirectional = bidirectional;
    l.area = area;
    l.name = name();
    return l;
}

// ---------------------------------------------------------- the agent

NavAgent3D::~NavAgent3D() { leave(); }

void NavAgent3D::join() {
    if (region_) return;
    for (Node *n = parent(); n; n = n->parent()) {
        if (NavRegion3D *r = n->cast_to<NavRegion3D>()) {
            region_ = r;
            break;
        }
    }
    if (!region_) {
        WR_WARN("nav: agent '%s' has no NavRegion3D above it; it will "
                "not move",
                name().c_str());
        return;
    }
    nav::CrowdAgent a;
    a.position = global_position();
    a.radius = radius;
    a.height = height;
    a.max_speed = max_speed;
    a.max_accel = max_accel;
    a.goal_radius = goal_radius;
    a.avoidance = avoidance;
    a.layer = layer;
    a.mask = mask;
    a.filter.include = uint16_t(nav_include);
    a.filter.exclude = uint16_t(nav_exclude);
    id_ = region_->crowd().add(a);
    if (pending_) {
        region_->crowd().set_target(id_, pending_target_);
        pending_ = false;
    }
}

void NavAgent3D::leave() {
    if (region_ && id_) region_->crowd().remove(id_);
    region_ = nullptr;
    id_ = 0;
}

const nav::CrowdAgent *NavAgent3D::me() const {
    return region_ && id_ ? region_->crowd().find(id_) : nullptr;
}
nav::CrowdAgent *NavAgent3D::me() {
    return region_ && id_ ? region_->crowd().find(id_) : nullptr;
}

void NavAgent3D::on_ready() {
    set_physics_processing(true);
    join();
}
void NavAgent3D::on_exit_tree() { leave(); }

void NavAgent3D::set_target(const Vec3 &p) {
    if (!region_) {
        // Asked before the tree was ready. Remember it rather than
        // dropping it, so a game can set a target in its own setup
        // without caring about node order.
        pending_target_ = p;
        pending_ = true;
        return;
    }
    region_->crowd().set_target(id_, p);
}

void NavAgent3D::stop() {
    pending_ = false;
    if (region_) region_->crowd().stop(id_);
}

bool NavAgent3D::has_target() const {
    const nav::CrowdAgent *a = me();
    return a ? a->has_target : pending_;
}
bool NavAgent3D::arrived() const {
    const nav::CrowdAgent *a = me();
    return a && a->arrived;
}
bool NavAgent3D::path_partial() const {
    const nav::CrowdAgent *a = me();
    return a && a->path_partial;
}
Vec3 NavAgent3D::velocity() const {
    const nav::CrowdAgent *a = me();
    return a ? a->velocity : Vec3();
}
Vec3 NavAgent3D::desired_position() const {
    const nav::CrowdAgent *a = me();
    return a ? a->position : global_position();
}
bool NavAgent3D::on_link() const {
    const nav::CrowdAgent *a = me();
    return a && a->on_link;
}
uint16_t NavAgent3D::link_area() const {
    const nav::CrowdAgent *a = me();
    if (!a || !a->on_link || !region_ || !region_->mesh()) return 0;
    const std::vector<nav::NavLink> &links = region_->mesh()->links();
    return a->link < links.size() ? links[a->link].area : 0;
}
float NavAgent3D::deflection() const {
    const nav::CrowdAgent *a = me();
    return a ? a->deflection : 0.0f;
}
float NavAgent3D::stuck_time() const {
    const nav::CrowdAgent *a = me();
    return a ? a->stuck_for : 0.0f;
}

void NavAgent3D::on_physics(float dt) {
    (void)dt;
    nav::CrowdAgent *a = me();
    if (!a) {
        join();
        return;
    }
    // Settings can change between frames -- a zombie that has just
    // been enraged is faster -- so they are pushed down each step
    // rather than only at join.
    a->radius = radius;
    a->height = height;
    a->max_speed = max_speed;
    a->max_accel = max_accel;
    a->goal_radius = goal_radius;
    a->avoidance = avoidance;
    a->layer = layer;
    a->mask = mask;
    a->filter.include = uint16_t(nav_include);
    a->filter.exclude = uint16_t(nav_exclude);

    if (drives_transform) {
        set_global_position(a->position);
    } else {
        // The game is driving. Tell the crowd where the body really
        // is, or its avoidance is solving for a body that is not
        // there. This is also what makes a character controller's
        // collisions and the crowd agree.
        a->position = global_position();
    }
}

// ------------------------------------------------------- reflection

static void register_nav_nodes() {
    ClassBuilder<NavRegion3D>()
        .field("active", &NavRegion3D::active)
        .method("bake", &NavRegion3D::bake_now)
        .method("poly_count", &NavRegion3D::poly_count)
        .method("bake_seconds", &NavRegion3D::bake_seconds)
        .method("collect_links", &NavRegion3D::collect_links_now)
        .method("set_area_in", &NavRegion3D::set_area_in)
            .args("box", "set_bits", "clear_bits")
        .method("area_at", &NavRegion3D::area_at).args("point")
        .method("find_path", &NavRegion3D::find_path).args("from", "to")
        .method("is_reachable", &NavRegion3D::is_reachable).args("from", "to")
        .method("nearest_point", &NavRegion3D::nearest_point_or).args("point")
        .method("random_point", &NavRegion3D::random_point_or)
            .args("near", "radius");

    ClassBuilder<NavLink3D>()
        .field("start", &NavLink3D::start)
        .field("end", &NavLink3D::end)
        .field("radius", &NavLink3D::radius, "range:0.1,8")
        .field("cost", &NavLink3D::cost, "range:0,64")
        .field("bidirectional", &NavLink3D::bidirectional)
        .method("world_start", &NavLink3D::world_start)
        .method("world_end", &NavLink3D::world_end);

    ClassBuilder<NavAgent3D>()
        .field("radius", &NavAgent3D::radius, "range:0.05,4")
        .field("height", &NavAgent3D::height, "range:0.2,8")
        .field("max_speed", &NavAgent3D::max_speed, "range:0,32")
        .field("max_accel", &NavAgent3D::max_accel, "range:0,128")
        .field("goal_radius", &NavAgent3D::goal_radius, "range:0,32")
        .field("avoidance", &NavAgent3D::avoidance)
        .field("nav_include", &NavAgent3D::nav_include)
        .field("nav_exclude", &NavAgent3D::nav_exclude)
        .field("drives_transform", &NavAgent3D::drives_transform)
        .method("set_target", &NavAgent3D::set_target).args("point")
        .method("stop", &NavAgent3D::stop)
        .method("has_target", &NavAgent3D::has_target)
        .method("arrived", &NavAgent3D::arrived)
        .method("path_partial", &NavAgent3D::path_partial)
        .method("velocity", &NavAgent3D::velocity)
        .method("on_link", &NavAgent3D::on_link)
        .method("deflection", &NavAgent3D::deflection)
        .method("stuck_time", &NavAgent3D::stuck_time);
}
WR_REGISTER(register_nav_nodes)

}  // namespace wr
