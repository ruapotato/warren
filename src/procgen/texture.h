// Warren -- surfaces described as formulas.
//
// The companion to sdf.h. That one says what shape a thing is; this
// one says what it looks like, and for the same reason: an asset an
// agent can write is an asset an agent can make, and an engine whose
// props all come out grey is an engine nobody will believe in.
//
// ONE SCALAR FIELD, COLOURED AT THE END.
//
// Every pattern here -- noise, bricks, checkers, gradients, cells --
// produces a single number between 0 and 1 across the surface.
// Patterns combine as numbers, which is a handful of arithmetic
// rather than a blend mode per channel per operation. Only at the
// last step does a colour ramp turn the number into pixels, and the
// SAME number turns into the roughness and, through its slope, the
// normal map. So one description gives a whole material that agrees
// with itself: the mortar is rougher than the brick because it is
// darker, without anybody saying so twice.
//
// TILEABLE BY CONSTRUCTION. Every pattern is periodic over the unit
// square, because a texture that does not tile is a texture that
// cannot go on a wall. The noise wraps its lattice rather than
// blending its edges, so there is no seam and no loss of detail.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/math/vector.h"

namespace wr {
class Material;
class Texture;
template <class T>
class Ref;
namespace rhi {
class Device;
}
}  // namespace wr

namespace wr::gen {

// An image on the CPU, before it is anything else. RGBA, eight bits
// a channel, sRGB-encoded for colour and linear for data -- which of
// those it is depends on what made it, and the two are never mixed
// in one image.
struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;

    Image() = default;
    Image(uint32_t w, uint32_t h) : width(w), height(h), pixels(size_t(w) * h * 4, 0) {}
    bool empty() const { return pixels.empty(); }
    uint8_t *at(uint32_t x, uint32_t y) {
        return &pixels[(size_t(y) * width + x) * 4];
    }
    const uint8_t *at(uint32_t x, uint32_t y) const {
        return &pixels[(size_t(y) * width + x) * 4];
    }
    // Wrapping, because everything here tiles.
    const uint8_t *wrapped(int32_t x, int32_t y) const {
        const uint32_t wx = uint32_t(((x % int32_t(width)) + int32_t(width)) % int32_t(width));
        const uint32_t wy = uint32_t(((y % int32_t(height)) + int32_t(height)) % int32_t(height));
        return at(wx, wy);
    }
    bool write_png(const std::string &path) const;
};

// The scalar field a pattern spec describes, sampled at a point in
// the unit square. Outside [0,1] it wraps.
float sample_pattern(const Json &spec, float u, float v, std::string *error);

// A pattern rendered as a greyscale height field, which is what
// everything else here is derived from.
Image render_height(const Json &spec, uint32_t size, std::string *error);

// Slope of a height field, encoded the way a shader expects: x and y
// in red and green about 0.5, z in blue. `strength` scales how deep
// the bumps read.
Image normal_from_height(const Image &height, float strength = 1.0f);

// EVERYTHING A SURFACE NEEDS, from one description.
//
// The height field drives all three: the ramp colours it, its slope
// becomes the normal map, and its value drives roughness between the
// two given limits. Occlusion is the field's low ground, which is
// crude and free and better than nothing.
struct SurfaceImages {
    Image albedo;     // sRGB
    Image normal;     // linear
    Image orm;        // linear: occlusion, roughness, metallic
    Image height;     // the field itself, greyscale
};
SurfaceImages render_surface(const Json &spec, uint32_t size, std::string *error);

// And the same thing as a Material ready to draw with. Null on a bad
// spec, with the reason in `error`.
Ref<Material> make_material(rhi::Device *device, const Json &spec,
                            uint32_t size = 256, std::string *error = nullptr);

// Every pattern, operation and field name, for whatever has to be
// told what the language is.
Json texture_grammar();

}  // namespace wr::gen
