#include "shapes.h"

#include <algorithm>

#include "core/log.h"

namespace wr {

AABB Shape::local_bounds() const {
    switch (type) {
        case ShapeType::Sphere:
            return {Vec3(-radius), Vec3(radius)};
        case ShapeType::Capsule: {
            float h = height * 0.5f + radius;
            return {{-radius, -h, -radius}, {radius, h, radius}};
        }
        case ShapeType::Box:
            return {-half_extents, half_extents};
        case ShapeType::Mesh:
            // The world's mesh has its own bounds; the shape does not
            // know them, so the caller must ask the mesh.
            return {};
    }
    return {};
}

AABB Shape::world_bounds(const Transform3D &t) const {
    AABB local = local_bounds();
    if (!local.valid()) return {};
    // A sphere and a capsule are round, so a transformed corner box is
    // loose; for a uniform scale the exact bound is cheap.
    if (type == ShapeType::Sphere) {
        float r = radius * t.basis.uniform_scale();
        return {t.origin - Vec3(r), t.origin + Vec3(r)};
    }
    return local.transformed(t);
}

Vec3 Shape::support(const Vec3 &dir) const {
    Vec3 d = dir.normalized();
    switch (type) {
        case ShapeType::Sphere:
            return d * radius;
        case ShapeType::Capsule: {
            float h = height * 0.5f;
            return Vec3(0, d.y >= 0 ? h : -h, 0) + d * radius;
        }
        case ShapeType::Box:
            return {d.x >= 0 ? half_extents.x : -half_extents.x,
                    d.y >= 0 ? half_extents.y : -half_extents.y,
                    d.z >= 0 ? half_extents.z : -half_extents.z};
        case ShapeType::Mesh:
            return {};
    }
    return {};
}

// ------------------------------------------------------------ narrow phase

Vec3 closest_point_on_triangle(const Vec3 &p, const Vec3 &a, const Vec3 &b,
                               const Vec3 &c) {
    // Ericson, Real-Time Collision Detection. Each early return is one
    // Voronoi region of the triangle.
    Vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    Vec3 bp = p - b;
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
        return a + ab * (d1 / (d1 - d3));

    Vec3 cp = p - c;
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
        return a + ac * (d2 / (d2 - d6));

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));

    float denom = va + vb + vc;
    if (std::fabs(denom) < 1e-20f) return a;
    float v = vb / denom, w = vc / denom;
    return a + ab * v + ac * w;
}

void closest_points_on_segments(const Vec3 &p1, const Vec3 &q1, const Vec3 &p2,
                                const Vec3 &q2, Vec3 *c1, Vec3 *c2) {
    Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    float s, t;
    const float eps = 1e-9f;

    if (a <= eps && e <= eps) {
        *c1 = p1;
        *c2 = p2;
        return;
    }
    if (a <= eps) {
        s = 0.0f;
        t = clampf(f / e, 0.0f, 1.0f);
    } else {
        float c = dot(d1, r);
        if (e <= eps) {
            t = 0.0f;
            s = clampf(-c / a, 0.0f, 1.0f);
        } else {
            float b = dot(d1, d2);
            float denom = a * e - b * b;
            // Parallel segments leave `denom` at zero; any point will
            // do, so the start of the first is used.
            s = denom > eps ? clampf((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = clampf(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = clampf((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    *c1 = p1 + d1 * s;
    *c2 = p2 + d2 * t;
}

bool ray_triangle(const Vec3 &origin, const Vec3 &dir, const Vec3 &a,
                  const Vec3 &b, const Vec3 &c, float *out_t) {
    const float eps = 1e-8f;
    Vec3 e1 = b - a, e2 = c - a;
    Vec3 pv = cross(dir, e2);
    float det = dot(e1, pv);
    // TWO SIDED. Through a portal you routinely see and shoot the back
    // of the world, and a one-sided test would let rays through it.
    if (std::fabs(det) < eps) return false;
    float inv = 1.0f / det;
    Vec3 tv = origin - a;
    float u = dot(tv, pv) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    Vec3 qv = cross(tv, e1);
    float v = dot(dir, qv) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    float t = dot(e2, qv) * inv;
    if (t < 0.0f) return false;
    if (out_t) *out_t = t;
    return true;
}

// ------------------------------------------------------------ TriangleMesh

void TriangleMesh::build(std::vector<Vec3> vertices, std::vector<uint32_t> indices) {
    vertices_ = std::move(vertices);
    indices_ = std::move(indices);
    order_.clear();
    nodes_.clear();
    tri_bounds_.clear();
    tri_centres_.clear();
    bounds_ = AABB();
    const size_t n = indices_.size() / 3;
    if (!n) return;

    tri_bounds_.resize(n);
    tri_centres_.resize(n);
    order_.resize(n);
    for (size_t i = 0; i < n; i++) {
        Vec3 t[3];
        triangle(i, t);
        AABB b;
        for (const Vec3 &p : t) b.expand(p);
        // A perfectly flat triangle makes a zero-thickness box that
        // slab tests can miss; a hair of padding costs nothing.
        tri_bounds_[i] = b.grown(1e-4f);
        tri_centres_[i] = (t[0] + t[1] + t[2]) / 3.0f;
        order_[i] = uint32_t(i);
        bounds_.expand(b);
    }

    nodes_.reserve(n * 2);
    Node root;
    root.first = 0;
    root.count = uint32_t(n);
    root.bounds = bounds_;
    nodes_.push_back(root);
    subdivide(0, 0);
    WR_DEBUG("physics: BVH over %zu triangles, %zu nodes", n, nodes_.size());
}

void TriangleMesh::subdivide(uint32_t index, int depth) {
    Node &node = nodes_[index];
    // Small leaves beat deep trees: the constant factor of visiting a
    // node is higher than testing a handful of triangles.
    if (node.count <= 4 || depth > 32) return;

    // Split at the mean of the centroids on the widest axis. Cheaper
    // than a surface-area heuristic and within a few percent of it for
    // level geometry, which is not adversarial.
    AABB centroid_bounds;
    for (uint32_t i = 0; i < node.count; i++)
        centroid_bounds.expand(tri_centres_[order_[node.first + i]]);
    Vec3 extent = centroid_bounds.size();
    int axis = extent.max_axis();
    if (extent[axis] < 1e-6f) return;
    float split = centroid_bounds.center()[axis];

    uint32_t i = node.first;
    uint32_t j = node.first + node.count - 1;
    while (i <= j) {
        if (tri_centres_[order_[i]][axis] < split) {
            i++;
        } else {
            std::swap(order_[i], order_[j]);
            if (j == 0) break;
            j--;
        }
    }
    uint32_t left_count = i - node.first;
    if (left_count == 0 || left_count == node.count) return;

    uint32_t first = node.first, count = node.count;
    uint32_t left = uint32_t(nodes_.size());
    nodes_.push_back({});
    uint32_t right = uint32_t(nodes_.size());
    nodes_.push_back({});
    // `node` may have been invalidated by the pushes.
    nodes_[index].count = 0;
    nodes_[index].left = left;
    nodes_[index].right = right;

    auto fill = [&](uint32_t n, uint32_t f, uint32_t c) {
        AABB b;
        for (uint32_t k = 0; k < c; k++) b.expand(tri_bounds_[order_[f + k]]);
        nodes_[n].first = f;
        nodes_[n].count = c;
        nodes_[n].bounds = b;
    };
    fill(left, first, left_count);
    fill(right, first + left_count, count - left_count);
    subdivide(left, depth + 1);
    subdivide(right, depth + 1);
}

void TriangleMesh::query(const AABB &box, std::vector<uint32_t> &out) const {
    if (nodes_.empty()) return;
    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const Node &n = nodes_[stack[--sp]];
        if (!n.bounds.intersects(box)) continue;
        if (n.count) {
            for (uint32_t i = 0; i < n.count; i++) {
                uint32_t tri = order_[n.first + i];
                if (tri_bounds_[tri].intersects(box)) out.push_back(tri);
            }
        } else if (sp + 2 <= 64) {
            stack[sp++] = n.left;
            stack[sp++] = n.right;
        }
    }
}

int32_t TriangleMesh::raycast(const Vec3 &origin, const Vec3 &dir, float max_t,
                              float *out_t, Vec3 *out_normal) const {
    if (nodes_.empty()) return -1;
    int32_t best = -1;
    float best_t = max_t;
    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        uint32_t index = stack[--sp];
        const Node &n = nodes_[index];
        float tmin = 0.0f;
        if (!n.bounds.intersects_ray(origin, dir, &tmin)) continue;
        // Already found something nearer than this whole subtree.
        if (tmin > best_t) continue;
        if (n.count) {
            for (uint32_t i = 0; i < n.count; i++) {
                uint32_t tri = order_[n.first + i];
                Vec3 t3[3];
                triangle(tri, t3);
                float t;
                if (ray_triangle(origin, dir, t3[0], t3[1], t3[2], &t) &&
                    t < best_t) {
                    best_t = t;
                    best = int32_t(tri);
                }
            }
        } else if (sp + 2 <= 64) {
            stack[sp++] = n.left;
            stack[sp++] = n.right;
        }
    }
    if (best < 0) return -1;
    if (out_t) *out_t = best_t;
    if (out_normal) {
        Vec3 t3[3];
        triangle(size_t(best), t3);
        Vec3 nrm = cross(t3[1] - t3[0], t3[2] - t3[0]).normalized();
        // Face the ray, so a surface hit from behind -- through a
        // portal, say -- still reports a usable normal.
        if (dot(nrm, dir) > 0.0f) nrm = -nrm;
        *out_normal = nrm;
    }
    return best;
}

}  // namespace wr
