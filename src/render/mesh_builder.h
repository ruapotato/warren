// Warren -- putting geometry together, from a script.
//
// The primitives on Mesh say they are "enough to build a level out of
// before there is an importer", and they are. They are also static
// functions, which the reflection cannot reach, so a game written in
// Python could not call one of them -- and even from C++ they hand
// back a mesh each, where a town wants one mesh with a few materials
// rather than four thousand nodes with one box apiece.
//
// A builder fixes both. It accumulates into a single mesh, keeps one
// submesh per material slot so the whole town is one upload and a
// handful of draws, and applies a transform and a colour to whatever
// is added next. Nothing here is new geometry; it is the primitives
// that already existed, reachable and composable.
//
// WHY UVs ARE IN METRES. A box primitive's UVs run 0..1 per face,
// which is right for a crate and wrong for a wall: the same material
// on a 0.6 m cupboard and a 20 m frontage reads at two completely
// different sizes and the level looks like a collage. Scaling the
// generated UVs by the face's size in metres makes one material one
// size everywhere, which is what a procedural level needs and what
// nobody wants to compute by hand at every call site.
#pragma once

#include <string>
#include <vector>

#include "core/object.h"
#include "render/mesh.h"

namespace wr {

class MeshBuilder : public Object {
    WR_CLASS(MeshBuilder, Object)

public:
    MeshBuilder();

    // ------------------------------------------------------ state
    //
    // Applied to everything added after it, so a caller sets a
    // colour and a slot once and then adds twenty walls.

    void set_colour(const Color &c) { colour_ = c; }
    Color get_colour() const { return colour_; }
    // The material slot, which becomes a submesh. Slots need not be
    // contiguous and need not be added in order.
    void set_slot(int slot) { slot_ = slot; }
    int get_slot() const { return slot_; }
    void set_transform(const Transform3D &t) { xform_ = t; }
    Transform3D get_transform() const { return xform_; }
    // Texture repeats per metre. Zero keeps each primitive's own
    // 0..1 UVs, for the cases where that is what was wanted.
    void set_uv_scale(float s) { uv_scale_ = s; }
    float get_uv_scale() const { return uv_scale_; }

    // -------------------------------------------------- primitives
    //
    // Positions are in the current transform's space.

    void add_box(const Vec3 &centre, const Vec3 &size);
    // The same, given opposite corners -- which is how a floor plan
    // is usually written down.
    void add_bounds(const AABB &b);
    void add_plane(const Vec3 &centre, const Vec2 &size, int subdivisions);
    void add_sphere(const Vec3 &centre, float radius, int rings, int segments);
    void add_cylinder(const Vec3 &centre, float radius, float height,
                      int segments);
    // The same, described by where it starts and where it ends,
    // which is how a limb, a pipe or a handrail is described.
    void add_cylinder_between(const Vec3 &from, const Vec3 &to, float radius,
                              int segments);
    void add_cone(const Vec3 &centre, float radius, float height, int segments);
    void add_quad(const Vec3 &a, const Vec3 &b, const Vec3 &c, const Vec3 &d);
    // Another mesh, through the current transform. Its own material
    // slots are ignored: everything lands in the builder's slot,
    // because a builder is for making one thing out of many.
    void add_mesh(Mesh *m);

    // ------------------------------------------- rooms and streets
    //
    // A town is mostly these, and writing them out of boxes by hand
    // is where the arithmetic errors live -- a wall placed at its
    // base and then given a height that also starts at its base is
    // how a building ends up a storey off the ground.

    // A wall standing ON the ground between two points. `from` and
    // `to` are the ENDS OF ITS BASE, not two of its corners, so a
    // wall never floats and never sinks.
    void add_wall(const Vec3 &from, const Vec3 &to, float height,
                  float thickness);
    // Four walls round a floor plan and the floor under them, with
    // the walls standing on the floor's top face. `floor` is the
    // room's inside measurement; its y extent is how thick the slab
    // is, and a flat plan gets a slab as thick as the walls.
    void add_room(const AABB &floor, float height, float thickness);
    // A flight rising along +x or +z within `bounds`. Each tread is
    // a box from the ground up, so the result is solid rather than
    // floating, which matters to the navigation bake -- it decides
    // what can be climbed by looking at the top of the solid below.
    void add_stairs(const AABB &bounds, int steps, bool along_x);

    // ------------------------------------------------------ output
    //
    // Finishes the mesh -- normals, tangents, bounds -- and hands it
    // over. The builder keeps a reference until it is cleared or
    // built again, so the result is safe to assign somewhere.
    Mesh *build();
    // Start a new, empty mesh. The one built before this is left
    // alone; whoever took it still has it.
    void clear();

    int vertex_count() const;
    int triangle_count() const;
    // Every slot used so far, in the order they will appear as
    // submeshes -- which is what a caller needs to know to put the
    // right material in the right place.
    Array get_slots() const;

private:
    struct Part {
        int slot = 0;
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
    };
    Part &part_for(int slot);
    // Appends a primitive's geometry through the current transform,
    // tinting it and rescaling its UVs. `uv_size` is the primitive's
    // extent in the two directions its UVs run, for the metre
    // scaling; zero means leave the UVs alone.
    void emit(const Mesh &src, const Vec2 &uv_size);

    std::vector<Part> parts_;
    Ref<Mesh> built_;
    Transform3D xform_;
    Color colour_ = Color::white();
    float uv_scale_ = 1.0f;
    int slot_ = 0;
};

}  // namespace wr
