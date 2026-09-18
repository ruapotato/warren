// Warren voxel -- what the ground is.
//
// The terrain is a SIGNED DISTANCE FIELD: a function from a point in
// space to how far it is from the surface, negative inside the rock
// and positive in the air. Everything else follows from that -- the
// mesher finds where it crosses zero, digging subtracts a sphere from
// it, and a cave is a region where it went positive.
//
// A field rather than a grid of blocks, because a field can be
// sampled at any resolution and always agrees with itself. That is
// what makes distant terrain meshable coarsely without the ground
// moving when you walk up to it.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "core/math/transform.h"

namespace wr::voxel {

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

// The source of the world. Implement this to make your own planet;
// the default below makes a good one.
class DensitySource {
public:
    virtual ~DensitySource() = default;
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
    // A conservative bound on |distance| over a box, used to skip
    // whole chunks without sampling them. Returning infinity is
    // always correct and always slow.
    virtual float bound(const AABB &box) const { return INF; }
    virtual const char *name() const { return "density"; }
};

// THE ONE THAT SHIPS.
//
// Rolling ground from fractal noise, mountains from ridged noise
// where a low-frequency mask says there should be, overhangs from a
// 3D warp, and caves carved by a second field. Materials by depth and
// slope: grass on flat ground near the surface, then dirt, then rock.
//
// Every parameter is exposed because "good terrain out of the box"
// means the defaults look right AND the knobs are reachable.
struct TerrainParams {
    uint32_t seed = 1337;
    // Metres. The height the ground sits at with no noise.
    float sea_level = 0.0f;
    // Broad shape.
    float continent_scale = 900.0f;
    float continent_height = 90.0f;
    // Hills on top of that.
    float hill_scale = 110.0f;
    float hill_height = 14.0f;
    int hill_octaves = 5;
    // Mountains, where the mask allows.
    float mountain_scale = 600.0f;
    float mountain_height = 220.0f;
    float mountain_mask_scale = 1800.0f;
    // Above this the mask starts letting mountains through.
    float mountain_threshold = 0.25f;
    // A 3D warp, which is what makes overhangs and cliffs rather than
    // a height field with texture.
    float warp_scale = 240.0f;
    float warp_strength = 26.0f;
    // Caves: a second field, thresholded.
    bool caves = true;
    float cave_scale = 70.0f;
    float cave_threshold = 0.42f;
    // No caves above this, so the surface is not swiss cheese.
    float cave_ceiling = -6.0f;
    // Materials.
    MaterialId material_grass = 1;
    MaterialId material_dirt = 2;
    MaterialId material_rock = 3;
    MaterialId material_sand = 4;
    MaterialId material_snow = 5;
    float dirt_depth = 3.0f;
    float snow_height = 140.0f;
    float sand_band = 3.0f;
    // Above this slope (the cosine of the angle from vertical) bare
    // rock shows through. 0.62 is about 52 degrees.
    float rock_slope = 0.62f;
    // How much the boundaries wander, so they are not contour lines.
    float material_noise = 12.0f;
};

class TerrainDensity : public DensitySource {
public:
    explicit TerrainDensity(const TerrainParams &p = {}) : params_(p) {}

    Sample sample(const Vec3 &p) const override;
    Vec3 gradient(const Vec3 &p, float h) const override;
    MaterialId surface_material(const Vec3 &p, const Vec3 &normal) const override;
    float bound(const AABB &box) const override;
    const char *name() const override { return "terrain"; }

    TerrainParams &params() { return params_; }
    const TerrainParams &params() const { return params_; }

    // Just the height of the ground under a column, ignoring caves
    // and overhangs. For placing things on the surface.
    float surface_height(float x, float z) const;

private:
    float ground_height(float x, float z) const;
    TerrainParams params_;
};

// A flat world, for tests and for a sandbox.
class FlatDensity : public DensitySource {
public:
    explicit FlatDensity(float height = 0.0f, MaterialId m = 3)
        : height_(height), material_(m) {}
    Sample sample(const Vec3 &p) const override {
        return {p.y - height_, p.y <= height_ ? material_ : MaterialId(0)};
    }
    MaterialId surface_material(const Vec3 &, const Vec3 &) const override {
        return material_;
    }
    Vec3 gradient(const Vec3 &, float) const override { return Vec3::up(); }
    float bound(const AABB &box) const override {
        return std::min(std::fabs(box.min.y - height_),
                        std::fabs(box.max.y - height_));
    }
    const char *name() const override { return "flat"; }

private:
    float height_;
    MaterialId material_;
};

// EDITS, LAID OVER WHATEVER IS UNDERNEATH.
//
// Digging does not rewrite the generator; it records what was removed
// and what was added, and the field is the generator combined with
// the record. That way the world is still a pure function of its seed
// plus a short list, which is what makes it saveable in a few
// kilobytes and identical on every machine that replays the list.
struct Edit {
    enum class Shape : uint8_t { Sphere, Box };
    enum class Op : uint8_t { Subtract, Add, Paint };
    Shape shape = Shape::Sphere;
    Op op = Op::Subtract;
    Vec3 centre;
    float radius = 1.0f;
    Vec3 half_extents{1, 1, 1};
    MaterialId material = 0;
    // Rounds the corners of a box edit and softens a sphere's rim.
    float smooth = 0.5f;

    AABB bounds() const;
    // The edit's own field at a point: negative inside it.
    float field(const Vec3 &p) const;
};

class EditedDensity : public DensitySource {
public:
    explicit EditedDensity(DensitySource *base) : base_(base) {}

    Sample sample(const Vec3 &p) const override;
    Vec3 gradient(const Vec3 &p, float h) const override;
    MaterialId surface_material(const Vec3 &p, const Vec3 &normal) const override;
    float bound(const AABB &box) const override;
    const char *name() const override { return "edited"; }

    void add_edit(const Edit &e);
    void clear_edits();
    size_t edit_count() const { return edits_.size(); }
    const std::vector<Edit> &edits() const { return edits_; }
    // Which edits touch a box, so a chunk only pays for its own.
    void edits_in(const AABB &box, std::vector<const Edit *> &out) const;
    DensitySource *base() const { return base_; }

private:
    DensitySource *base_ = nullptr;
    std::vector<Edit> edits_;
    std::vector<AABB> bounds_;
};

}  // namespace wr::voxel
