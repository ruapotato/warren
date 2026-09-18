// Warren voxel -- what the ground is.
//
// The terrain generator and the record of what has been dug out of
// it. The field interface both of these implement lives in the
// engine, in procgen/field.h, because the procedural modelling tools
// contour the same kind of field with the same mesher.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "procgen/field.h"
#include "procgen/noise.h"

namespace wr::voxel {

// The field vocabulary is the engine's; the terrain is this
// plugin's use of it.
using gen::Field;
using gen::MaterialId;
using gen::Sample;
using gen::fbm;
using gen::perlin;
using gen::ridged;
using gen::worley;

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

class TerrainDensity : public Field {
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
class FlatDensity : public Field {
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

class EditedDensity : public Field {
public:
    explicit EditedDensity(Field *base) : base_(base) {}

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
    Field *base() const { return base_; }

private:
    Field *base_ = nullptr;
    std::vector<Edit> edits_;
    std::vector<AABB> bounds_;
};

}  // namespace wr::voxel
