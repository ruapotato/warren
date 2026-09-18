#include "mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <queue>
#include <set>
#include <unordered_map>

#include "core/log.h"

namespace wr {

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

AABB compute_bounds(const std::vector<Vertex> &vertices) {
    AABB b;
    for (const Vertex &v : vertices) b.expand(v.position);
    return b;
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
void Mesh::compute_tangents() { wr::compute_tangents(vertices, indices); }

void compute_tangents(std::vector<Vertex> &vertices,
                      const std::vector<uint32_t> &indices) {
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

// ------------------------------------------------- simplification

namespace {

// A quadric is the symmetric 4x4 matrix of a sum of squared plane
// distances, which has ten distinct entries. Adding two quadrics adds
// the two sets of planes; evaluating one at a point gives the total
// squared distance to all of them.
struct Quadric {
    double m[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    // THE TOTAL WEIGHT, so the error can be reported in metres.
    //
    // The planes are weighted by face area, which is what keeps a
    // big flat face from being outvoted by a cluster of small ones.
    // But it also means the raw quadric is an area times a squared
    // distance, and a threshold in those units means nothing to a
    // caller and changes meaning with the size of the model.
    // Dividing by the weight gives a mean squared distance, whose
    // square root is a length somebody can reason about.
    double w = 0;

    void add_plane(double a, double b, double c, double d, double weight) {
        m[0] += a * a * weight; m[1] += a * b * weight; m[2] += a * c * weight;
        m[3] += a * d * weight;
        m[4] += b * b * weight; m[5] += b * c * weight; m[6] += b * d * weight;
        m[7] += c * c * weight; m[8] += c * d * weight;
        m[9] += d * d * weight;
        w += weight;
    }
    void add(const Quadric &o) {
        for (int i = 0; i < 10; i++) m[i] += o.m[i];
        w += o.w;
    }
    double at(const Vec3 &p) const {
        const double x = p.x, y = p.y, z = p.z;
        return m[0]*x*x + 2*m[1]*x*y + 2*m[2]*x*z + 2*m[3]*x +
               m[4]*y*y + 2*m[5]*y*z + 2*m[6]*y +
               m[7]*z*z + 2*m[8]*z + m[9];
    }
};

struct Collapse {
    uint32_t a = 0, b = 0;
    double cost = 0;
    Vec3 to;
    // The version of each endpoint when this was costed; a stale
    // entry is discarded rather than kept up to date, because
    // re-costing every neighbour of every collapse into a heap that
    // cannot delete is far more work than throwing a few away.
    uint32_t version_a = 0, version_b = 0;
    bool operator<(const Collapse &o) const { return cost > o.cost; }  // min-heap
};

}  // namespace

size_t Mesh::simplify(float ratio, float max_error) {
    const size_t triangles = indices.size() / 3;
    if (triangles < 4 || ratio >= 1.0f) return triangles;
    const size_t target = std::max<size_t>(4, size_t(double(triangles) * std::max(ratio, 0.0f)));

    // WELDED BY POSITION FIRST, and mapped back afterwards.
    //
    // A mesh whose vertices were split for a hard edge or a UV seam
    // has several vertices in the same place, and an edge collapse
    // that moves one and not the others tears the surface open. So
    // the topology worked on here is the merged one, and every
    // original vertex follows the merged vertex it belongs to.
    struct Key {
        int64_t x, y, z;
        bool operator<(const Key &o) const {
            return x != o.x ? x < o.x : y != o.y ? y < o.y : z < o.z;
        }
    };
    std::map<Key, uint32_t> unique;
    std::vector<uint32_t> to_merged(vertices.size());
    std::vector<uint32_t> first_original;  // one original per merged vertex
    std::vector<Vec3> position;
    for (size_t i = 0; i < vertices.size(); i++) {
        const Vec3 &p = vertices[i].position;
        const Key k{int64_t(std::llround(double(p.x) * 65536.0)),
                    int64_t(std::llround(double(p.y) * 65536.0)),
                    int64_t(std::llround(double(p.z) * 65536.0))};
        auto it = unique.find(k);
        if (it == unique.end()) {
            it = unique.emplace(k, uint32_t(position.size())).first;
            position.push_back(p);
            first_original.push_back(uint32_t(i));
        }
        to_merged[i] = it->second;
    }
    const size_t n = position.size();

    std::vector<std::array<uint32_t, 3>> faces;
    faces.reserve(triangles);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::array<uint32_t, 3> f{to_merged[indices[i]], to_merged[indices[i + 1]],
                                        to_merged[indices[i + 2]]};
        if (f[0] == f[1] || f[1] == f[2] || f[0] == f[2]) continue;  // already degenerate
        faces.push_back(f);
    }

    std::vector<Quadric> quadrics(n);
    std::vector<std::vector<uint32_t>> vertex_faces(n);
    auto face_plane = [&](const std::array<uint32_t, 3> &f, double *a, double *b,
                          double *c, double *d) {
        const Vec3 &p0 = position[f[0]];
        const Vec3 nrm = cross(position[f[1]] - p0, position[f[2]] - p0);
        const float len = nrm.length();
        if (len < 1e-20f) return 0.0;
        const Vec3 u = nrm / len;
        *a = u.x; *b = u.y; *c = u.z;
        *d = -double(dot(u, p0));
        return double(len) * 0.5;  // the area, used as the weight
    };
    for (size_t i = 0; i < faces.size(); i++) {
        double a, b, c, d;
        const double area = face_plane(faces[i], &a, &b, &c, &d);
        if (area <= 0.0) continue;
        for (int k = 0; k < 3; k++) {
            quadrics[faces[i][k]].add_plane(a, b, c, d, area);
            vertex_faces[faces[i][k]].push_back(uint32_t(i));
        }
    }

    // OPEN EDGES ARE PINNED.
    //
    // An edge belonging to one triangle is the rim of a surface that
    // does not close -- a terrain chunk's border, a decal, a cut
    // plane. Collapsing across it eats the rim away and leaves a gap
    // next to whatever was sitting alongside. A plane perpendicular
    // to the surface along that edge, weighted heavily, makes any
    // collapse that moves it prohibitively expensive without
    // forbidding it outright.
    {
        std::map<std::pair<uint32_t, uint32_t>, int> edge_count;
        for (const auto &f : faces)
            for (int k = 0; k < 3; k++) {
                const uint32_t u = f[k], v = f[(k + 1) % 3];
                edge_count[{std::min(u, v), std::max(u, v)}]++;
            }
        for (const auto &f : faces) {
            double a, b, c, d;
            if (face_plane(f, &a, &b, &c, &d) <= 0.0) continue;
            const Vec3 normal{float(a), float(b), float(c)};
            for (int k = 0; k < 3; k++) {
                const uint32_t u = f[k], v = f[(k + 1) % 3];
                if (edge_count[{std::min(u, v), std::max(u, v)}] != 1) continue;
                const Vec3 along = position[v] - position[u];
                const Vec3 wall = cross(along, normal);
                const float len = wall.length();
                if (len < 1e-20f) continue;
                const Vec3 wn = wall / len;
                const double wd = -double(dot(wn, position[u]));
                quadrics[u].add_plane(wn.x, wn.y, wn.z, wd, 1000.0);
                quadrics[v].add_plane(wn.x, wn.y, wn.z, wd, 1000.0);
            }
        }
    }

    std::vector<uint32_t> version(n, 0);
    std::vector<bool> dead(n, false);

    // WHERE TO PUT THE MERGED VERTEX.
    //
    // The minimiser of the combined quadric, found by solving the
    // 3x3 system its gradient gives. This is not a refinement over
    // picking the better endpoint or the midpoint: on a curved
    // surface every one of those three lies INSIDE the surface, so
    // taking them loses volume on every collapse and a decimated
    // sphere comes out four percent small. The solved point sits
    // where the neighbouring planes intersect, which is outside the
    // chord, and the shrinkage goes away.
    //
    // The system is singular exactly where the planes are parallel
    // -- a flat region -- and there the three candidates are already
    // exact, so they are the fallback and cost nothing.
    auto best_point = [&](uint32_t a, uint32_t bb, Quadric *q, double *cost) {
        Quadric sum = quadrics[a];
        sum.add(quadrics[bb]);
        *q = sum;

        const double *m = sum.m;
        const double det =
            m[0] * (m[4] * m[7] - m[5] * m[5]) -
            m[1] * (m[1] * m[7] - m[5] * m[2]) +
            m[2] * (m[1] * m[5] - m[4] * m[2]);
        // Scaled against the matrix's own magnitude, so the test
        // means "nearly singular" at any size of model.
        const double scale = std::fabs(m[0]) + std::fabs(m[4]) + std::fabs(m[7]);
        Vec3 chosen;
        bool solved = false;
        if (scale > 0.0 && std::fabs(det) > 1e-10 * scale * scale * scale) {
            const double inv = 1.0 / det;
            const double x = -inv * (m[3] * (m[4] * m[7] - m[5] * m[5]) -
                                     m[6] * (m[1] * m[7] - m[2] * m[5]) +
                                     m[8] * (m[1] * m[5] - m[2] * m[4]));
            const double y = -inv * (m[0] * (m[6] * m[7] - m[5] * m[8]) -
                                     m[1] * (m[3] * m[7] - m[2] * m[8]) +
                                     m[2] * (m[3] * m[5] - m[2] * m[6]));
            const double z = -inv * (m[0] * (m[4] * m[8] - m[5] * m[6]) -
                                     m[1] * (m[1] * m[8] - m[2] * m[6]) +
                                     m[3] * (m[1] * m[5] - m[2] * m[4]));
            const Vec3 p{float(x), float(y), float(z)};
            // A solve that lands a long way from the edge it came
            // from is a solve that has gone wrong, whatever the
            // determinant said.
            const Vec3 mid = (position[a] + position[bb]) * 0.5f;
            const float span = (position[a] - position[bb]).length() + 1e-6f;
            if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
                (p - mid).length() < span * 4.0f) {
                chosen = p;
                solved = true;
            }
        }
        if (!solved) {
            const Vec3 candidates[3] = {position[a], position[bb],
                                        (position[a] + position[bb]) * 0.5f};
            double best = sum.at(candidates[0]);
            int which = 0;
            for (int i = 1; i < 3; i++) {
                const double e = sum.at(candidates[i]);
                if (e < best) { best = e; which = i; }
            }
            chosen = candidates[which];
        }
        // Mean squared distance to the planes, so the threshold the
        // caller gave is a distance in metres.
        *cost = sum.w > 0.0 ? std::max(sum.at(chosen), 0.0) / sum.w : 0.0;
        return chosen;
    };

    std::priority_queue<Collapse> queue;
    auto push_edge = [&](uint32_t a, uint32_t bb) {
        if (a == bb) return;
        Collapse c;
        c.a = std::min(a, bb);
        c.b = std::max(a, bb);
        Quadric q;
        c.to = best_point(c.a, c.b, &q, &c.cost);
        c.version_a = version[c.a];
        c.version_b = version[c.b];
        queue.push(c);
    };
    {
        std::set<std::pair<uint32_t, uint32_t>> seen;
        for (const auto &f : faces)
            for (int k = 0; k < 3; k++) {
                const uint32_t u = f[k], v = f[(k + 1) % 3];
                if (seen.emplace(std::min(u, v), std::max(u, v)).second)
                    push_edge(u, v);
            }
    }

    size_t live_faces = faces.size();
    const double error_limit = double(max_error) * double(max_error);
    while (live_faces > target && !queue.empty()) {
        const Collapse c = queue.top();
        queue.pop();
        if (dead[c.a] || dead[c.b]) continue;
        if (version[c.a] != c.version_a || version[c.b] != c.version_b) continue;
        if (c.cost > error_limit) break;  // everything cheaper is gone

        // WOULD ANY TRIANGLE TURN INSIDE OUT?
        //
        // A collapse that is cheap by the quadric can still fold a
        // triangle over, because the quadric measures distance to
        // planes and says nothing about which side of them anything
        // is on. One fold is a black facet that no amount of
        // relighting fixes, so every affected face is checked.
        bool flips = false;
        for (uint32_t which : {c.a, c.b}) {
            for (uint32_t fi : vertex_faces[which]) {
                const auto &f = faces[fi];
                if (f[0] == f[1]) continue;  // retired
                // Faces containing both endpoints vanish; they cannot flip.
                int has = 0;
                for (int k = 0; k < 3; k++)
                    if (f[k] == c.a || f[k] == c.b) has++;
                if (has == 2) continue;
                Vec3 p[3];
                for (int k = 0; k < 3; k++)
                    p[k] = (f[k] == c.a || f[k] == c.b) ? c.to : position[f[k]];
                const Vec3 before = cross(position[f[1]] - position[f[0]],
                                          position[f[2]] - position[f[0]]);
                const Vec3 after = cross(p[1] - p[0], p[2] - p[0]);
                if (after.length_sq() < 1e-24f || dot(before, after) <= 0.0f) {
                    flips = true;
                    break;
                }
            }
            if (flips) break;
        }
        if (flips) continue;

        // Do it: b becomes a, a moves to the new point.
        position[c.a] = c.to;
        quadrics[c.a].add(quadrics[c.b]);
        dead[c.b] = true;
        version[c.a]++;

        for (uint32_t fi : vertex_faces[c.b]) {
            auto &f = faces[fi];
            if (f[0] == f[1]) continue;
            for (int k = 0; k < 3; k++)
                if (f[k] == c.b) f[k] = c.a;
            if (f[0] == f[1] || f[1] == f[2] || f[0] == f[2]) {
                f = {0, 0, 0};  // retired
                live_faces--;
            } else {
                vertex_faces[c.a].push_back(fi);
            }
        }
        vertex_faces[c.b].clear();

        // Re-cost every edge still touching the merged vertex.
        std::set<uint32_t> neighbours;
        for (uint32_t fi : vertex_faces[c.a]) {
            const auto &f = faces[fi];
            if (f[0] == f[1]) continue;
            for (int k = 0; k < 3; k++)
                if (f[k] != c.a && !dead[f[k]]) neighbours.insert(f[k]);
        }
        for (uint32_t v : neighbours) push_edge(c.a, v);
    }

    // --- rebuild
    //
    // Each surviving merged vertex keeps one original, so its normal,
    // UV and colour come from a vertex that was really there rather
    // than from an average of several that were not.
    std::vector<uint32_t> remap(n, UINT32_MAX);
    std::vector<Vertex> out_vertices;
    for (size_t i = 0; i < n; i++) {
        if (dead[i]) continue;
        remap[i] = uint32_t(out_vertices.size());
        Vertex v = vertices[first_original[i]];
        v.position = position[i];
        out_vertices.push_back(v);
    }
    std::vector<uint32_t> out_indices;
    out_indices.reserve(live_faces * 3);
    for (const auto &f : faces) {
        if (f[0] == f[1] || f[1] == f[2] || f[0] == f[2]) continue;
        if (remap[f[0]] == UINT32_MAX || remap[f[1]] == UINT32_MAX ||
            remap[f[2]] == UINT32_MAX)
            continue;
        out_indices.push_back(remap[f[0]]);
        out_indices.push_back(remap[f[1]]);
        out_indices.push_back(remap[f[2]]);
    }

    vertices.swap(out_vertices);
    indices.swap(out_indices);
    // One submesh: the ranges the old ones described no longer exist.
    if (submeshes.size() > 1)
        WR_WARN("mesh: simplify collapsed %zu submeshes into one",
                submeshes.size());
    submeshes.clear();
    SubMesh sm;
    sm.first_index = 0;
    sm.index_count = uint32_t(indices.size());
    sm.name = "simplified";
    submeshes.push_back(sm);
    compute_bounds();
    return indices.size() / 3;
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
        WR_WARN("mesh '%s' has nothing to upload", name ? name : "?");
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
            WR_ERROR("mesh '%s': %zu skin entries for %zu vertices",
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
        // THE MAGNITUDE OF THE HALF-EXTENT ALONG THE NORMAL, not
        // the signed projection. The signed one puts the -Z face at
        // +Z -- two minus signs cancelling -- so the "box" was three
        // doubled quads through the positive faces and nothing at
        // all on the other three sides. It renders as a box from one
        // octant, which is where every screenshot of it was taken
        // from, and the renderer draws back faces so the missing
        // sides never showed as holes.
        Vec3 centre = f.n * (std::fabs(f.n.x) * h.x + std::fabs(f.n.y) * h.y +
                             std::fabs(f.n.z) * h.z);
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
            // Counter-clockwise seen from outside. It was the other
            // way round, which nothing noticed because the renderer
            // draws back faces on purpose -- a portal shows you the
            // far side of things constantly -- so an inside-out
            // sphere shades exactly like a right-way-out one.
            for (uint32_t i : {a, b, c, b, d, c}) m->indices.push_back(i);
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
                // Seen from outside: the top cap runs one way and
                // the bottom the other. Both were the wrong way.
                if (end) {
                    m->indices.push_back(centre);
                    m->indices.push_back(a + 1);
                    m->indices.push_back(a);
                } else {
                    m->indices.push_back(centre);
                    m->indices.push_back(a);
                    m->indices.push_back(a + 1);
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
        // The base faces down, so seen from below it runs the other
        // way round than the slope does seen from outside.
        m->indices.push_back(centre);
        m->indices.push_back(a);
        m->indices.push_back(a + 1);
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
            for (uint32_t k : {a, b, c, b, d, c}) m->indices.push_back(k);
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
    // THE ROWS MUST DESCEND, all the way down, with no repeats.
    //
    // They did not: the top cap was built equator-upwards to the
    // pole and then the cylinder's top ring was emitted again, so
    // the strip ran up the cap and straight back down it -- a
    // folded copy of the cap laid over itself, cancelling. Same at
    // the bottom. The mesh looked like a capsule from outside
    // because the fold is exactly where the surface already is.
    //
    // The top cap's last row IS the cylinder's top ring, and the
    // bottom cap's first row is its bottom ring, so the cylinder is
    // the band between them and needs no rings of its own.
    for (int i = rings; i >= 0; i--) {
        float t = float(i) / float(rings);
        float phi = t * PI * 0.5f;
        ring(half + std::sin(phi) * radius, std::cos(phi) * radius,
             0.75f + t * 0.25f, {0, half, 0});
    }
    for (int i = 0; i <= rings; i++) {
        float t = float(i) / float(rings);
        float phi = t * PI * 0.5f;
        ring(-half - std::sin(phi) * radius, std::cos(phi) * radius,
             0.25f - t * 0.25f, {0, -half, 0});
    }
    int row = segments + 1;
    int rows = int(m->vertices.size()) / row;
    for (int y = 0; y < rows - 1; y++)
        for (int x = 0; x < segments; x++) {
            uint32_t a = uint32_t(y * row + x);
            uint32_t b = a + 1;
            uint32_t c = a + uint32_t(row);
            uint32_t d = c + 1;
            for (uint32_t k : {a, b, c, b, d, c}) m->indices.push_back(k);
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
WR_REGISTER(register_mesh_class)

}  // namespace wr
