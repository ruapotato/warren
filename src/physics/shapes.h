// Warren -- collision shapes.
//
// Four primitives and a triangle mesh, which between them cover
// everything a game actually collides: a capsule is a character, a
// sphere is a projectile or a pickup, a box is a crate or a trigger
// volume, and a triangle mesh is the world.
//
// Convex hulls are deliberately absent for now. They need a full
// GJK/EPA pair to be worth having and the three primitives plus a
// static mesh carry a whole game without them.
#pragma once

#include <vector>

#include "core/math/transform.h"

namespace wr {

enum class ShapeType : uint8_t { Sphere, Capsule, Box, Mesh };

// A capsule stands along its local Y. `height` is the distance BETWEEN
// the two cap centres, so the whole thing is height + 2 * radius tall
// -- the same convention as Mesh::capsule, so a collider and its mesh
// agree without a fudge factor.
struct Shape {
    ShapeType type = ShapeType::Sphere;
    float radius = 0.5f;
    float height = 1.0f;         // capsule only
    Vec3 half_extents{0.5f, 0.5f, 0.5f};  // box only
    // Mesh only: an index into the world's mesh table.
    int32_t mesh = -1;

    static Shape sphere(float r) {
        Shape s;
        s.type = ShapeType::Sphere;
        s.radius = r;
        return s;
    }
    static Shape capsule(float r, float h) {
        Shape s;
        s.type = ShapeType::Capsule;
        s.radius = r;
        s.height = h;
        return s;
    }
    static Shape box(const Vec3 &half) {
        Shape s;
        s.type = ShapeType::Box;
        s.half_extents = half;
        return s;
    }
    static Shape triangle_mesh(int32_t index) {
        Shape s;
        s.type = ShapeType::Mesh;
        s.mesh = index;
        return s;
    }

    // Grown by a uniform factor. A body that walks through a portal
    // into a larger one needs exactly this.
    Shape scaled(float k) const {
        Shape s = *this;
        s.radius *= k;
        s.height *= k;
        s.half_extents *= k;
        return s;
    }

    AABB local_bounds() const;
    AABB world_bounds(const Transform3D &t) const;
    // The furthest point along `dir`, in local space. The building
    // block of every sweep below.
    Vec3 support(const Vec3 &dir) const;
};

// A triangle soup with a BVH over it. Static: the world, not the
// things moving through it.
class TriangleMesh {
public:
    void build(std::vector<Vec3> vertices, std::vector<uint32_t> indices);
    bool empty() const { return indices_.empty(); }
    size_t triangle_count() const { return indices_.size() / 3; }
    const AABB &bounds() const { return bounds_; }

    void triangle(size_t i, Vec3 out[3]) const {
        out[0] = vertices_[indices_[i * 3 + 0]];
        out[1] = vertices_[indices_[i * 3 + 1]];
        out[2] = vertices_[indices_[i * 3 + 2]];
    }

    // Every triangle whose bounds touch `box`, appended to `out`.
    void query(const AABB &box, std::vector<uint32_t> &out) const;
    // The nearest triangle a ray hits, or -1. `t` is along `dir`,
    // which need not be normalised.
    int32_t raycast(const Vec3 &origin, const Vec3 &dir, float max_t, float *out_t,
                    Vec3 *out_normal) const;

private:
    struct Node {
        AABB bounds;
        // Leaf when count > 0; `first` then indexes `order_`.
        uint32_t first = 0, count = 0;
        // BOTH CHILDREN ARE STORED.
        //
        // The usual trick is to keep only the right child and assume
        // the left one is at `this + 1`. That holds for the root and
        // for nothing else once the tree is built by recursive
        // push_back, and the result is a BVH that traverses into its
        // own siblings -- so every query finds nothing and all mesh
        // collision silently stops working. Two indices cost eight
        // bytes a node.
        uint32_t left = 0, right = 0;
    };
    void subdivide(uint32_t node, int depth);

    std::vector<Vec3> vertices_;
    std::vector<uint32_t> indices_;
    std::vector<uint32_t> order_;      // triangle indices, BVH order
    std::vector<AABB> tri_bounds_;
    std::vector<Vec3> tri_centres_;
    std::vector<Node> nodes_;
    AABB bounds_;
};

// ----------------------------------------------------------- narrow phase

// The closest point on a triangle to `p`, and whether it landed on the
// face rather than an edge or a vertex. Barycentric, branch-light, and
// the workhorse of the character controller.
Vec3 closest_point_on_triangle(const Vec3 &p, const Vec3 &a, const Vec3 &b,
                               const Vec3 &c);
// The closest points between two segments. Degenerate cases (either
// segment a point, the two parallel) are handled rather than producing
// a NaN the caller has to notice.
void closest_points_on_segments(const Vec3 &p1, const Vec3 &q1, const Vec3 &p2,
                                const Vec3 &q2, Vec3 *c1, Vec3 *c2);
// Moller-Trumbore. `t` is along `dir`; the triangle is hit from either
// side, because a portal lets you see and shoot the back of the world.
bool ray_triangle(const Vec3 &origin, const Vec3 &dir, const Vec3 &a,
                  const Vec3 &b, const Vec3 &c, float *out_t);

}  // namespace wr
