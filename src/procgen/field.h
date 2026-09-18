// Warren -- a field, and what reads one.
//
// A SIGNED DISTANCE FIELD is a function from a point in space to how
// far it is from a surface: negative inside, positive outside. It is
// the engine's one representation of "a shape that is not yet
// triangles", and two very different things are built on it.
//
// The voxel terrain is one: a planet is a field, digging subtracts a
// sphere from it, and a cave is a region that went positive. A field
// can be sampled at any resolution and always agrees with itself,
// which is what lets distant ground be meshed coarsely without it
// changing shape as you walk towards it.
//
// PROCEDURAL MODELLING is the other, and it is the same interface.
// A shape described as a formula -- a box minus four cylinders,
// smoothly blended, then twisted -- is a field, and contouring it
// gives a mesh. Booleans on fields are min and max, which always
// work; booleans on triangle meshes need exact predicates and still
// fail on coplanar faces. That is why this engine's CSG is here and
// not in a mesh library.
#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include "core/math/transform.h"

namespace wr::gen {

// What a cell is made of: an index into the terrain's palette, with 0
// always air. NOT called Material, because the engine already has a
// class of that name and a voxel is not made of one -- it refers to
// one, which is what an id is for.
using MaterialId = uint8_t;

struct Sample {
    // Negative inside the solid, positive outside, in metres. It need
    // not be a true distance -- the mesher only needs the sign and a
    // roughly linear crossing -- but the closer it is, the better the
    // surface looks.
    float distance = 1.0f;
    MaterialId material = 0;
};

// ANYTHING THAT HAS A SHAPE, before it has triangles.
//
// Implement this to make your own planet, or use the Sdf tree in
// sdf.h to build one out of primitives and booleans. Either way the
// contouring in surface.h turns it into a mesh.
class Field {
public:
    virtual ~Field() = default;
    virtual Sample sample(const Vec3 &p) const = 0;
    // THE GRADIENT, WHICH IS THE SURFACE NORMAL.
    //
    // The default takes six extra samples; a generator that knows its
    // own derivative should say so, because this is called once per
    // surface crossing and there are a great many of them.
    virtual Vec3 gradient(const Vec3 &p, float h = 0.05f) const {
        return {(sample({p.x + h, p.y, p.z}).distance -
                 sample({p.x - h, p.y, p.z}).distance),
                (sample({p.x, p.y + h, p.z}).distance -
                 sample({p.x, p.y - h, p.z}).distance),
                (sample({p.x, p.y, p.z + h}).distance -
                 sample({p.x, p.y, p.z - h}).distance)};
    }
    // WHAT THE SURFACE IS MADE OF, given where it is and which way
    // it faces.
    //
    // Distinct from `sample().material`, which says what is at a
    // point. At the surface itself the depth is zero, so a
    // depth-banded rule gives the same answer everywhere; and asking
    // a corner of the cell instead makes the answer flip between
    // neighbouring cells wherever a band boundary passes through,
    // which paints contour stripes across the whole landscape.
    //
    // The slope is what a surface material actually depends on:
    // grass on the flat, rock on the steep, snow up high.
    virtual MaterialId surface_material(const Vec3 &p, const Vec3 &normal) const {
        return sample(p).material;
    }
    // A conservative bound on |distance| over a box: no point in the
    // box is closer to the surface than this. Used to skip whole
    // regions without sampling them. Returning zero is always
    // correct and always slow, which is the default so that a field
    // written in five minutes is right before it is fast.
    virtual float bound(const AABB &box) const { return 0.0f; }
    virtual const char *name() const { return "density"; }
};

}  // namespace wr::gen
