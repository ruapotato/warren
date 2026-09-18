#include "mesh.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>

#include "core/log.h"

namespace mf {

rhi::VertexLayout standard_vertex_layout() {
    using namespace rhi;
    VertexLayout vl;
    vl.bindings.push_back({0, uint32_t(sizeof(Vertex)), false});
    vl.attributes.push_back({0, 0, Format::RGB32F, offsetof(Vertex, position)});
    vl.attributes.push_back({1, 0, Format::RGB32F, offsetof(Vertex, normal)});
    vl.attributes.push_back({2, 0, Format::RGBA32F, offsetof(Vertex, tangent)});
    vl.attributes.push_back({3, 0, Format::RG32F, offsetof(Vertex, uv)});
    vl.attributes.push_back({4, 0, Format::RGBA8, offsetof(Vertex, colour)});
    return vl;
}

Mesh::~Mesh() {
    if (owner_) release(owner_);
}

void Mesh::clear() {
    vertices.clear();
    indices.clear();
    skin.clear();
    submeshes.clear();
    bounds_ = AABB();
}

int Mesh::append(const std::vector<Vertex> &v, const std::vector<uint32_t> &i,
                 int material_slot, const std::string &name) {
    SubMesh sm;
    sm.first_index = uint32_t(indices.size());
    sm.index_count = uint32_t(i.size());
    sm.material_slot = material_slot;
    sm.name = name;
    uint32_t base = uint32_t(vertices.size());
    vertices.insert(vertices.end(), v.begin(), v.end());
    indices.reserve(indices.size() + i.size());
    for (uint32_t idx : i) indices.push_back(base + idx);
    for (const Vertex &vert : v) sm.bounds.expand(vert.position);
    submeshes.push_back(sm);
    bounds_.expand(sm.bounds);
    return int(submeshes.size()) - 1;
}

void Mesh::transform(const Transform3D &t) {
    // A direction transforms by the basis; a normal by the inverse
    // transpose. They are only the same under a uniform scale, and
    // this must survive a non-uniform one because an importer may hand
    // us anything.
    Basis nb = t.basis.inverse().transposed();
    for (Vertex &v : vertices) {
        v.position = t.xform(v.position);
        v.normal = nb.xform(v.normal).normalized();
        Vec3 tg = t.basis.xform(v.tangent.xyz()).normalized();
        v.tangent = Vec4(tg, v.tangent.w);
    }
    compute_bounds();
}

void Mesh::compute_bounds() {
    bounds_ = AABB();
    for (const Vertex &v : vertices) bounds_.expand(v.position);
    for (SubMesh &sm : submeshes) {
        sm.bounds = AABB();
        for (uint32_t i = sm.first_index; i < sm.first_index + sm.index_count; i++)
            if (i < indices.size() && indices[i] < vertices.size())
                sm.bounds.expand(vertices[indices[i]].position);
    }
}

// Area-weighted face normals accumulated per position, then split
// wherever two faces meet at more than `smooth_angle`. Area weighting
// matters: an unweighted average lets a sliver triangle pull a normal
// as hard as the large face beside it.
void Mesh::compute_normals(float smooth_angle) {
    const float cos_limit = std::cos(smooth_angle);
    struct FaceN {
        Vec3 n;
        float area;
    };
    std::vector<FaceN> faces(indices.size() / 3);
    for (size_t f = 0; f < faces.size(); f++) {
        const Vec3 &a = vertices[indices[f * 3 + 0]].position;
        const Vec3 &b = vertices[indices[f * 3 + 1]].position;
        const Vec3 &c = vertices[indices[f * 3 + 2]].position;
        Vec3 cr = cross(b - a, c - a);
        float len = cr.length();
        faces[f].area = len * 0.5f;
        faces[f].n = len > EPS ? cr / len : Vec3::up();
    }

    // Which faces touch each vertex.
    std::vector<std::vector<uint32_t>> touching(vertices.size());
    for (size_t f = 0; f < faces.size(); f++)
        for (int k = 0; k < 3; k++) touching[indices[f * 3 + k]].push_back(uint32_t(f));

    for (size_t v = 0; v < vertices.size(); v++) {
        if (touching[v].empty()) continue;
        // Anchor on the largest face so the smoothing group is decided
        // by the dominant surface, not by whichever face came first.
        uint32_t anchor = touching[v][0];
        for (uint32_t f : touching[v])
            if (faces[f].area > faces[anchor].area) anchor = f;
        Vec3 sum;
        for (uint32_t f : touching[v])
            if (dot(faces[f].n, faces[anchor].n) >= cos_limit)
                sum += faces[f].n * faces[f].area;
        vertices[v].normal = sum.length_sq() > EPS ? sum.normalized() : faces[anchor].n;
    }
}

// Lengyel's method: accumulate per-triangle tangent and bitangent from
// the UV derivatives, then Gram-Schmidt against the normal and store
// the handedness in w.
void Mesh::compute_tangents() {
    std::vector<Vec3> tan(vertices.size());
    std::vector<Vec3> bitan(vertices.size());

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        uint32_t i0 = indices[i], i1 = indices[i + 1], i2 = indices[i + 2];
        const Vertex &v0 = vertices[i0], &v1 = vertices[i1], &v2 = vertices[i2];
        Vec3 e1 = v1.position - v0.position;
        Vec3 e2 = v2.position - v0.position;
        Vec2 d1 = v1.uv - v0.uv;
        Vec2 d2 = v2.uv - v0.uv;
        float det = d1.x * d2.y - d2.x * d1.y;
        // A degenerate UV triangle contributes nothing rather than an
        // infinity that poisons every vertex it touches.
        if (std::fabs(det) < 1e-12f) continue;
        float r = 1.0f / det;
        Vec3 t = (e1 * d2.y - e2 * d1.y) * r;
        Vec3 b = (e2 * d1.x - e1 * d2.x) * r;
        tan[i0] += t; tan[i1] += t; tan[i2] += t;
        bitan[i0] += b; bitan[i1] += b; bitan[i2] += b;
    }

    for (size_t v = 0; v < vertices.size(); v++) {
        const Vec3 &n = vertices[v].normal;
        Vec3 t = tan[v];
        if (t.length_sq() < EPS) {
            // No UV gradient here at all; any tangent perpendicular to
            // the normal is as good as another and better than zero.
            t = any_perpendicular(n);
        }
        t = (t - n * dot(n, t)).normalized();
        float w = dot(cross(n, t), bitan[v]) < 0.0f ? -1.0f : 1.0f;
        vertices[v].tangent = Vec4(t, w);
    }
}

size_t Mesh::weld(float epsilon) {
    if (vertices.empty()) return 0;
    const float inv = 1.0f / std::max(epsilon, 1e-9f);
    struct Key {
        int64_t x, y, z;
        int16_t nx, ny, nz;
        int32_t u, v;
        bool operator<(const Key &o) const {
            if (x != o.x) return x < o.x;
            if (y != o.y) return y < o.y;
            if (z != o.z) return z < o.z;
            if (nx != o.nx) return nx < o.nx;
            if (ny != o.ny) return ny < o.ny;
            if (nz != o.nz) return nz < o.nz;
            if (u != o.u) return u < o.u;
            return v < o.v;
        }
    };
    std::map<Key, uint32_t> seen;
    std::vector<Vertex> out;
    std::vector<uint32_t> remap(vertices.size());
    out.reserve(vertices.size());
    for (size_t i = 0; i < vertices.size(); i++) {
        const Vertex &v = vertices[i];
        Key k{int64_t(std::lround(v.position.x * inv)),
              int64_t(std::lround(v.position.y * inv)),
              int64_t(std::lround(v.position.z * inv)),
              int16_t(std::lround(v.normal.x * 64.0f)),
              int16_t(std::lround(v.normal.y * 64.0f)),
              int16_t(std::lround(v.normal.z * 64.0f)),
              int32_t(std::lround(v.uv.x * 4096.0f)),
              int32_t(std::lround(v.uv.y * 4096.0f))};
        auto it = seen.find(k);
        if (it != seen.end()) {
            remap[i] = it->second;
        } else {
            remap[i] = uint32_t(out.size());
            seen[k] = remap[i];
            out.push_back(v);
        }
    }
    size_t removed = vertices.size() - out.size();
    if (!removed) return 0;
    vertices.swap(out);
    for (uint32_t &i : indices) i = remap[i];
    return removed;
}

void Mesh::flip_winding() {
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
        std::swap(indices[i + 1], indices[i + 2]);
    for (Vertex &v : vertices) v.normal = -v.normal;
}

// ------------------------------------------------------------ uploading

bool Mesh::upload(rhi::Device *dev, const char *name) {
    using namespace rhi;
    if (!dev) return false;
    if (vertices.empty() || indices.empty()) {
        MF_WARN("mesh '%s' has nothing to upload", name ? name : "?");
        return false;
    }
    release(dev);
    owner_ = dev;
    if (submeshes.empty()) {
        SubMesh sm;
        sm.first_index = 0;
        sm.index_count = uint32_t(indices.size());
        sm.bounds = bounds_;
        submeshes.push_back(sm);
    }
    compute_bounds();

    BufferDesc vd;
    vd.size = vertices.size() * sizeof(Vertex);
    vd.usage = BufferUsage::Vertex;
    vd.name = name;
    vertex_buffer_ = dev->create_buffer(vd, vertices.data());

    BufferDesc id;
    id.size = indices.size() * sizeof(uint32_t);
    id.usage = BufferUsage::Index;
    id.name = name;
    index_buffer_ = dev->create_buffer(id, indices.data());

    if (!skin.empty()) {
        if (skin.size() != vertices.size()) {
            MF_ERROR("mesh '%s': %zu skin entries for %zu vertices",
                     name ? name : "?", skin.size(), vertices.size());
        } else {
            BufferDesc sd;
            sd.size = skin.size() * sizeof(SkinVertex);
            sd.usage = BufferUsage::Vertex;
            sd.name = name;
            skin_buffer_ = dev->create_buffer(sd, skin.data());
        }
    }
    return vertex_buffer_.valid() && index_buffer_.valid();
}

void Mesh::release(rhi::Device *dev) {
    if (!dev) return;
    if (vertex_buffer_.valid()) dev->destroy(vertex_buffer_);
    if (index_buffer_.valid()) dev->destroy(index_buffer_);
    if (skin_buffer_.valid()) dev->destroy(skin_buffer_);
    vertex_buffer_ = {};
    index_buffer_ = {};
    skin_buffer_ = {};
    owner_ = nullptr;
}

// ------------------------------------------------------------ primitives

namespace {

Vertex vtx(const Vec3 &p, const Vec3 &n, const Vec2 &uv) {
    Vertex v;
    v.position = p;
    v.normal = n;
    v.uv = uv;
    return v;
}

Ref<Mesh> finish(Ref<Mesh> m) {
    m->compute_tangents();
    m->compute_bounds();
    return m;
}

}  // namespace

Ref<Mesh> Mesh::box(const Vec3 &size) {
    Ref<Mesh> m(new Mesh());
    Vec3 h = size * 0.5f;
    // Six faces, each with its own normals and UVs -- a cube made of
    // eight shared vertices cannot have either.
    struct Face {
        Vec3 n, u, v;
    };
    const Face faces[6] = {
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},    // +Z
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},  // -Z
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},   // +X
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},   // -X
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},   // +Y
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},   // -Y
    };
    for (const Face &f : faces) {
        uint32_t base = uint32_t(m->vertices.size());
        Vec3 centre = f.n * (f.n.x * h.x + f.n.y * h.y + f.n.z * h.z);
        Vec3 eu = f.u * (std::fabs(f.u.x) * h.x + std::fabs(f.u.y) * h.y +
                         std::fabs(f.u.z) * h.z);
        Vec3 ev = f.v * (std::fabs(f.v.x) * h.x + std::fabs(f.v.y) * h.y +
                         std::fabs(f.v.z) * h.z);
        m->vertices.push_back(vtx(centre - eu - ev, f.n, {0, 0}));
        m->vertices.push_back(vtx(centre + eu - ev, f.n, {1, 0}));
        m->vertices.push_back(vtx(centre + eu + ev, f.n, {1, 1}));
        m->vertices.push_back(vtx(centre - eu + ev, f.n, {0, 1}));
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m->indices.push_back(base + i);
    }
    return finish(m);
}

Ref<Mesh> Mesh::plane(const Vec2 &size, int subdivisions) {
    Ref<Mesh> m(new Mesh());
    int n = std::max(1, subdivisions);
    Vec2 h = size * 0.5f;
    for (int y = 0; y <= n; y++)
        for (int x = 0; x <= n; x++) {
            float fx = float(x) / float(n), fy = float(y) / float(n);
            m->vertices.push_back(vtx({lerp(-h.x, h.x, fx), 0, lerp(h.y, -h.y, fy)},
                                      Vec3::up(), {fx, fy}));
        }
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            uint32_t a = uint32_t(y * (n + 1) + x);
            uint32_t b = a + 1;
            uint32_t c = a + uint32_t(n + 1);
            uint32_t d = c + 1;
            for (uint32_t i : {a, c, d, a, d, b}) m->indices.push_back(i);
        }
    return finish(m);
}

Ref<Mesh> Mesh::sphere(float radius, int rings, int segments) {
    Ref<Mesh> m(new Mesh());
    rings = std::max(3, rings);
    segments = std::max(3, segments);
    for (int y = 0; y <= rings; y++) {
        float v = float(y) / float(rings);
        float phi = v * PI;
        float sp = std::sin(phi), cp = std::cos(phi);
        for (int x = 0; x <= segments; x++) {
            float u = float(x) / float(segments);
            float theta = u * TAU;
            Vec3 n(sp * std::cos(theta), cp, sp * std::sin(theta));
            m->vertices.push_back(vtx(n * radius, n, {u, 1.0f - v}));
        }
    }
    for (int y = 0; y < rings; y++)
        for (int x = 0; x < segments; x++) {
            uint32_t a = uint32_t(y * (segments + 1) + x);
            uint32_t b = a + 1;
            uint32_t c = a + uint32_t(segments + 1);
            uint32_t d = c + 1;
            for (uint32_t i : {a, c, b, b, c, d}) m->indices.push_back(i);
        }
    return finish(m);
}

Ref<Mesh> Mesh::cylinder(float radius, float height, int segments, bool capped) {
    Ref<Mesh> m(new Mesh());
    segments = std::max(3, segments);
    float h = height * 0.5f;
    // The side, as its own strip so the cap normals do not bleed in.
    for (int x = 0; x <= segments; x++) {
        float u = float(x) / float(segments);
        float a = u * TAU;
        Vec3 n(std::cos(a), 0, std::sin(a));
        m->vertices.push_back(vtx({n.x * radius, -h, n.z * radius}, n, {u, 0}));
        m->vertices.push_back(vtx({n.x * radius, h, n.z * radius}, n, {u, 1}));
    }
    for (int x = 0; x < segments; x++) {
        uint32_t a = uint32_t(x * 2);
        for (uint32_t i : {a, a + 1u, a + 3u, a, a + 3u, a + 2u})
            m->indices.push_back(i);
    }
    if (capped) {
        for (int end = 0; end < 2; end++) {
            float y = end ? h : -h;
            Vec3 n = end ? Vec3::up() : Vec3::down();
            uint32_t centre = uint32_t(m->vertices.size());
            m->vertices.push_back(vtx({0, y, 0}, n, {0.5f, 0.5f}));
            for (int x = 0; x <= segments; x++) {
                float a = float(x) / float(segments) * TAU;
                Vec3 p(std::cos(a) * radius, y, std::sin(a) * radius);
                m->vertices.push_back(
                    vtx(p, n, {std::cos(a) * 0.5f + 0.5f, std::sin(a) * 0.5f + 0.5f}));
            }
            for (int x = 0; x < segments; x++) {
                uint32_t a = centre + 1 + uint32_t(x);
                if (end) {
                    m->indices.push_back(centre);
                    m->indices.push_back(a);
                    m->indices.push_back(a + 1);
                } else {
                    m->indices.push_back(centre);
                    m->indices.push_back(a + 1);
                    m->indices.push_back(a);
                }
            }
        }
    }
    return finish(m);
}

Ref<Mesh> Mesh::cone(float radius, float height, int segments) {
    Ref<Mesh> m(new Mesh());
    segments = std::max(3, segments);
    float h = height * 0.5f;
    // The slope's normal is tilted by the cone's angle, not radial.
    float slope = radius / std::max(height, 1e-5f);
    for (int x = 0; x <= segments; x++) {
        float u = float(x) / float(segments);
        float a = u * TAU;
        Vec3 radial(std::cos(a), 0, std::sin(a));
        Vec3 n = Vec3(radial.x, slope, radial.z).normalized();
        m->vertices.push_back(vtx({radial.x * radius, -h, radial.z * radius}, n, {u, 0}));
        m->vertices.push_back(vtx({0, h, 0}, n, {u, 1}));
    }
    for (int x = 0; x < segments; x++) {
        uint32_t a = uint32_t(x * 2);
        for (uint32_t i : {a, a + 1u, a + 2u}) m->indices.push_back(i);
    }
    uint32_t centre = uint32_t(m->vertices.size());
    m->vertices.push_back(vtx({0, -h, 0}, Vec3::down(), {0.5f, 0.5f}));
    for (int x = 0; x <= segments; x++) {
        float a = float(x) / float(segments) * TAU;
        m->vertices.push_back(vtx({std::cos(a) * radius, -h, std::sin(a) * radius},
                                  Vec3::down(),
                                  {std::cos(a) * 0.5f + 0.5f, std::sin(a) * 0.5f + 0.5f}));
    }
    for (int x = 0; x < segments; x++) {
        uint32_t a = centre + 1 + uint32_t(x);
        m->indices.push_back(centre);
        m->indices.push_back(a + 1);
        m->indices.push_back(a);
    }
    return finish(m);
}

Ref<Mesh> Mesh::torus(float major, float minor, int major_segments,
                      int minor_segments) {
    Ref<Mesh> m(new Mesh());
    major_segments = std::max(3, major_segments);
    minor_segments = std::max(3, minor_segments);
    for (int i = 0; i <= major_segments; i++) {
        float u = float(i) / float(major_segments);
        float a = u * TAU;
        Vec3 centre(std::cos(a) * major, 0, std::sin(a) * major);
        Vec3 out(std::cos(a), 0, std::sin(a));
        for (int j = 0; j <= minor_segments; j++) {
            float v = float(j) / float(minor_segments);
            float b = v * TAU;
            Vec3 n = out * std::cos(b) + Vec3::up() * std::sin(b);
            m->vertices.push_back(vtx(centre + n * minor, n, {u, v}));
        }
    }
    for (int i = 0; i < major_segments; i++)
        for (int j = 0; j < minor_segments; j++) {
            uint32_t a = uint32_t(i * (minor_segments + 1) + j);
            uint32_t b = a + 1;
            uint32_t c = a + uint32_t(minor_segments + 1);
            uint32_t d = c + 1;
            for (uint32_t k : {a, c, b, b, c, d}) m->indices.push_back(k);
        }
    return finish(m);
}

Ref<Mesh> Mesh::capsule(float radius, float height, int rings, int segments) {
    Ref<Mesh> m(new Mesh());
    rings = std::max(2, rings);
    segments = std::max(3, segments);
    // `height` is the cylinder between the caps, so the whole thing is
    // height + 2 * radius tall -- the same convention as the physics
    // capsule, so a collider and its mesh agree without a fudge.
    float half = height * 0.5f;
    auto ring = [&](float y, float r, float v, const Vec3 &centre) {
        for (int x = 0; x <= segments; x++) {
            float u = float(x) / float(segments);
            float a = u * TAU;
            Vec3 p(std::cos(a) * r, y, std::sin(a) * r);
            m->vertices.push_back(vtx(p, (p - centre).normalized(), {u, v}));
        }
    };
    for (int i = 0; i <= rings; i++) {
        float t = float(i) / float(rings);
        float phi = t * PI * 0.5f;
        ring(half + std::sin(phi) * radius, std::cos(phi) * radius,
             1.0f - t * 0.25f, {0, half, 0});
    }
    ring(half, radius, 0.75f, {0, half, 0});
    ring(-half, radius, 0.25f, {0, -half, 0});
    for (int i = rings; i >= 0; i--) {
        float t = float(i) / float(rings);
        float phi = t * PI * 0.5f;
        ring(-half - std::sin(phi) * radius, std::cos(phi) * radius,
             t * 0.25f, {0, -half, 0});
    }
    int row = segments + 1;
    int rows = int(m->vertices.size()) / row;
    for (int y = 0; y < rows - 1; y++)
        for (int x = 0; x < segments; x++) {
            uint32_t a = uint32_t(y * row + x);
            uint32_t b = a + 1;
            uint32_t c = a + uint32_t(row);
            uint32_t d = c + 1;
            for (uint32_t k : {a, c, b, b, c, d}) m->indices.push_back(k);
        }
    return finish(m);
}

Ref<Mesh> Mesh::quad(const Vec2 &size) {
    Ref<Mesh> m(new Mesh());
    Vec2 h = size * 0.5f;
    m->vertices.push_back(vtx({-h.x, -h.y, 0}, Vec3::back(), {0, 0}));
    m->vertices.push_back(vtx({h.x, -h.y, 0}, Vec3::back(), {1, 0}));
    m->vertices.push_back(vtx({h.x, h.y, 0}, Vec3::back(), {1, 1}));
    m->vertices.push_back(vtx({-h.x, h.y, 0}, Vec3::back(), {0, 1}));
    m->indices = {0, 1, 2, 0, 2, 3};
    return finish(m);
}

Ref<Mesh> Mesh::wire_box(const AABB &box) {
    Ref<Mesh> m(new Mesh());
    for (int i = 0; i < 8; i++)
        m->vertices.push_back(vtx(box.corner(i), Vec3::up(), {0, 0}));
    const uint32_t edges[24] = {0, 1, 1, 3, 3, 2, 2, 0, 4, 5, 5, 7,
                                7, 6, 6, 4, 0, 4, 1, 5, 2, 6, 3, 7};
    m->indices.assign(edges, edges + 24);
    m->compute_bounds();
    return m;
}

static void register_mesh_class() {
    ClassBuilder<Mesh>()
        .method("clear", &Mesh::clear)
        .method("compute_normals", &Mesh::compute_normals,
                {Variant(double(deg2rad(60.0f)))})
        .args("smooth_angle")
        .method("compute_tangents", &Mesh::compute_tangents)
        .method("compute_bounds", &Mesh::compute_bounds)
        .method("weld", &Mesh::weld, {Variant(1e-5)}).args("epsilon")
        .method("flip_winding", &Mesh::flip_winding)
        .method("transform", &Mesh::transform).args("transform")
        .prop_ro("bounds", &Mesh::bounds)
        .prop_ro("vertex_count", &Mesh::vertex_count)
        .prop_ro("triangle_count", &Mesh::triangle_count)
        .prop_ro("uploaded", &Mesh::uploaded);
}
MF_REGISTER(register_mesh_class)

}  // namespace mf
