#include "portal.h"

#include <algorithm>

#include "core/log.h"

namespace mf {

Portal3D::Portal3D() { set_name("Portal3D"); }

void Portal3D::on_exit_tree() {
    // A portal leaving the tree must not leave its partner pointing at
    // a node that is about to be freed.
    unlink();
}

void Portal3D::link_to(Portal3D *other) {
    if (other == this) {
        MF_ERROR("%s: a portal cannot link to itself", name().c_str());
        return;
    }
    if (link_ == other) return;
    unlink();
    if (!other) return;
    other->unlink();
    link_ = other;
    other->link_ = this;
}

void Portal3D::unlink() {
    if (!link_) return;
    Portal3D *other = link_;
    link_ = nullptr;
    if (other->link_ == this) other->link_ = nullptr;
}

Plane Portal3D::plane() const {
    Transform3D g = global_transform();
    return Plane(g.basis.z().normalized(), g.origin);
}

void Portal3D::corners(Vec3 out[4]) const {
    Transform3D g = global_transform();
    float hw = width * 0.5f, hh = height * 0.5f;
    out[0] = g.xform({-hw, -hh, 0});
    out[1] = g.xform({hw, -hh, 0});
    out[2] = g.xform({hw, hh, 0});
    out[3] = g.xform({-hw, hh, 0});
}

AABB Portal3D::bounds() const {
    Vec3 c[4];
    corners(c);
    AABB b;
    for (const Vec3 &p : c) b.expand(p);
    return b;
}

// ---------------------------------------------------------------- the warp

float Portal3D::scale_ratio(const Portal3D *from, const Portal3D *to) {
    if (!from || !to) return 1.0f;
    float a = from->world_width();
    float b = to->world_width();
    if (a < 1e-5f) return 1.0f;
    return b / a;
}

Transform3D Portal3D::warp(const Portal3D *from, const Portal3D *to) {
    if (!from || !to) return Transform3D::identity();

    // Into `from`'s local frame. The inverse of the global transform
    // removes whatever uniform scale the node carries, so the result
    // is in aperture units.
    Transform3D inv = from->global_transform().inverse();

    // THE HALF TURN. About the aperture's up axis, so that going in
    // facing the front of A is coming out facing away from the front
    // of B. Turning about the other axis would work too and would put
    // the world upside down.
    Transform3D flip(Basis::from_axis_angle(Vec3::up(), PI), Vec3());

    // THE RATIO, and the whole point of the class. `width` is in local
    // units on both ends and the node scales are already accounted for
    // by the inverse above and the multiply below, so the local widths
    // are what belong here.
    float s = from->width > 1e-5f ? to->width / from->width : 1.0f;
    Transform3D resize(Basis::uniform(s), Vec3());

    return to->global_transform() * resize * flip * inv;
}

// ------------------------------------------------------------- traversal

bool Portal3D::crossed(const Vec3 &from, const Vec3 &to, float *t_out) const {
    Plane p = plane();
    float a = p.distance_to(from);
    float b = p.distance_to(to);
    // FRONT TO BACK ONLY. Coming at the rear face is walking into the
    // back of a hole: it is a wall.
    if (!(a > 0.0f && b <= 0.0f)) return false;
    float den = a - b;
    if (std::fabs(den) < EPS) return false;
    float t = a / den;
    if (t < 0.0f || t > 1.0f) return false;
    if (!within_aperture(lerp(from, to, t))) return false;
    if (t_out) *t_out = t;
    return true;
}

bool Portal3D::within_aperture(const Vec3 &world_point, float margin) const {
    Transform3D g = global_transform();
    Vec3 local = g.inverse().xform(world_point);
    return std::fabs(local.x) <= width * 0.5f + margin &&
           std::fabs(local.y) <= height * 0.5f + margin;
}

Vec3 Portal3D::closest_point(const Vec3 &world_point) const {
    Transform3D g = global_transform();
    Vec3 local = g.inverse().xform(world_point);
    local.x = clampf(local.x, -width * 0.5f, width * 0.5f);
    local.y = clampf(local.y, -height * 0.5f, height * 0.5f);
    local.z = 0.0f;
    return g.xform(local);
}

bool Portal3D::camera_is_close(const Vec3 &eye, float near_plane) const {
    // The near plane is a sphere's worth of slack around the eye; if
    // the aperture is within it, the near plane can slice the portal
    // surface and the seam shows.
    float d = std::fabs(plane().distance_to(eye));
    if (d > near_plane * 2.5f) return false;
    return within_aperture(eye, near_plane * 2.5f);
}

// ------------------------------------------------------------ the screen

bool Portal3D::screen_rect(const Transform3D &view, const Projection &proj,
                           Rect2 *out) const {
    Vec3 c[4];
    corners(c);

    // CLIP AGAINST THE NEAR PLANE FIRST. A corner behind the eye
    // projects to a point on the far side of infinity, and a bounding
    // box that includes it is a bounding box of the whole world -- the
    // classic way a portal you are standing in ends up scissored to
    // nothing or to everything.
    Vec3 v[4];
    for (int i = 0; i < 4; i++) v[i] = view.xform(c[i]);

    Vec3 poly[8];
    int n = 0;
    const float eps = 1e-3f;
    for (int i = 0; i < 4; i++) {
        const Vec3 &a = v[i];
        const Vec3 &b = v[(i + 1) & 3];
        bool ina = a.z <= -eps, inb = b.z <= -eps;
        if (ina) poly[n++] = a;
        if (ina != inb) {
            float t = (-eps - a.z) / (b.z - a.z);
            poly[n++] = lerp(a, b, t);
        }
        if (n >= 8) break;
    }
    if (n < 3) return false;

    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (int i = 0; i < n; i++) {
        Vec4 clip = proj.xform(Vec4(poly[i], 1.0f));
        if (clip.w <= 1e-6f) continue;
        float nx = clip.x / clip.w;
        float ny = clip.y / clip.w;
        x0 = std::min(x0, nx);
        x1 = std::max(x1, nx);
        y0 = std::min(y0, ny);
        y1 = std::max(y1, ny);
    }
    if (x1 <= x0 || y1 <= y0) return false;

    // NDC to [0,1], y down, clamped to the screen.
    float sx0 = clampf(x0 * 0.5f + 0.5f, 0.0f, 1.0f);
    float sx1 = clampf(x1 * 0.5f + 0.5f, 0.0f, 1.0f);
    float sy0 = clampf(0.5f - y1 * 0.5f, 0.0f, 1.0f);
    float sy1 = clampf(0.5f - y0 * 0.5f, 0.0f, 1.0f);
    if (sx1 - sx0 < 1e-5f || sy1 - sy0 < 1e-5f) return false;
    if (out) *out = Rect2({sx0, sy0}, {sx1 - sx0, sy1 - sy0});
    return true;
}

// ------------------------------------------------------------ reflection

static void register_portal_class() {
    ClassBuilder<Portal3D>()
        .field("width", &Portal3D::width, "range:0.05,64")
        .field("height", &Portal3D::height, "range:0.05,64")
        .field("active", &Portal3D::active)
        .field("max_recursion", &Portal3D::max_recursion, "range:0,8")
        .field("edge_colour", &Portal3D::edge_colour)
        .field("edge_width", &Portal3D::edge_width, "range:0,0.5")
        .field("open", &Portal3D::open, "range:0,1")
        .method("link_to", &Portal3D::link_to).args("other")
        .method("unlink", &Portal3D::unlink)
        .method("get_link", &Portal3D::link)
        .method("is_linked", &Portal3D::linked)
        .method("get_normal", &Portal3D::normal)
        .method("get_plane", &Portal3D::plane)
        .method("get_bounds", &Portal3D::bounds)
        .method("world_width", &Portal3D::world_width)
        .method("world_height", &Portal3D::world_height)
        .method("warp_out", &Portal3D::warp_out)
        .method("scale_out", &Portal3D::scale_out)
        .method("within_aperture", &Portal3D::within_aperture, {Variant(0.0)}).args("world_point", "margin")
        .method("closest_point", &Portal3D::closest_point).args("world_point")
        .method("faces", &Portal3D::faces).args("point")
        .signal("traversed");
}
MF_REGISTER(register_portal_class)

}  // namespace mf
