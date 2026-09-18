// Warren -- shapes described as formulas.
//
// An Sdf is a shape before it is triangles: a function from a point
// to how far that point is from the surface. Building one is cheap,
// combining two is a `min`, and turning the result into a mesh is
// contouring, which is somebody else's problem (surface.h) and
// already solved.
//
// WHY THIS AND NOT MESH BOOLEANS.
//
// Cutting one triangle mesh out of another is a famously nasty
// problem: it needs exact predicates to decide which side of a plane
// a point is on, it produces slivers, and it falls over on coplanar
// faces -- which are exactly what you get when you subtract a box
// from a box, which is exactly what everybody does first. Subtracting
// one field from another is `max(a, -b)`. It cannot fail, it has no
// special cases, and it costs one instruction.
//
// The price is that the result is contoured rather than exact, so a
// flat face comes back as a grid of triangles rather than two. For
// procedural props -- which is what this is for -- that is a good
// trade, and dual contouring keeps the sharp edges that made you want
// the boolean in the first place.
//
// It also buys two things mesh booleans cannot do at all: smooth
// blends, where two shapes merge with a fillet rather than a crease,
// and deformations -- twist, bend, displace -- applied to the whole
// tree at once.
//
// SHARING AND THREADS. An Sdf is an immutable value holding a shared
// pointer to its tree. Copying one is a refcount, and every method
// returns a new Sdf rather than changing this one, so the mesher can
// hand the same shape to eight worker threads without a lock.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/json.h"
#include "procgen/field.h"

namespace wr {
class Mesh;
template <class T>
class Ref;
}  // namespace wr

namespace wr::gen {

struct SdfNode;
using SdfRef = std::shared_ptr<const SdfNode>;

class Sdf : public Field {
public:
    Sdf() = default;  // empty: infinitely far from anything

    // --- primitives, all centred on the origin ------------------------
    static Sdf sphere(float radius = 0.5f);
    // `round` rounds the corners without growing the box, which is
    // what somebody asking for a rounded box means.
    static Sdf box(const Vec3 &size = Vec3::one(), float round = 0.0f);
    static Sdf cylinder(float radius = 0.5f, float height = 1.0f,
                        float round = 0.0f);
    // `height` is the distance BETWEEN the cap centres, so the whole
    // thing is height + 2 * radius tall -- the same convention as
    // the physics capsule and Mesh::capsule, so a shape, its
    // collider and its mesh agree without a fudge factor.
    static Sdf capsule(float radius = 0.25f, float height = 1.0f);
    static Sdf cone(float radius = 0.5f, float height = 1.0f, float round = 0.0f);
    static Sdf torus(float major = 0.5f, float minor = 0.15f);
    // A half space: everything on the negative side of the plane is
    // solid. Unbounded, so it is only useful intersected with
    // something else -- which is how you slice a shape in half.
    static Sdf half_space(const Vec3 &normal = Vec3::up(), float offset = 0.0f);

    bool is_empty() const { return !root_; }

    // --- combining ----------------------------------------------------
    Sdf merged(const Sdf &other) const;        // both
    Sdf intersected(const Sdf &other) const;   // the overlap
    Sdf subtracted(const Sdf &other) const;    // this, minus that
    // `k` is the width of the blend in metres. The three above are
    // these with k = 0, and a smooth union is the reason anybody
    // models this way rather than with boxes.
    Sdf blended(const Sdf &other, float k) const;
    Sdf blend_intersected(const Sdf &other, float k) const;
    Sdf blend_subtracted(const Sdf &other, float k) const;

    // --- placing ------------------------------------------------------
    Sdf translated(const Vec3 &by) const;
    Sdf rotated(const Vec3 &axis, float radians) const;
    Sdf scaled(float by) const;
    Sdf transformed(const Transform3D &t) const;

    // --- reshaping ----------------------------------------------------
    // Grow the shape by r in every direction, rounding every edge.
    Sdf rounded(float r) const;
    // Replace the solid with a shell of that thickness around its
    // surface. A sphere becomes a hollow ball; a box becomes a
    // closed crate with walls.
    Sdf shelled(float thickness) const;
    // Turns per metre about the Y axis.
    Sdf twisted(float turns_per_metre) const;
    // Bends about the Z axis as X increases -- an arch, a hook.
    Sdf bent(float curvature) const;
    // Fractal noise added to the surface. What turns a sphere into a
    // rock and a cylinder into a tree trunk.
    Sdf displaced(float amplitude, float scale = 1.0f, int octaves = 4,
                  uint32_t seed = 1) const;
    // Stretch the shape by sliding its halves apart, which keeps the
    // ends round instead of scaling them oval.
    Sdf elongated(const Vec3 &by) const;
    // Mirror through the origin on the given axes, so only half the
    // shape need be built.
    Sdf mirrored(bool x, bool y = false, bool z = false) const;
    // Copies on a lattice. `count` of zero on an axis repeats for
    // ever, which is what a floor tile wants and what a bounding box
    // cannot express -- so a repeat with any zero is unbounded and
    // must be intersected with something before it can be meshed.
    Sdf repeated(const Vec3 &spacing, const Vec3 &count = Vec3()) const;

    // What the surface is made of. Applies to everything below it in
    // the tree that has not said otherwise.
    Sdf with_material(MaterialId m) const;

    // --- reading ------------------------------------------------------
    Sample sample(const Vec3 &p) const override;
    Vec3 gradient(const Vec3 &p, float h) const override;
    MaterialId surface_material(const Vec3 &p, const Vec3 &normal) const override;
    float bound(const AABB &box) const override;
    const char *name() const override { return "sdf"; }

    float distance(const Vec3 &p) const { return sample(p).distance; }
    // A box the shape fits inside. Conservative, and infinite in any
    // direction the shape is unbounded in -- check `bounded()` before
    // asking a mesher to fill it.
    AABB bounds() const;
    bool bounded() const;
    // How many nodes: what an agent gets told when it builds
    // something enormous by accident.
    size_t node_count() const;

    // --- meshing ------------------------------------------------------
    struct MeshOptions {
        MeshOptions() {}
        // Metres per cell, or 0 to pick one from the bounds so that
        // the longest axis gets `target_cells`.
        float cell_size = 0.0f;
        int target_cells = 48;
        // Hard limit, because a cell size and a bounding box together
        // are an easy way to ask for forty gigabytes. Exceeding it
        // coarsens the cell rather than failing, and says so.
        int max_cells_per_axis = 192;
        bool smooth_normals = false;
        // Collapse vertices that ended up in the same place.
        bool weld = true;
        // HOW FAR THE SURFACE MAY MOVE, as a fraction of the cell.
        //
        // A contourer emits one quad per surface cell whether the
        // surface is curved there or not, so a crate comes out with
        // sixty thousand triangles and needs a few hundred.
        // Decimation puts that back, and driving it by error rather
        // than by a triangle count is what makes one setting work
        // on a crate and on a boulder: the flat faces collapse for
        // nothing while the creases cost real distance and survive.
        //
        // Half a cell is invisible by construction -- it is smaller
        // than the contourer's own sampling error. 0 keeps every
        // triangle.
        float simplify_error = 0.5f;
        // A floor, so a small shape is not decimated to a
        // tetrahedron by a generous error budget.
        int min_triangles = 200;
        // Sharper than this stays a hard edge.
        float smooth_angle_degrees = 40.0f;
    };
    // Null when the shape is empty or unbounded, with the reason in
    // `error` -- both are things a caller will do and neither is a
    // crash.
    Ref<Mesh> to_mesh(const MeshOptions &options = {},
                      std::string *error = nullptr) const;

    // --- as data --------------------------------------------------------
    //
    // THE POINT OF THE JSON.
    //
    // A shape built by thirty method calls is thirty round trips for
    // anything driving this engine from outside. A shape written as
    // one JSON object is one. It is also diffable, storable and
    // printable, which a tree of shared pointers is not.
    Json to_json() const;
    static Sdf from_json(const Json &j, std::string *error);

    explicit Sdf(SdfRef root) : root_(std::move(root)) {}
    const SdfRef &root() const { return root_; }

private:
    SdfRef root_;
};

}  // namespace wr::gen
