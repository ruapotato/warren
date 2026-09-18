#include "procgen/sdf.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "core/suggest.h"
#include "procgen/noise.h"
#include "procgen/surface.h"
#include "render/mesh.h"

namespace wr::gen {

// ------------------------------------------------------------- the node

struct SdfNode {
    enum class Op : uint8_t {
        Sphere, Box, Cylinder, Capsule, Cone, Torus, HalfSpace,
        Union, Intersection, Difference,
        Blend, BlendIntersection, BlendDifference,
        Transform, Round, Shell, Twist, Bend, Displace, Elongate, Mirror,
        Repeat, Material,
    };
    Op op = Op::Sphere;
    float a = 0, b = 0, c = 0;
    Vec3 v;
    // For Transform: the INVERSE, because evaluating a transformed
    // shape means moving the query point into its space, not moving
    // the shape.
    Transform3D inverse;
    float scale = 1.0f;  // the transform's uniform scale
    int octaves = 4;
    uint32_t seed = 1;
    MaterialId material = 1;
    std::vector<SdfRef> children;
    // Worked out once, when the node is built. Contouring asks for
    // this on every chunk and the tree does not change.
    AABB box;
    // A shape that goes on for ever in some direction: a half space,
    // or an unlimited repeat. Its `box` is meaningless.
    bool unbounded = false;
    size_t count = 1;  // nodes in this subtree
};

namespace {

constexpr float kBig = 1e18f;

AABB infinite_box() {
    return AABB(Vec3(-kBig, -kBig, -kBig), Vec3(kBig, kBig, kBig));
}

AABB grown(const AABB &b, float r) {
    return AABB(b.min - Vec3(r, r, r), b.max + Vec3(r, r, r));
}

AABB merged_box(const AABB &a, const AABB &b) {
    return AABB(Vec3(std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y),
                     std::min(a.min.z, b.min.z)),
                Vec3(std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y),
                     std::max(a.max.z, b.max.z)));
}

AABB overlap_box(const AABB &a, const AABB &b) {
    AABB r(Vec3(std::max(a.min.x, b.min.x), std::max(a.min.y, b.min.y),
                std::max(a.min.z, b.min.z)),
           Vec3(std::min(a.max.x, b.max.x), std::min(a.max.y, b.max.y),
                std::min(a.max.z, b.max.z)));
    // An empty intersection is a degenerate box, not a negative one.
    for (int i = 0; i < 3; i++)
        if ((&r.max.x)[i] < (&r.min.x)[i]) (&r.max.x)[i] = (&r.min.x)[i];
    return r;
}

// A box big enough to hold the original under any rotation, which is
// what you get when you transform the eight corners and take their
// bounds.
AABB transformed_box(const AABB &b, const Transform3D &t) {
    AABB out(Vec3(kBig, kBig, kBig), Vec3(-kBig, -kBig, -kBig));
    for (int i = 0; i < 8; i++) {
        const Vec3 corner(i & 1 ? b.max.x : b.min.x, i & 2 ? b.max.y : b.min.y,
                          i & 4 ? b.max.z : b.min.z);
        const Vec3 p = t.xform(corner);
        out = merged_box(out, AABB(p, p));
    }
    return out;
}

// THE SMOOTH MINIMUM, and the reason the blends look like castings
// rather than two shapes sitting next to each other. Quadratic
// polynomial: cheap, C1 continuous, and it does not move the surface
// where the two shapes are far apart.
float smin(float a, float b, float k) {
    if (k <= 0.0f) return std::min(a, b);
    const float h = std::max(0.0f, k - std::fabs(a - b)) / k;
    return std::min(a, b) - h * h * k * 0.25f;
}

float smax(float a, float b, float k) {
    if (k <= 0.0f) return std::max(a, b);
    const float h = std::max(0.0f, k - std::fabs(a - b)) / k;
    return std::max(a, b) + h * h * k * 0.25f;
}

Vec3 abs3(const Vec3 &p) {
    return {std::fabs(p.x), std::fabs(p.y), std::fabs(p.z)};
}

float max3(const Vec3 &p) { return std::max(p.x, std::max(p.y, p.z)); }

// The exact distance to a box: outside is the length of the part of
// the offset that is positive, inside is the largest (least negative)
// component. Both together handle every case with no branches.
float box_distance(const Vec3 &p, const Vec3 &half) {
    const Vec3 q = abs3(p) - half;
    const Vec3 outside(std::max(q.x, 0.0f), std::max(q.y, 0.0f),
                       std::max(q.z, 0.0f));
    return outside.length() + std::min(max3(q), 0.0f);
}

float cylinder_distance(const Vec3 &p, float radius, float half_height) {
    const float dr = std::sqrt(p.x * p.x + p.z * p.z) - radius;
    const float dy = std::fabs(p.y) - half_height;
    const float outside = std::sqrt(std::max(dr, 0.0f) * std::max(dr, 0.0f) +
                                    std::max(dy, 0.0f) * std::max(dy, 0.0f));
    return outside + std::min(std::max(dr, dy), 0.0f);
}

float cone_distance(const Vec3 &p, float radius, float height) {
    // A cone standing on the XZ plane with its tip up, centred on the
    // origin like every other primitive here.
    const float half = height * 0.5f;
    const Vec2 q(std::sqrt(p.x * p.x + p.z * p.z), p.y + half);
    const Vec2 tip(0.0f, height);
    const Vec2 base(radius, 0.0f);
    const Vec2 edge = tip - base;
    const Vec2 to_base = q - base;
    const float t = std::clamp(dot(to_base, edge) / dot(edge, edge), 0.0f, 1.0f);
    const Vec2 on_edge = base + edge * t;
    const float d_side = (q - on_edge).length();
    const float d_base = std::sqrt(std::max(q.x - radius, 0.0f) *
                                       std::max(q.x - radius, 0.0f) +
                                   q.y * q.y);
    float d = std::min(d_side, q.y < 0.0f ? d_base : d_side);
    // Inside is where the point is above the base and inside the
    // sloped side.
    const bool inside = q.y > 0.0f && q.y < height &&
                        q.x < radius * (1.0f - q.y / height);
    return inside ? -std::min(d_side, q.y) : d;
}

std::shared_ptr<SdfNode> make(SdfNode::Op op) {
    auto n = std::make_shared<SdfNode>();
    n->op = op;
    return n;
}

void finish_leaf(const std::shared_ptr<SdfNode> &n, const AABB &box) {
    n->box = box;
    n->count = 1;
}

void finish_unary(const std::shared_ptr<SdfNode> &n, const SdfRef &child,
                  const AABB &box) {
    n->children.push_back(child);
    n->box = box;
    n->unbounded = child->unbounded;
    n->count = child->count + 1;
}

void finish_binary(const std::shared_ptr<SdfNode> &n, const SdfRef &a,
                   const SdfRef &b, const AABB &box, bool unbounded) {
    n->children.push_back(a);
    n->children.push_back(b);
    n->box = box;
    n->unbounded = unbounded;
    n->count = a->count + b->count + 1;
}

// ------------------------------------------------------------ evaluating

// `material` is filled with the id of whichever branch won, so a
// shape assembled from parts can be painted per part. Following it
// through the smooth ops means taking the nearer one, which is the
// only answer that does not flicker across the blend.
float eval(const SdfNode *n, const Vec3 &p, MaterialId *material) {
    if (!n) return kBig;
    switch (n->op) {
        case SdfNode::Op::Sphere:
            if (material) *material = n->material;
            return p.length() - n->a;
        case SdfNode::Op::Box:
            if (material) *material = n->material;
            return box_distance(p, n->v) - n->a;
        case SdfNode::Op::Cylinder:
            if (material) *material = n->material;
            return cylinder_distance(p, n->a, n->b) - n->c;
        case SdfNode::Op::Capsule: {
            if (material) *material = n->material;
            // A segment of length b, thickened by a.
            Vec3 q = p;
            q.y -= std::clamp(q.y, -n->b, n->b);
            return q.length() - n->a;
        }
        case SdfNode::Op::Cone:
            if (material) *material = n->material;
            return cone_distance(p, n->a, n->b) - n->c;
        case SdfNode::Op::Torus: {
            if (material) *material = n->material;
            const float ring = std::sqrt(p.x * p.x + p.z * p.z) - n->a;
            return std::sqrt(ring * ring + p.y * p.y) - n->b;
        }
        case SdfNode::Op::HalfSpace:
            if (material) *material = n->material;
            return dot(p, n->v) - n->a;

        case SdfNode::Op::Union: {
            MaterialId ma = 1, mb = 1;
            const float a = eval(n->children[0].get(), p, &ma);
            const float b = eval(n->children[1].get(), p, &mb);
            if (material) *material = a <= b ? ma : mb;
            return std::min(a, b);
        }
        case SdfNode::Op::Intersection: {
            MaterialId ma = 1, mb = 1;
            const float a = eval(n->children[0].get(), p, &ma);
            const float b = eval(n->children[1].get(), p, &mb);
            if (material) *material = a >= b ? ma : mb;
            return std::max(a, b);
        }
        case SdfNode::Op::Difference: {
            MaterialId ma = 1, mb = 1;
            const float a = eval(n->children[0].get(), p, &ma);
            const float b = eval(n->children[1].get(), p, &mb);
            // The cut face takes the CUTTER's material, which is what
            // lets a hole drilled through painted wood show bare wood
            // inside.
            if (material) *material = a >= -b ? ma : mb;
            return std::max(a, -b);
        }
        case SdfNode::Op::Blend: {
            MaterialId ma = 1, mb = 1;
            const float a = eval(n->children[0].get(), p, &ma);
            const float b = eval(n->children[1].get(), p, &mb);
            if (material) *material = a <= b ? ma : mb;
            return smin(a, b, n->a);
        }
        case SdfNode::Op::BlendIntersection: {
            MaterialId ma = 1, mb = 1;
            const float a = eval(n->children[0].get(), p, &ma);
            const float b = eval(n->children[1].get(), p, &mb);
            if (material) *material = a >= b ? ma : mb;
            return smax(a, b, n->a);
        }
        case SdfNode::Op::BlendDifference: {
            MaterialId ma = 1, mb = 1;
            const float a = eval(n->children[0].get(), p, &ma);
            const float b = eval(n->children[1].get(), p, &mb);
            if (material) *material = a >= -b ? ma : mb;
            return smax(a, -b, n->a);
        }

        case SdfNode::Op::Transform: {
            // The distance scales with the shape. Forgetting this is
            // the classic SDF bug: a shape scaled by two reports
            // distances half what they are, and the mesher walks
            // straight past the surface.
            const Vec3 local = n->inverse.xform(p);
            return eval(n->children[0].get(), local, material) * n->scale;
        }
        case SdfNode::Op::Round:
            return eval(n->children[0].get(), p, material) - n->a;
        case SdfNode::Op::Shell:
            return std::fabs(eval(n->children[0].get(), p, material)) - n->a;
        case SdfNode::Op::Twist: {
            const float angle = n->a * p.y;
            const float s = std::sin(angle), c = std::cos(angle);
            const Vec3 q(c * p.x - s * p.z, p.y, s * p.x + c * p.z);
            // A TWIST IS NOT AN ISOMETRY, so the child's distance is
            // an overestimate out at the rim -- the mesher would step
            // over thin features. Dividing by the worst-case stretch
            // makes it an underestimate, which is safe. The cost is a
            // slightly slower march, which nothing here notices.
            const float radius = std::sqrt(p.x * p.x + p.z * p.z);
            const float stretch = 1.0f + std::fabs(n->a) * radius;
            return eval(n->children[0].get(), q, material) / stretch;
        }
        case SdfNode::Op::Bend: {
            const float angle = n->a * p.x;
            const float s = std::sin(angle), c = std::cos(angle);
            const Vec3 q(c * p.x - s * p.y, s * p.x + c * p.y, p.z);
            const float stretch = 1.0f + std::fabs(n->a) * abs3(p).length();
            return eval(n->children[0].get(), q, material) / stretch;
        }
        case SdfNode::Op::Displace: {
            const float d = eval(n->children[0].get(), p, material);
            const float noise = fbm(p * (1.0f / n->b), n->seed, n->octaves);
            // DIVIDED BY HOW STEEP THE NOISE CAN BE.
            //
            // Adding noise to a distance adds the noise's slope to
            // the field's, so the sum can climb faster than one
            // metre per metre and stop being a distance. `c` holds
            // the bound, worked out once when the node was built.
            //
            // Dividing by a constant is free: it cannot move the
            // zero crossing, both ends of an interpolated edge
            // shrink together, and the gradient is normalised before
            // it is used as a normal. So the surface is identical
            // and the field is honest again.
            return (d + noise * n->a) / n->c;
        }
        case SdfNode::Op::Elongate: {
            const Vec3 q = abs3(p) - n->v;
            const Vec3 clamped(std::max(q.x, 0.0f), std::max(q.y, 0.0f),
                               std::max(q.z, 0.0f));
            const Vec3 sign(p.x < 0 ? -1.0f : 1.0f, p.y < 0 ? -1.0f : 1.0f,
                            p.z < 0 ? -1.0f : 1.0f);
            return eval(n->children[0].get(),
                        Vec3(clamped.x * sign.x, clamped.y * sign.y,
                             clamped.z * sign.z),
                        material);
        }
        case SdfNode::Op::Mirror: {
            Vec3 q = p;
            if (n->v.x != 0.0f) q.x = std::fabs(q.x);
            if (n->v.y != 0.0f) q.y = std::fabs(q.y);
            if (n->v.z != 0.0f) q.z = std::fabs(q.z);
            return eval(n->children[0].get(), q, material);
        }
        case SdfNode::Op::Repeat: {
            Vec3 q = p;
            for (int i = 0; i < 3; i++) {
                const float spacing = (&n->v.x)[i];
                if (spacing <= 0.0f) continue;
                const float limit = (&n->box.max.x)[i] < kBig ? (&n->inverse.origin.x)[i]
                                                              : 0.0f;
                float cell = std::round((&q.x)[i] / spacing);
                if (limit > 0.0f) cell = std::clamp(cell, 0.0f, limit - 1.0f);
                (&q.x)[i] -= spacing * cell;
            }
            return eval(n->children[0].get(), q, material);
        }
        case SdfNode::Op::Material: {
            const float d = eval(n->children[0].get(), p, nullptr);
            if (material) *material = n->material;
            return d;
        }
    }
    return kBig;
}

}  // namespace

// ------------------------------------------------------------ primitives

Sdf Sdf::sphere(float radius) {
    auto n = make(SdfNode::Op::Sphere);
    n->a = std::max(radius, 0.0f);
    finish_leaf(n, AABB(Vec3(-n->a, -n->a, -n->a), Vec3(n->a, n->a, n->a)));
    return Sdf(n);
}

Sdf Sdf::box(const Vec3 &size, float round) {
    auto n = make(SdfNode::Op::Box);
    // The rounding is taken OUT of the half extents so that a rounded
    // box is the size you asked for. A caller who says "a 1m cube
    // with 10cm corners" does not mean a 1.2m cube.
    const float r = std::max(round, 0.0f);
    n->v = Vec3(std::max(size.x * 0.5f - r, 0.0f), std::max(size.y * 0.5f - r, 0.0f),
                std::max(size.z * 0.5f - r, 0.0f));
    n->a = r;
    finish_leaf(n, AABB(-(n->v + Vec3(r, r, r)), n->v + Vec3(r, r, r)));
    return Sdf(n);
}

Sdf Sdf::cylinder(float radius, float height, float round) {
    auto n = make(SdfNode::Op::Cylinder);
    const float r = std::max(round, 0.0f);
    n->a = std::max(radius - r, 0.0f);
    n->b = std::max(height * 0.5f - r, 0.0f);
    n->c = r;
    finish_leaf(n, AABB(Vec3(-radius, -height * 0.5f, -radius),
                        Vec3(radius, height * 0.5f, radius)));
    return Sdf(n);
}

Sdf Sdf::capsule(float radius, float height) {
    auto n = make(SdfNode::Op::Capsule);
    n->a = std::max(radius, 0.0f);
    // The half-segment. `height` is between the cap centres, which
    // is what the physics capsule and Mesh::capsule both mean by it.
    n->b = std::max(height * 0.5f, 0.0f);
    const float half = n->b + n->a;
    finish_leaf(n, AABB(Vec3(-n->a, -half, -n->a), Vec3(n->a, half, n->a)));
    return Sdf(n);
}

Sdf Sdf::cone(float radius, float height, float round) {
    auto n = make(SdfNode::Op::Cone);
    n->a = std::max(radius, 0.0f);
    n->b = std::max(height, 0.0f);
    n->c = std::max(round, 0.0f);
    finish_leaf(n, AABB(Vec3(-radius - n->c, -height * 0.5f - n->c, -radius - n->c),
                        Vec3(radius + n->c, height * 0.5f + n->c, radius + n->c)));
    return Sdf(n);
}

Sdf Sdf::torus(float major, float minor) {
    auto n = make(SdfNode::Op::Torus);
    n->a = std::max(major, 0.0f);
    n->b = std::max(minor, 0.0f);
    const float r = n->a + n->b;
    finish_leaf(n, AABB(Vec3(-r, -n->b, -r), Vec3(r, n->b, r)));
    return Sdf(n);
}

Sdf Sdf::half_space(const Vec3 &normal, float offset) {
    auto n = make(SdfNode::Op::HalfSpace);
    n->v = normal.normalized();
    n->a = offset;
    finish_leaf(n, infinite_box());
    n->unbounded = true;
    return Sdf(n);
}

// ------------------------------------------------------------- combining

namespace {

Sdf binary(SdfNode::Op op, const Sdf &lhs, const Sdf &rhs, float k) {
    // An empty operand is not an error: building a shape by adding
    // parts in a loop starts with nothing, and every one of those
    // loops would otherwise need a first-time branch.
    if (lhs.is_empty()) {
        if (op == SdfNode::Op::Union || op == SdfNode::Op::Blend) return rhs;
        return Sdf();
    }
    if (rhs.is_empty()) return lhs;

    auto n = make(op);
    n->a = std::max(k, 0.0f);
    const AABB &a = lhs.root()->box;
    const AABB &b = rhs.root()->box;
    AABB box;
    bool unbounded = false;
    switch (op) {
        case SdfNode::Op::Union:
        case SdfNode::Op::Blend:
            box = grown(merged_box(a, b), n->a);
            unbounded = lhs.root()->unbounded || rhs.root()->unbounded;
            break;
        case SdfNode::Op::Intersection:
        case SdfNode::Op::BlendIntersection:
            // An intersection is inside BOTH, so an unbounded operand
            // stops mattering -- which is exactly how a half space
            // becomes useful.
            box = grown(overlap_box(a, b), n->a);
            unbounded = lhs.root()->unbounded && rhs.root()->unbounded;
            break;
        default:
            // Subtracting can only remove material, so the result
            // still fits in the first operand's box.
            box = grown(a, n->a);
            unbounded = lhs.root()->unbounded;
            break;
    }
    finish_binary(n, lhs.root(), rhs.root(), box, unbounded);
    return Sdf(n);
}

}  // namespace

Sdf Sdf::merged(const Sdf &o) const { return binary(SdfNode::Op::Union, *this, o, 0); }
Sdf Sdf::intersected(const Sdf &o) const {
    return binary(SdfNode::Op::Intersection, *this, o, 0);
}
Sdf Sdf::subtracted(const Sdf &o) const {
    return binary(SdfNode::Op::Difference, *this, o, 0);
}
Sdf Sdf::blended(const Sdf &o, float k) const {
    return binary(SdfNode::Op::Blend, *this, o, k);
}
Sdf Sdf::blend_intersected(const Sdf &o, float k) const {
    return binary(SdfNode::Op::BlendIntersection, *this, o, k);
}
Sdf Sdf::blend_subtracted(const Sdf &o, float k) const {
    return binary(SdfNode::Op::BlendDifference, *this, o, k);
}

// --------------------------------------------------------------- placing

Sdf Sdf::transformed(const Transform3D &t) const {
    if (!root_) return *this;
    auto n = make(SdfNode::Op::Transform);
    n->inverse = t.inverse();
    n->scale = t.basis.uniform_scale();
    if (n->scale <= 1e-6f) {
        WR_WARN("sdf: a transform with zero scale collapses the shape");
        n->scale = 1e-6f;
    }
    finish_unary(n, root_, root_->unbounded ? infinite_box()
                                            : transformed_box(root_->box, t));
    return Sdf(n);
}

Sdf Sdf::translated(const Vec3 &by) const {
    return transformed(Transform3D::translation(by));
}
Sdf Sdf::rotated(const Vec3 &axis, float radians) const {
    return transformed(Transform3D::rotation(axis, radians));
}
Sdf Sdf::scaled(float by) const { return transformed(Transform3D::scaling(by)); }

// ------------------------------------------------------------- reshaping

Sdf Sdf::rounded(float r) const {
    if (!root_ || r == 0.0f) return *this;
    auto n = make(SdfNode::Op::Round);
    n->a = r;
    finish_unary(n, root_, grown(root_->box, std::max(r, 0.0f)));
    return Sdf(n);
}

Sdf Sdf::shelled(float thickness) const {
    if (!root_) return *this;
    auto n = make(SdfNode::Op::Shell);
    n->a = std::max(thickness, 0.0f) * 0.5f;
    finish_unary(n, root_, grown(root_->box, n->a));
    return Sdf(n);
}

Sdf Sdf::twisted(float turns_per_metre) const {
    if (!root_) return *this;
    auto n = make(SdfNode::Op::Twist);
    n->a = turns_per_metre * 6.283185307179586f;
    // Twisting about Y sweeps the shape's horizontal extent around,
    // so the box becomes the circle that contains it.
    const AABB &b = root_->box;
    const float r = std::max(std::max(std::fabs(b.min.x), std::fabs(b.max.x)),
                             std::max(std::fabs(b.min.z), std::fabs(b.max.z))) *
                    1.4143f;
    finish_unary(n, root_, AABB(Vec3(-r, b.min.y, -r), Vec3(r, b.max.y, r)));
    return Sdf(n);
}

Sdf Sdf::bent(float curvature) const {
    if (!root_) return *this;
    auto n = make(SdfNode::Op::Bend);
    n->a = curvature;
    // A bend sweeps X into Y, so both grow to the larger of the two.
    const AABB &b = root_->box;
    const float r = std::max(b.max.x - b.min.x, b.max.y - b.min.y);
    finish_unary(n, root_,
                 AABB(Vec3(-r, -r, b.min.z), Vec3(r, r, b.max.z)));
    return Sdf(n);
}

Sdf Sdf::displaced(float amplitude, float scale, int octaves, uint32_t seed) const {
    if (!root_) return *this;
    auto n = make(SdfNode::Op::Displace);
    n->a = amplitude;
    n->b = std::max(scale, 1e-4f);
    n->octaves = std::clamp(octaves, 1, 8);
    n->seed = seed;
    // Each octave of fbm halves its amplitude and doubles its
    // frequency, so every one contributes the same slope: the
    // steepest Perlin gradient, over the feature size. With the
    // default lacunarity and gain the octaves simply add up.
    constexpr float kPerlinSlope = 2.0f;
    n->c = 1.0f + std::fabs(amplitude) * float(n->octaves) * kPerlinSlope / n->b;
    finish_unary(n, root_, grown(root_->box, std::fabs(amplitude)));
    return Sdf(n);
}

Sdf Sdf::elongated(const Vec3 &by) const {
    if (!root_) return *this;
    auto n = make(SdfNode::Op::Elongate);
    n->v = Vec3(std::fabs(by.x), std::fabs(by.y), std::fabs(by.z)) * 0.5f;
    finish_unary(n, root_, AABB(root_->box.min - n->v, root_->box.max + n->v));
    return Sdf(n);
}

Sdf Sdf::mirrored(bool x, bool y, bool z) const {
    if (!root_ || (!x && !y && !z)) return *this;
    auto n = make(SdfNode::Op::Mirror);
    n->v = Vec3(x ? 1.0f : 0.0f, y ? 1.0f : 0.0f, z ? 1.0f : 0.0f);
    AABB b = root_->box;
    // Only the positive side is sampled, so the result is that side
    // and its reflection.
    if (x) { b.max.x = std::max(b.max.x, -b.min.x); b.min.x = -b.max.x; }
    if (y) { b.max.y = std::max(b.max.y, -b.min.y); b.min.y = -b.max.y; }
    if (z) { b.max.z = std::max(b.max.z, -b.min.z); b.min.z = -b.max.z; }
    finish_unary(n, root_, b);
    return Sdf(n);
}

Sdf Sdf::repeated(const Vec3 &spacing, const Vec3 &count) const {
    if (!root_) return *this;
    auto n = make(SdfNode::Op::Repeat);
    n->v = spacing;
    // The counts ride in the transform's origin, which is otherwise
    // unused on this op -- one field rather than a fourth vector.
    n->inverse.origin = Vec3(std::max(count.x, 0.0f), std::max(count.y, 0.0f),
                             std::max(count.z, 0.0f));
    AABB b = root_->box;
    bool unbounded = root_->unbounded;
    for (int i = 0; i < 3; i++) {
        const float s = (&spacing.x)[i];
        if (s <= 0.0f) continue;
        const float c = (&n->inverse.origin.x)[i];
        if (c <= 0.0f) {
            unbounded = true;
            (&b.min.x)[i] = -kBig;
            (&b.max.x)[i] = kBig;
        } else {
            (&b.max.x)[i] += s * (c - 1.0f);
        }
    }
    finish_unary(n, root_, b);
    n->unbounded = unbounded;
    return Sdf(n);
}

Sdf Sdf::with_material(MaterialId m) const {
    if (!root_) return *this;
    auto n = make(SdfNode::Op::Material);
    n->material = m;
    finish_unary(n, root_, root_->box);
    return Sdf(n);
}

// --------------------------------------------------------------- reading

Sample Sdf::sample(const Vec3 &p) const {
    if (!root_) return {kBig, 0};
    MaterialId m = 1;
    const float d = eval(root_.get(), p, &m);
    return {d, d <= 0.0f ? m : MaterialId(0)};
}

Vec3 Sdf::gradient(const Vec3 &p, float h) const {
    if (!root_) return Vec3::up();
    // The tetrahedron trick: four samples rather than six, and the
    // same accuracy.
    const float k = h > 0.0f ? h : 1e-3f;
    const Vec3 a(1, -1, -1), b(-1, -1, 1), c(-1, 1, -1), d(1, 1, 1);
    return (a * eval(root_.get(), p + a * k, nullptr) +
            b * eval(root_.get(), p + b * k, nullptr) +
            c * eval(root_.get(), p + c * k, nullptr) +
            d * eval(root_.get(), p + d * k, nullptr));
}

MaterialId Sdf::surface_material(const Vec3 &p, const Vec3 &) const {
    if (!root_) return 0;
    MaterialId m = 1;
    eval(root_.get(), p, &m);
    return m;
}

float Sdf::bound(const AABB &box) const {
    if (!root_ || root_->unbounded) return 0.0f;
    // How far the query box is from the shape's box. Positive means
    // they do not overlap and nothing inside can be closer than that,
    // which lets a mesher skip the whole region.
    const AABB &s = root_->box;
    float sq = 0.0f;
    for (int i = 0; i < 3; i++) {
        const float lo = (&s.min.x)[i] - (&box.max.x)[i];
        const float hi = (&box.min.x)[i] - (&s.max.x)[i];
        const float d = std::max(lo, hi);
        if (d > 0.0f) sq += d * d;
    }
    return std::sqrt(sq);
}

AABB Sdf::bounds() const { return root_ ? root_->box : AABB(); }
bool Sdf::bounded() const { return root_ && !root_->unbounded; }
size_t Sdf::node_count() const { return root_ ? root_->count : 0; }

// --------------------------------------------------------------- meshing

Ref<Mesh> Sdf::to_mesh(const MeshOptions &options, std::string *error) const {
    auto fail = [&](const char *why) {
        if (error) *error = why;
        return Ref<Mesh>();
    };
    if (!root_) return fail("the shape is empty");
    if (root_->unbounded)
        return fail(
            "the shape is unbounded -- a half space or an unlimited repeat has "
            "to be intersected with something before it can be meshed");

    // A cell of padding all round, so a surface that touches the
    // bounding box is still closed rather than sliced open.
    AABB box = root_->box;
    const Vec3 size = box.max - box.min;
    const float longest = std::max(size.x, std::max(size.y, size.z));
    if (longest <= 0.0f) return fail("the shape has no extent");

    float cell = options.cell_size > 0.0f
                     ? options.cell_size
                     : longest / float(std::max(options.target_cells, 1));
    const int limit = std::max(options.max_cells_per_axis, 8);
    if (longest / cell > float(limit)) {
        const float coarser = longest / float(limit);
        WR_WARN("sdf: %.4g m cells would need %d along the longest axis; using "
                "%.4g m instead",
                double(cell), int(longest / cell), double(coarser));
        cell = coarser;
    }
    box = grown(box, cell * 1.5f);

    ContourRequest req;
    req.field = this;
    req.origin = box.min;
    req.cell_size = cell;
    // One resolution for all three axes, because the contourer takes
    // a cube. The empty cells outside the shape cost a sample each
    // and no triangles.
    const Vec3 padded = box.max - box.min;
    req.resolution = int(std::ceil(std::max(padded.x, std::max(padded.y, padded.z)) /
                                   cell)) + 1;
    req.smooth_normals = options.smooth_normals;

    ContourResult result;
    contour_field(req, &result);
    if (result.empty || result.indices.empty())
        return fail("nothing crossed the surface -- check the size and the cell");

    Ref<Mesh> mesh(new Mesh());
    mesh->vertices = std::move(result.vertices);
    mesh->indices = std::move(result.indices);
    mesh->append({}, {}, 0, "sdf");
    mesh->submeshes.clear();
    SubMesh sm;
    sm.first_index = 0;
    sm.index_count = uint32_t(mesh->indices.size());
    sm.bounds = result.bounds;
    sm.name = "sdf";
    mesh->submeshes.push_back(sm);
    if (options.weld) mesh->weld(cell * 1e-3f);
    if (options.simplify_error > 0.0f) {
        const size_t before = mesh->triangle_count();
        const float floor_ratio =
            before ? float(options.min_triangles) / float(before) : 1.0f;
        mesh->simplify(std::min(floor_ratio, 1.0f),
                       options.simplify_error * cell);
    }
    // AFTER the decimation, not before: collapsing edges moves
    // vertices, and a normal computed for where a vertex used to be
    // is worse than no normal at all.
    if (!options.smooth_normals)
        mesh->compute_normals(deg2rad(options.smooth_angle_degrees));
    mesh->compute_tangents();
    mesh->compute_bounds();
    return mesh;
}

// ------------------------------------------------------------ as JSON
//
// The format is written for somebody -- or something -- composing a
// shape in one message rather than in thirty calls. So a node is one
// object, placement and modifiers ride on the same object as the
// shape they apply to, and the nesting matches how the shape would
// be described out loud:
//
//   {"op": "difference", "of": [
//       {"shape": "box", "size": [2, 1, 2], "round": 0.05},
//       {"shape": "cylinder", "radius": 0.3, "height": 3, "at": [0.6, 0, 0.6]}]}
//
// Order of application on one node: the shape, then its modifiers,
// then its material, then its placement. Placement last is what
// makes "at" mean where the finished part goes.

namespace {

struct OpName {
    const char *name;
    SdfNode::Op op;
};

const OpName kBinaryOps[] = {
    {"union", SdfNode::Op::Union},
    {"intersection", SdfNode::Op::Intersection},
    {"difference", SdfNode::Op::Difference},
};

const char *const kShapeNames[] = {"sphere",  "box",   "cylinder", "capsule",
                                   "cone",    "torus", "half_space"};

std::string list_of(const std::vector<std::string> &names, const char *last) {
    std::string s;
    for (size_t i = 0; i < names.size(); i++) {
        if (i) s += i + 1 == names.size() ? last : ", ";
        s += "\"" + names[i] + "\"";
    }
    return s;
}

std::string suggestion_for(const std::string &wanted,
                           const std::vector<std::string> &have) {
    const std::vector<std::string> near = suggest(wanted, have, 3);
    if (!near.empty()) return " -- did you mean " + list_of(near, " or ") + "?";
    // NOTHING WAS CLOSE, which is the case where a caller is most
    // stuck. "substract" is nowhere near "difference" by any string
    // measure, and answering with silence leaves them guessing. The
    // list is short enough to print, so print it.
    return " -- it should be one of " + list_of(have, " or ");
}

bool read_vec3(const Json &j, Vec3 *out, std::string *error, const char *what) {
    if (j.type() == Json::Type::Number) {
        *out = Vec3(float(j.number()));
        return true;
    }
    if (j.type() == Json::Type::Array && j.size() == 3) {
        *out = Vec3(float(j[0].number()), float(j[1].number()), float(j[2].number()));
        return true;
    }
    if (j.type() == Json::Type::Object) {
        *out = Vec3(float(j["x"].number()), float(j["y"].number()),
                    float(j["z"].number()));
        return true;
    }
    *error = std::string(what) + " should be a number, [x, y, z] or {x, y, z}";
    return false;
}

float number_or(const Json &j, const char *key, float fallback) {
    const Json &v = j[key];
    return v.type() == Json::Type::Number ? float(v.number()) : fallback;
}

Json vec3_json(const Vec3 &v) {
    Json j = Json::array();
    j.push(Json(double(v.x))).push(Json(double(v.y))).push(Json(double(v.z)));
    return j;
}

}  // namespace

Sdf Sdf::from_json(const Json &j, std::string *error) {
    std::string ignored;
    if (!error) error = &ignored;
    error->clear();

    if (j.type() != Json::Type::Object) {
        *error = "a shape is a JSON object with either \"shape\" or \"op\"";
        return Sdf();
    }

    Sdf result;

    // --- an operation over children --------------------------------
    if (j.has("op")) {
        const std::string op = j["op"].string();
        const Json &of = j["of"];
        if (of.type() != Json::Type::Array || of.size() == 0) {
            *error = "\"" + op + "\" needs \"of\": a list of shapes";
            return Sdf();
        }
        const float blend = number_or(j, "blend", 0.0f);
        const SdfNode::Op *chosen = nullptr;
        for (const OpName &o : kBinaryOps)
            if (op == o.name) chosen = &o.op;
        if (!chosen) {
            std::vector<std::string> names;
            for (const OpName &o : kBinaryOps) names.push_back(o.name);
            *error = "no operation called \"" + op + "\"" + suggestion_for(op, names);
            return Sdf();
        }
        // FOLDED LEFT, so "of" may list any number of shapes. A
        // union of five is four unions, and writing it as five
        // nested pairs is something no caller should have to do.
        for (size_t i = 0; i < of.size(); i++) {
            std::string child_error;
            const Sdf child = from_json(of[i], &child_error);
            if (!child_error.empty()) {
                *error = "in \"" + op + "\" child " + std::to_string(i) + ": " +
                         child_error;
                return Sdf();
            }
            if (i == 0) {
                result = child;
                continue;
            }
            switch (*chosen) {
                case SdfNode::Op::Union:
                    result = blend > 0.0f ? result.blended(child, blend)
                                          : result.merged(child);
                    break;
                case SdfNode::Op::Intersection:
                    result = blend > 0.0f ? result.blend_intersected(child, blend)
                                          : result.intersected(child);
                    break;
                default:
                    result = blend > 0.0f ? result.blend_subtracted(child, blend)
                                          : result.subtracted(child);
                    break;
            }
        }
    } else if (j.has("shape")) {
        const std::string shape = j["shape"].string();
        if (shape == "sphere") {
            result = sphere(number_or(j, "radius", 0.5f));
        } else if (shape == "box") {
            Vec3 size(1, 1, 1);
            if (j.has("size") && !read_vec3(j["size"], &size, error, "size"))
                return Sdf();
            result = box(size, number_or(j, "round", 0.0f));
        } else if (shape == "cylinder") {
            result = cylinder(number_or(j, "radius", 0.5f),
                              number_or(j, "height", 1.0f),
                              number_or(j, "round", 0.0f));
        } else if (shape == "capsule") {
            result = capsule(number_or(j, "radius", 0.25f),
                             number_or(j, "height", 1.0f));
        } else if (shape == "cone") {
            result = cone(number_or(j, "radius", 0.5f), number_or(j, "height", 1.0f),
                          number_or(j, "round", 0.0f));
        } else if (shape == "torus") {
            result = torus(number_or(j, "major", 0.5f), number_or(j, "minor", 0.15f));
        } else if (shape == "half_space") {
            Vec3 normal = Vec3::up();
            if (j.has("normal") && !read_vec3(j["normal"], &normal, error, "normal"))
                return Sdf();
            result = half_space(normal, number_or(j, "offset", 0.0f));
        } else {
            std::vector<std::string> names(std::begin(kShapeNames),
                                           std::end(kShapeNames));
            *error = "no shape called \"" + shape + "\"" + suggestion_for(shape, names);
            return Sdf();
        }
    } else {
        // The commonest mistake will be a nearly-right key, so say
        // which keys this object does have.
        *error = "a shape needs \"shape\" (a primitive) or \"op\" (a combination)";
        std::vector<std::string> keys;
        for (const auto &kv : j.fields()) keys.push_back(kv.first);
        const std::vector<std::string> near = suggest("shape", keys, 2);
        if (!near.empty()) *error += "; this one has \"" + near[0] + "\"";
        return Sdf();
    }

    // --- modifiers, in the order they read -------------------------
    if (j.has("shell")) result = result.shelled(number_or(j, "shell", 0.0f));
    if (j.has("twist")) result = result.twisted(number_or(j, "twist", 0.0f));
    if (j.has("bend")) result = result.bent(number_or(j, "bend", 0.0f));
    if (j.has("elongate")) {
        Vec3 by;
        if (!read_vec3(j["elongate"], &by, error, "elongate")) return Sdf();
        result = result.elongated(by);
    }
    if (j.has("mirror")) {
        Vec3 axes;
        if (!read_vec3(j["mirror"], &axes, error, "mirror")) return Sdf();
        result = result.mirrored(axes.x != 0, axes.y != 0, axes.z != 0);
    }
    if (j.has("repeat")) {
        Vec3 spacing, count;
        if (!read_vec3(j["repeat"], &spacing, error, "repeat")) return Sdf();
        if (j.has("count") && !read_vec3(j["count"], &count, error, "count"))
            return Sdf();
        result = result.repeated(spacing, count);
    }
    if (j.has("displace")) {
        const Json &d = j["displace"];
        if (d.type() == Json::Type::Number) {
            result = result.displaced(float(d.number()));
        } else if (d.type() == Json::Type::Object) {
            result = result.displaced(number_or(d, "amplitude", 0.05f),
                                      number_or(d, "scale", 1.0f),
                                      int(number_or(d, "octaves", 4)),
                                      uint32_t(number_or(d, "seed", 1)));
        } else {
            *error = "displace should be a number or {amplitude, scale, octaves, seed}";
            return Sdf();
        }
    }
    // Rounding a primitive is done by the primitive, which keeps its
    // size honest; rounding an operation happens here.
    if (j.has("round") && j.has("op")) result = result.rounded(number_or(j, "round", 0));
    if (j.has("material"))
        result = result.with_material(MaterialId(number_or(j, "material", 1)));

    // --- placement, last -------------------------------------------
    {
        Transform3D t;
        bool placed = false;
        if (j.has("scale")) {
            t.basis = Basis::uniform(number_or(j, "scale", 1.0f));
            placed = true;
        }
        if (j.has("rotate")) {
            Vec3 e;
            if (!read_vec3(j["rotate"], &e, error, "rotate")) return Sdf();
            const float k = 0.017453292519943295f;
            // Degrees, and x, y, z are turns ABOUT those axes, which
            // is what everyone outside this file means by them.
            t.basis = Basis::from_euler_yxz(e.y * k, e.x * k, e.z * k) * t.basis;
            placed = true;
        }
        if (j.has("at")) {
            if (!read_vec3(j["at"], &t.origin, error, "at")) return Sdf();
            placed = true;
        }
        if (placed) result = result.transformed(t);
    }
    return result;
}

namespace {

Json node_json(const SdfNode *n);

// A modifier writes itself as a key on the shape it wraps, which is
// compact and reads well -- right up until the same modifier appears
// twice in a row, where the outer one would silently overwrite the
// inner. Then the child is wrapped in a one-element union, which
// from_json folds straight back out.
Json with_key(Json child, const char *key) {
    if (!child.has(key)) return child;
    Json wrapper = Json::object();
    wrapper.set("op", "union");
    wrapper.set("of", Json::array().push(child));
    return wrapper;
}

Json node_json(const SdfNode *n) {
    Json j = Json::object();
    if (!n) return j;
    auto child = [&](int i) { return node_json(n->children[size_t(i)].get()); };
    switch (n->op) {
        case SdfNode::Op::Sphere:
            j.set("shape", "sphere").set("radius", double(n->a));
            break;
        case SdfNode::Op::Box:
            j.set("shape", "box");
            j.set("size", vec3_json((n->v + Vec3(n->a, n->a, n->a)) * 2.0f));
            if (n->a > 0) j.set("round", double(n->a));
            break;
        case SdfNode::Op::Cylinder:
            j.set("shape", "cylinder").set("radius", double(n->a + n->c));
            j.set("height", double((n->b + n->c) * 2.0f));
            if (n->c > 0) j.set("round", double(n->c));
            break;
        case SdfNode::Op::Capsule:
            j.set("shape", "capsule").set("radius", double(n->a));
            j.set("height", double(n->b * 2.0f));
            break;
        case SdfNode::Op::Cone:
            j.set("shape", "cone").set("radius", double(n->a));
            j.set("height", double(n->b));
            if (n->c > 0) j.set("round", double(n->c));
            break;
        case SdfNode::Op::Torus:
            j.set("shape", "torus").set("major", double(n->a));
            j.set("minor", double(n->b));
            break;
        case SdfNode::Op::HalfSpace:
            j.set("shape", "half_space").set("normal", vec3_json(n->v));
            j.set("offset", double(n->a));
            break;
        case SdfNode::Op::Union:
        case SdfNode::Op::Blend:
            j.set("op", "union");
            j.set("of", Json::array().push(child(0)).push(child(1)));
            if (n->a > 0) j.set("blend", double(n->a));
            break;
        case SdfNode::Op::Intersection:
        case SdfNode::Op::BlendIntersection:
            j.set("op", "intersection");
            j.set("of", Json::array().push(child(0)).push(child(1)));
            if (n->a > 0) j.set("blend", double(n->a));
            break;
        case SdfNode::Op::Difference:
        case SdfNode::Op::BlendDifference:
            j.set("op", "difference");
            j.set("of", Json::array().push(child(0)).push(child(1)));
            if (n->a > 0) j.set("blend", double(n->a));
            break;
        case SdfNode::Op::Transform: {
            // A transform is not a node in the written form -- it is
            // "at", "rotate" and "scale" on the child -- so it folds
            // into whatever it wraps.
            const Transform3D t = n->inverse.inverse();
            j = child(0);
            for (const char *key : {"at", "rotate", "scale"}) j = with_key(j, key);
            if (t.origin.length_sq() > 0) j.set("at", vec3_json(t.origin));
            const Vec3 e = t.basis.orthonormalized().to_euler_yxz();
            if (e.length_sq() > 1e-8f) {
                const double deg = 57.29577951308232;
                Json r = Json::array();
                r.push(Json(double(e.y) * deg)).push(Json(double(e.x) * deg));
                r.push(Json(double(e.z) * deg));
                j.set("rotate", r);
            }
            if (std::fabs(n->scale - 1.0f) > 1e-6f)
                j.set("scale", double(1.0f / n->scale));
            break;
        }
        case SdfNode::Op::Round:
            j = with_key(child(0), "round");
            j.set("round", double(n->a));
            break;
        case SdfNode::Op::Shell:
            j = with_key(child(0), "shell");
            j.set("shell", double(n->a * 2.0f));
            break;
        case SdfNode::Op::Twist:
            j = with_key(child(0), "twist");
            j.set("twist", double(n->a / 6.283185307179586));
            break;
        case SdfNode::Op::Bend:
            j = with_key(child(0), "bend");
            j.set("bend", double(n->a));
            break;
        case SdfNode::Op::Displace: {
            j = with_key(child(0), "displace");
            Json d = Json::object();
            d.set("amplitude", double(n->a)).set("scale", double(n->b));
            d.set("octaves", n->octaves).set("seed", double(n->seed));
            j.set("displace", d);
            break;
        }
        case SdfNode::Op::Elongate:
            j = with_key(child(0), "elongate");
            j.set("elongate", vec3_json(n->v * 2.0f));
            break;
        case SdfNode::Op::Mirror:
            j = with_key(child(0), "mirror");
            j.set("mirror", vec3_json(n->v));
            break;
        case SdfNode::Op::Repeat:
            j = with_key(with_key(child(0), "repeat"), "count");
            j.set("repeat", vec3_json(n->v));
            j.set("count", vec3_json(n->inverse.origin));
            break;
        case SdfNode::Op::Material:
            j = with_key(child(0), "material");
            j.set("material", double(n->material));
            break;
    }
    return j;
}

}  // namespace

Json Sdf::to_json() const { return root_ ? node_json(root_.get()) : Json::object(); }

}  // namespace wr::gen
