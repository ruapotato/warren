// Warren -- geometry.
#pragma once

#include <string>
#include <vector>

#include "core/object.h"
#include "resource/resource.h"
#include "rhi/rhi.h"

namespace wr {

// THE ONE VERTEX FORMAT.
//
// Fifty-two bytes, and every mesh in the engine uses it. A format per
// mesh would save memory on the simple ones and cost a pipeline per
// variant, a branch in every shader and a permutation explosion in the
// material system; one format costs a few bytes on a cube and nothing
// else, ever. Skinned meshes add a second stream rather than a second
// format.
struct Vertex {
    Vec3 position;
    Vec3 normal;
    // w carries the bitangent's handedness, which is how a mirrored UV
    // island keeps its normal map the right way round.
    Vec4 tangent{1, 0, 0, 1};
    Vec2 uv;
    // Unsigned byte, normalised. Vertex colour is a tint and an ambient
    // occlusion channel, and eight bits is more than either needs.
    uint8_t colour[4] = {255, 255, 255, 255};
};
static_assert(sizeof(Vertex) == 52, "the vertex format is part of the ABI");

rhi::VertexLayout standard_vertex_layout();

// THE SAME LAYOUT PLUS A SECOND STREAM.
//
// Joints and weights live in their own buffer rather than in Vertex,
// because most of the world is static: putting eight bytes of skin on
// every vertex of a town costs that town a fifth of its memory and
// its vertex fetch bandwidth for eight bytes of zero. A skinned mesh
// binds two buffers; a static one binds one and uses a pipeline that
// does not know the second exists.
rhi::VertexLayout skinned_vertex_layout();

// A second stream, present only on skinned meshes.
struct SkinVertex {
    uint8_t joints[4] = {0, 0, 0, 0};
    uint8_t weights[4] = {255, 0, 0, 0};
};

// One draw's worth of a mesh: a range of indices and the material slot
// it wants. A mesh with three materials is one buffer and three of
// these, which is one upload and three draws rather than three of both.
struct SubMesh {
    uint32_t first_index = 0;
    uint32_t index_count = 0;
    int32_t material_slot = 0;
    AABB bounds;
    std::string name;
};

// THE SAME WORK, WITHOUT A Mesh TO HANG IT ON.
//
// A streaming system generates vertices on a worker thread and has no
// reason to wait for the main thread to derive tangents from them --
// but Mesh is an Object, and making one per chunk off-thread to call
// a method is the tail wagging the dog. These take the arrays.
void compute_tangents(std::vector<Vertex> &vertices,
                      const std::vector<uint32_t> &indices);
AABB compute_bounds(const std::vector<Vertex> &vertices);

class Mesh : public Resource {
    WR_CLASS(Mesh, Resource)

public:
    Mesh() = default;
    ~Mesh() override;

    // --- building -------------------------------------------------------
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<SkinVertex> skin;
    std::vector<SubMesh> submeshes;

    void clear();
    // Append geometry, offsetting its indices. Returns the submesh made.
    int append(const std::vector<Vertex> &v, const std::vector<uint32_t> &i,
               int material_slot = 0, const std::string &name = "");
    void transform(const Transform3D &t);

    // Recompute from the triangles. `smooth_angle` in radians: edges
    // sharper than this keep their hard normal.
    void compute_normals(float smooth_angle = deg2rad(60.0f));
    // Needs UVs and normals. Meshes without tangents get a black normal
    // map's worth of nothing, which looks like flat lighting and is
    // very hard to diagnose, so this runs by default on import.
    void compute_tangents();
    void compute_bounds();
    AABB bounds() const { return bounds_; }
    // For a mesh whose bounds were worked out elsewhere -- a streamed
    // chunk, say, whose mesher already knew them. Also fixes up the
    // default submesh, since a mesh with no bounds on its submeshes
    // is culled away entirely.
    void set_bounds(const AABB &b) {
        bounds_ = b;
        for (SubMesh &sm : submeshes) sm.bounds = b;
    }

    // Weld vertices closer than `epsilon` that also agree on normal and
    // UV. Returns how many were removed.
    size_t weld(float epsilon = 1e-5f);
    // Reverse every triangle. For geometry authored with the other
    // winding convention.
    void flip_winding();

    // FEWER TRIANGLES, IN THE PLACES THEY ARE NOT DOING ANYTHING.
    //
    // Contoured geometry has no idea which of its triangles matter.
    // A crate cut out of a signed distance field comes out with
    // thirty-six thousand of them, most lying flat on a face that
    // two would have described, because the contourer emits one
    // quad per cell whether the surface is curved there or not. An
    // importer has the same problem with somebody else's export.
    //
    // Quadric error metrics: each vertex accumulates the squared
    // distance to the planes of the faces around it, an edge
    // collapse costs the error at the point it collapses to, and
    // the cheapest collapse is taken until the target is reached.
    // Flat regions cost nothing to collapse and go first; a
    // silhouette or a crease costs a great deal and survives.
    //
    // Two limits, whichever stops it first. `ratio` is the fraction
    // of triangles to keep; `max_error` is how far, in metres, a
    // surface may move. Returns how many triangles are left.
    //
    // The error limit is the more useful of the two, and the reason
    // the quadrics are normalised by area: a flat face collapses for
    // nothing and vanishes entirely, while a crease costs real
    // distance and survives. One setting then works on a crate and
    // on a boulder, where a fixed ratio would over-simplify one and
    // under-simplify the other.
    //
    // Collapses that would flip a triangle over, or pull a vertex
    // off an open boundary, are refused, so a closed mesh stays
    // closed and nothing turns inside out.
    size_t simplify(float ratio, float max_error = 1e30f);

    // --- uploading --------------------------------------------------------
    // Idempotent; call again after changing the arrays to re-upload.
    bool upload(rhi::Device *dev, const char *name = nullptr);
    void release(rhi::Device *dev);
    bool uploaded() const { return vertex_buffer_.valid(); }
    rhi::BufferH vertex_buffer() const { return vertex_buffer_; }
    rhi::BufferH index_buffer() const { return index_buffer_; }
    rhi::BufferH skin_buffer() const { return skin_buffer_; }
    uint32_t index_count() const { return uint32_t(indices.size()); }
    uint32_t vertex_count() const { return uint32_t(vertices.size()); }
    uint32_t triangle_count() const { return uint32_t(indices.size() / 3); }
    // A mesh with a skin stream as long as its vertices is one the
    // skinned pipeline can draw; a partly-filled stream is a mesh
    // that was assembled from both kinds and is not.
    bool skinned() const {
        return !skin.empty() && skin.size() == vertices.size();
    }

    // --- primitives ---------------------------------------------------------
    //
    // Enough to build a level out of before there is an importer, and
    // enough that a plugin can hand back geometry without carrying its
    // own maths. All are counter-clockwise, all have correct normals,
    // UVs and tangents, and all are centred on the origin.
    static Ref<Mesh> box(const Vec3 &size = Vec3::one());
    static Ref<Mesh> plane(const Vec2 &size = Vec2(1, 1), int subdivisions = 1);
    static Ref<Mesh> sphere(float radius = 0.5f, int rings = 24, int segments = 32);
    static Ref<Mesh> cylinder(float radius = 0.5f, float height = 1.0f,
                              int segments = 24, bool capped = true);
    static Ref<Mesh> cone(float radius = 0.5f, float height = 1.0f,
                          int segments = 24);
    static Ref<Mesh> torus(float major = 0.5f, float minor = 0.15f,
                           int major_segments = 32, int minor_segments = 16);
    static Ref<Mesh> capsule(float radius = 0.25f, float height = 1.0f,
                             int rings = 8, int segments = 16);
    // A quad in the XY plane facing +Z, which is the portal's own shape
    // and the shape of every screen-space pass.
    static Ref<Mesh> quad(const Vec2 &size = Vec2(1, 1));
    // Axis lines and a wire box, for gizmos and debug drawing.
    static Ref<Mesh> wire_box(const AABB &box);

private:
    AABB bounds_;
    rhi::BufferH vertex_buffer_;
    rhi::BufferH index_buffer_;
    rhi::BufferH skin_buffer_;
    rhi::Device *owner_ = nullptr;
};

}  // namespace wr
