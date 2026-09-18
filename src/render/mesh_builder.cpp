#include "render/mesh_builder.h"

#include <algorithm>
#include <cmath>

#include "core/bind.h"

namespace wr {

MeshBuilder::MeshBuilder() = default;

MeshBuilder::Part &MeshBuilder::part_for(int slot) {
    for (Part &p : parts_)
        if (p.slot == slot) return p;
    parts_.push_back(Part{slot, {}, {}});
    return parts_.back();
}

void MeshBuilder::emit(const Mesh &src, const Vec2 &uv_size) {
    Part &part = part_for(slot_);
    const uint32_t base = uint32_t(part.vertices.size());
    part.vertices.reserve(part.vertices.size() + src.vertices.size());

    // A normal transforms by the inverse transpose, not the basis.
    // For the uniform scales this engine allows, that is the basis
    // over the square of the scale -- and renormalising afterwards
    // makes the scale drop out entirely.
    const Basis b = xform_.basis;
    const bool tile = uv_scale_ > 0.0f;

    for (Vertex v : src.vertices) {
        v.position = xform_.xform(v.position);
        v.normal = b.xform(v.normal).normalized();
        Vec3 t = b.xform(Vec3(v.tangent.x, v.tangent.y, v.tangent.z));
        float tl = t.length();
        if (tl > EPS) t = t / tl;
        v.tangent = Vec4(t.x, t.y, t.z, v.tangent.w);
        if (tile) {
            v.uv.x *= uv_size.x * uv_scale_;
            v.uv.y *= uv_size.y * uv_scale_;
        }
        v.colour[0] = uint8_t(std::clamp(colour_.r, 0.0f, 1.0f) * 255.0f);
        v.colour[1] = uint8_t(std::clamp(colour_.g, 0.0f, 1.0f) * 255.0f);
        v.colour[2] = uint8_t(std::clamp(colour_.b, 0.0f, 1.0f) * 255.0f);
        v.colour[3] = uint8_t(std::clamp(colour_.a, 0.0f, 1.0f) * 255.0f);
        part.vertices.push_back(v);
    }

    // WINDING SURVIVES A MIRROR. A basis with a negative determinant
    // turns every triangle inside out, and the result is a wall lit
    // from the wrong side that also fails to stop a navigation ray.
    // Flipping the index order puts it back.
    const bool mirrored = b.determinant() < 0.0f;
    part.indices.reserve(part.indices.size() + src.indices.size());
    for (size_t i = 0; i + 2 < src.indices.size(); i += 3) {
        part.indices.push_back(base + src.indices[i]);
        part.indices.push_back(base + src.indices[i + (mirrored ? 2 : 1)]);
        part.indices.push_back(base + src.indices[i + (mirrored ? 1 : 2)]);
    }
}

// ------------------------------------------------------- primitives

void MeshBuilder::add_box(const Vec3 &centre, const Vec3 &size) {
    Ref<Mesh> m = Mesh::box(size);
    Transform3D keep = xform_;
    xform_ = keep * Transform3D(Basis(), centre);
    // Box UVs run 0..1 per face and the faces are not all the same
    // size, so one number cannot be right for all six. The largest
    // pair is the least wrong and keeps big surfaces from smearing.
    emit(*m, Vec2(std::max(size.x, size.z), std::max(size.y, size.z)));
    xform_ = keep;
}

void MeshBuilder::add_bounds(const AABB &b) {
    if (!b.valid()) return;
    add_box(b.center(), b.size());
}

void MeshBuilder::add_plane(const Vec3 &centre, const Vec2 &size,
                            int subdivisions) {
    Ref<Mesh> m = Mesh::plane(size, std::max(1, subdivisions));
    Transform3D keep = xform_;
    xform_ = keep * Transform3D(Basis(), centre);
    emit(*m, size);
    xform_ = keep;
}

void MeshBuilder::add_sphere(const Vec3 &centre, float radius, int rings,
                             int segments) {
    Ref<Mesh> m = Mesh::sphere(radius, std::max(3, rings), std::max(3, segments));
    Transform3D keep = xform_;
    xform_ = keep * Transform3D(Basis(), centre);
    emit(*m, Vec2(radius * TAU, radius * float(PI)));
    xform_ = keep;
}

void MeshBuilder::add_cylinder(const Vec3 &centre, float radius, float height,
                               int segments) {
    Ref<Mesh> m = Mesh::cylinder(radius, height, std::max(3, segments), true);
    Transform3D keep = xform_;
    xform_ = keep * Transform3D(Basis(), centre);
    emit(*m, Vec2(radius * TAU, height));
    xform_ = keep;
}

void MeshBuilder::add_cone(const Vec3 &centre, float radius, float height,
                           int segments) {
    Ref<Mesh> m = Mesh::cone(radius, height, std::max(3, segments));
    Transform3D keep = xform_;
    xform_ = keep * Transform3D(Basis(), centre);
    emit(*m, Vec2(radius * TAU, height));
    xform_ = keep;
}

void MeshBuilder::add_quad(const Vec3 &a, const Vec3 &b, const Vec3 &c,
                           const Vec3 &d) {
    Part &part = part_for(slot_);
    const uint32_t base = uint32_t(part.vertices.size());
    Vec3 n = cross(b - a, c - a);
    float nl = n.length();
    n = nl > EPS ? n / nl : Vec3(0, 1, 0);

    // UVs from the quad's own edge lengths, so a long thin one does
    // not get a square of texture squashed onto it.
    const float w = (b - a).length(), h = (d - a).length();
    const float su = uv_scale_ > 0.0f ? uv_scale_ : 1.0f;
    const Vec2 uv[4] = {Vec2(0, 0), Vec2(w * su, 0), Vec2(w * su, h * su),
                        Vec2(0, h * su)};
    const Vec3 corner[4] = {a, b, c, d};
    for (int i = 0; i < 4; ++i) {
        Vertex v;
        v.position = xform_.xform(corner[i]);
        v.normal = xform_.basis.xform(n).normalized();
        v.uv = uv[i];
        v.colour[0] = uint8_t(std::clamp(colour_.r, 0.0f, 1.0f) * 255.0f);
        v.colour[1] = uint8_t(std::clamp(colour_.g, 0.0f, 1.0f) * 255.0f);
        v.colour[2] = uint8_t(std::clamp(colour_.b, 0.0f, 1.0f) * 255.0f);
        v.colour[3] = uint8_t(std::clamp(colour_.a, 0.0f, 1.0f) * 255.0f);
        part.vertices.push_back(v);
    }
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    for (uint32_t i : idx) part.indices.push_back(base + i);
}

void MeshBuilder::add_mesh(Mesh *m) {
    if (m) emit(*m, Vec2(1, 1));
}

// -------------------------------------------------- rooms and streets

void MeshBuilder::add_wall(const Vec3 &from, const Vec3 &to, float height,
                           float thickness) {
    Vec3 along = to - from;
    along.y = 0.0f;
    const float len = along.length();
    if (len < 1e-4f || height <= 0.0f) return;

    // The wall's own frame: +x along it, +y up, +z across it. Built
    // this way rather than as an axis-aligned box so a wall can run
    // at any angle, which a town laid out on a grid does not need
    // and a town with a diagonal street does.
    Vec3 x = along / len;
    Vec3 z = cross(Vec3(0, 1, 0), x).normalized();
    Basis b;
    b.col[0] = x;
    b.col[1] = Vec3(0, 1, 0);
    b.col[2] = z;

    // Standing ON the ground: the centre is half a height up from
    // the base, not at the base.
    Vec3 mid = (from + to) * 0.5f;
    mid.y = std::min(from.y, to.y) + height * 0.5f;

    Transform3D keep = xform_;
    xform_ = keep * Transform3D(b, mid);
    Ref<Mesh> m = Mesh::box(Vec3(len, height, thickness));
    emit(*m, Vec2(len, height));
    xform_ = keep;
}

void MeshBuilder::add_room(const AABB &floor, float height, float thickness) {
    if (!floor.valid()) return;
    // The floor slab, with its TOP at floor.max.y -- so a room given
    // a plan at y = 0 has its floor surface at 0 and everything
    // stands on it, rather than half a slab proud of it.
    //
    // It is as thick as the plan's own y extent, falling back to the
    // wall thickness when the plan is flat. Using the number that
    // was given beats ignoring it: a caller who wrote a slab from
    // -0.5 to 0 meant a slab from -0.5 to 0.
    Vec3 size = floor.size();
    const float slab = std::max(std::max(size.y, thickness), 0.05f);
    add_box(Vec3(floor.center().x, floor.max.y - slab * 0.5f, floor.center().z),
            Vec3(size.x + thickness * 2.0f, slab, size.z + thickness * 2.0f));

    const float y = floor.max.y;
    const float hx = size.x * 0.5f + thickness * 0.5f;
    const float hz = size.z * 0.5f + thickness * 0.5f;
    const Vec3 c = floor.center();
    add_wall(Vec3(c.x - hx, y, c.z - hz), Vec3(c.x + hx, y, c.z - hz), height,
             thickness);
    add_wall(Vec3(c.x + hx, y, c.z + hz), Vec3(c.x - hx, y, c.z + hz), height,
             thickness);
    add_wall(Vec3(c.x - hx, y, c.z + hz), Vec3(c.x - hx, y, c.z - hz), height,
             thickness);
    add_wall(Vec3(c.x + hx, y, c.z - hz), Vec3(c.x + hx, y, c.z + hz), height,
             thickness);
}

void MeshBuilder::add_stairs(const AABB &bounds, int steps, bool along_x) {
    if (!bounds.valid() || steps < 1) return;
    const Vec3 size = bounds.size();
    const float run = (along_x ? size.x : size.z) / float(steps);
    const float rise = size.y / float(steps);
    for (int i = 0; i < steps; ++i) {
        const float top = bounds.min.y + rise * float(i + 1);
        // FROM THE GROUND UP, not a floating tread. The navigation
        // bake decides what can be climbed from the top of the solid
        // below, and a tread with nothing under it is a tread with a
        // drop beside it -- which is a ledge, and gets filtered out.
        AABB step;
        if (along_x) {
            step = AABB(Vec3(bounds.min.x + run * float(i), bounds.min.y,
                             bounds.min.z),
                        Vec3(bounds.min.x + run * float(i + 1), top,
                             bounds.max.z));
        } else {
            step = AABB(Vec3(bounds.min.x, bounds.min.y,
                             bounds.min.z + run * float(i)),
                        Vec3(bounds.max.x, top,
                             bounds.min.z + run * float(i + 1)));
        }
        add_bounds(step);
    }
}

// ------------------------------------------------------------ output

Mesh *MeshBuilder::build() {
    Ref<Mesh> m(new Mesh());
    // Slots in the order they were first used, so a caller can put
    // materials in the same order it named them.
    for (const Part &p : parts_) {
        if (p.indices.empty()) continue;
        m->append(p.vertices, p.indices, p.slot,
                  "slot" + std::to_string(p.slot));
    }
    m->compute_tangents();
    m->compute_bounds();
    built_ = m;
    return m.get();
}

void MeshBuilder::clear() {
    parts_.clear();
    built_ = Ref<Mesh>();
}

int MeshBuilder::vertex_count() const {
    size_t n = 0;
    for (const Part &p : parts_) n += p.vertices.size();
    return int(n);
}

int MeshBuilder::triangle_count() const {
    size_t n = 0;
    for (const Part &p : parts_) n += p.indices.size();
    return int(n / 3);
}

Array MeshBuilder::get_slots() const {
    Array a;
    a.reserve(parts_.size());
    for (const Part &p : parts_) a.push_back(Variant(p.slot));
    return a;
}

static void register_mesh_builder() {
    ClassBuilder<MeshBuilder>()
        .prop("colour", &MeshBuilder::get_colour, &MeshBuilder::set_colour)
        .prop("slot", &MeshBuilder::get_slot, &MeshBuilder::set_slot)
        .prop("transform", &MeshBuilder::get_transform,
              &MeshBuilder::set_transform)
        .prop("uv_scale", &MeshBuilder::get_uv_scale, &MeshBuilder::set_uv_scale)
        .method("add_box", &MeshBuilder::add_box).args("centre", "size")
        .method("add_bounds", &MeshBuilder::add_bounds).args("bounds")
        .method("add_plane", &MeshBuilder::add_plane, {Variant(1)})
            .args("centre", "size", "subdivisions")
        .method("add_sphere", &MeshBuilder::add_sphere,
                {Variant(16), Variant(24)})
            .args("centre", "radius", "rings", "segments")
        .method("add_cylinder", &MeshBuilder::add_cylinder, {Variant(20)})
            .args("centre", "radius", "height", "segments")
        .method("add_cone", &MeshBuilder::add_cone, {Variant(20)})
            .args("centre", "radius", "height", "segments")
        .method("add_quad", &MeshBuilder::add_quad).args("a", "b", "c", "d")
        .method("add_mesh", &MeshBuilder::add_mesh).args("mesh")
        .method("add_wall", &MeshBuilder::add_wall, {Variant(0.25)})
            .args("from_point", "to_point", "height", "thickness")
        .method("add_room", &MeshBuilder::add_room, {Variant(0.25)})
            .args("floor", "height", "thickness")
        .method("add_stairs", &MeshBuilder::add_stairs, {Variant(true)})
            .args("bounds", "steps", "along_x")
        .method("build", &MeshBuilder::build)
        .method("clear", &MeshBuilder::clear)
        .method("vertex_count", &MeshBuilder::vertex_count)
        .method("triangle_count", &MeshBuilder::triangle_count)
        .method("get_slots", &MeshBuilder::get_slots);
}
WR_REGISTER(register_mesh_builder)

}  // namespace wr
