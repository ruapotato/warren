#include "procgen/texture.h"

#include <algorithm>
#include <cmath>

#include "stb/stb_image_write.h"

#include "core/log.h"
#include "core/suggest.h"
#include "procgen/noise.h"
#include "render/material.h"
#include "render/texture.h"

namespace wr::gen {

bool Image::write_png(const std::string &path) const {
    if (empty()) return false;
    return stbi_write_png(path.c_str(), int(width), int(height), 4, pixels.data(),
                          int(width) * 4) != 0;
}

namespace {

float fract(float x) { return x - std::floor(x); }

float number_or(const Json &j, const char *key, float fallback) {
    const Json &v = j[key];
    return v.type() == Json::Type::Number ? float(v.number()) : fallback;
}

int int_or(const Json &j, const char *key, int fallback) {
    const Json &v = j[key];
    return v.type() == Json::Type::Number ? int(v.number()) : fallback;
}

float smoothstep_between(float edge0, float edge1, float x) {
    if (edge1 <= edge0) return x < edge0 ? 0.0f : 1.0f;
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// ------------------------------------------------------------- patterns

const char *const kPatterns[] = {"noise", "ridged",   "cells",  "checker",
                                 "bricks", "stripes", "gradient", "radial",
                                 "dots",   "constant", "wave"};

float pattern_value(const Json &j, const std::string &kind, float u, float v,
                    std::string *error);

// Bricks are the one pattern worth doing properly, because they are
// the one everybody reaches for first and the one where a lazy
// implementation shows: the rows have to stagger, the mortar has to
// be a groove rather than a line, and each brick wants its own shade
// or the wall reads as wallpaper.
float bricks(const Json &j, float u, float v) {
    const float rows = std::max(number_or(j, "rows", 8.0f), 1.0f);
    const float columns = std::max(number_or(j, "columns", 4.0f), 1.0f);
    const float mortar = std::clamp(number_or(j, "mortar", 0.04f), 0.0f, 0.4f);
    const float stagger = number_or(j, "stagger", 0.5f);
    const float variation = std::clamp(number_or(j, "variation", 0.25f), 0.0f, 1.0f);
    const uint32_t seed = uint32_t(int_or(j, "seed", 1));

    // WRAPPED ROW AND COLUMN INDICES.
    //
    // Everything else about this pattern tiles for free, and the
    // per-brick shade did not: brick 4 of one tile and brick 0 of
    // the next are the same brick, so they have to hash the same.
    // The stagger has to come from the wrapped row for the same
    // reason, or the courses step sideways at every tile boundary.
    const int32_t nrows = std::max(int32_t(rows), 1);
    const int32_t ncols = std::max(int32_t(columns), 1);
    auto wrap_index = [](int32_t i, int32_t n) {
        const int32_t m = i % n;
        return m < 0 ? m + n : m;
    };

    const float row = v * rows;
    const int32_t ri = wrap_index(int32_t(std::floor(row)), nrows);
    // Every other row slides along, which is what makes it a wall
    // and not a grid.
    const float shifted = u + float(ri) * stagger / columns;
    const float column = shifted * columns;
    const int32_t ci = wrap_index(int32_t(std::floor(column)), ncols);

    const float fy = fract(row), fx = fract(column);
    // Distance to the nearest edge of this brick, in brick units,
    // then a smooth step across the mortar so the groove has a
    // shoulder rather than an aliased edge.
    const float edge_x = std::min(fx, 1.0f - fx);
    const float edge_y = std::min(fy, 1.0f - fy);
    const float half = mortar * 0.5f;
    const float bx = smoothstep_between(half * 0.5f, half * 1.5f, edge_x * (1.0f / columns) * columns);
    const float by = smoothstep_between(half * 0.5f, half * 1.5f, edge_y);
    const float brick = std::min(bx, by);

    // A shade per brick, from its own coordinates, so the same brick
    // is the same colour on every tile.
    const float shade =
        hash_float(ci, ri, 0, seed) * variation + (1.0f - variation);
    return brick * shade;
}

float pattern_value(const Json &j, const std::string &kind, float u, float v,
                    std::string *error) {
    const float scale = std::max(number_or(j, "scale", 4.0f), 0.0001f);
    const uint32_t seed = uint32_t(int_or(j, "seed", 1));
    const int octaves = std::clamp(int_or(j, "octaves", 4), 1, 8);
    // The lattice period in cells, so the result repeats exactly
    // once over the unit square.
    const int period = std::max(int(std::lround(scale)), 1);
    const Vec3 p(u * float(period), v * float(period), 0.5f);

    if (kind == "noise") {
        return fbm_tiled(p, seed, octaves, period) * 0.5f + 0.5f;
    }
    if (kind == "ridged") {
        // 1 - |noise|, which turns rolling hills into creases: veins
        // in marble, cracks in mud, the grain in wood.
        const float n = fbm_tiled(p, seed, octaves, period);
        const float r = 1.0f - std::fabs(n) * 2.0f;
        return std::clamp(r, 0.0f, 1.0f);
    }
    if (kind == "cells") {
        const float d = worley_tiled(p, seed, period);
        return std::clamp(d, 0.0f, 1.0f);
    }
    if (kind == "checker") {
        const int n = std::max(int_or(j, "size", 8), 1);
        const int cx = int(std::floor(u * float(n)));
        const int cy = int(std::floor(v * float(n)));
        return ((cx + cy) & 1) ? 1.0f : 0.0f;
    }
    if (kind == "bricks") return bricks(j, u, v);
    if (kind == "stripes") {
        const float n = std::max(number_or(j, "count", 8.0f), 1.0f);
        const bool vertical = j["axis"].string() != "y";
        const float t = fract((vertical ? u : v) * n);
        const float width = std::clamp(number_or(j, "width", 0.5f), 0.01f, 0.99f);
        const float soft = std::max(number_or(j, "soft", 0.02f), 0.0001f);
        return smoothstep_between(width - soft, width + soft, t) > 0.5f ? 0.0f : 1.0f;
    }
    if (kind == "gradient") {
        const std::string axis = j["axis"].string();
        return std::clamp(axis == "x" ? u : axis == "diagonal" ? (u + v) * 0.5f : v,
                          0.0f, 1.0f);
    }
    if (kind == "radial") {
        const float dx = u - 0.5f, dy = v - 0.5f;
        const float r = std::sqrt(dx * dx + dy * dy) * 2.0f;
        return std::clamp(1.0f - r / std::max(number_or(j, "radius", 1.0f), 1e-4f),
                          0.0f, 1.0f);
    }
    if (kind == "dots") {
        const float n = std::max(number_or(j, "count", 6.0f), 1.0f);
        const float radius = std::clamp(number_or(j, "radius", 0.3f), 0.01f, 0.5f);
        const float fx = fract(u * n) - 0.5f, fy = fract(v * n) - 0.5f;
        const float d = std::sqrt(fx * fx + fy * fy);
        return 1.0f - smoothstep_between(radius - 0.03f, radius, d);
    }
    if (kind == "wave") {
        const float n = std::max(number_or(j, "count", 4.0f), 1.0f);
        const bool vertical = j["axis"].string() != "y";
        return std::sin((vertical ? u : v) * n * 6.283185307179586f) * 0.5f + 0.5f;
    }
    if (kind == "constant") return std::clamp(number_or(j, "value", 0.5f), 0.0f, 1.0f);

    std::vector<std::string> names(std::begin(kPatterns), std::end(kPatterns));
    const std::vector<std::string> near = suggest(kind, names, 3);
    *error = "no pattern called \"" + kind + "\" -- it should be one of ";
    for (size_t i = 0; i < names.size(); i++) {
        if (i) *error += i + 1 == names.size() ? " or " : ", ";
        *error += "\"" + names[i] + "\"";
    }
    if (!near.empty()) *error += "; did you mean \"" + near[0] + "\"?";
    return 0.0f;
}

// ----------------------------------------------------------- operations

float blend(const std::string &mode, float a, float b, float amount) {
    float mixed;
    if (mode == "multiply") mixed = a * b;
    else if (mode == "add") mixed = a + b;
    else if (mode == "subtract") mixed = a - b;
    else if (mode == "min" || mode == "darken") mixed = std::min(a, b);
    else if (mode == "max" || mode == "lighten") mixed = std::max(a, b);
    else if (mode == "screen") mixed = 1.0f - (1.0f - a) * (1.0f - b);
    else if (mode == "difference") mixed = std::fabs(a - b);
    else if (mode == "overlay")
        mixed = a < 0.5f ? 2.0f * a * b : 1.0f - 2.0f * (1.0f - a) * (1.0f - b);
    else mixed = b;  // "mix"
    return std::clamp(a + (mixed - a) * amount, 0.0f, 1.0f);
}

const char *const kBlendModes[] = {"mix",    "multiply", "add",    "subtract",
                                   "min",    "max",      "screen", "difference",
                                   "overlay"};

}  // namespace

float sample_pattern(const Json &spec, float u, float v, std::string *error) {
    std::string ignored;
    if (!error) error = &ignored;
    if (spec.type() != Json::Type::Object) {
        *error = "a pattern is a JSON object with \"pattern\" or \"op\"";
        return 0.0f;
    }

    // WARPED FIRST, if at all.
    //
    // Pushing the lookup around with another pattern is what turns
    // stripes into wood grain and noise into marble, and it is one
    // line. It has to happen before the pattern is sampled, not
    // after, which is why it is here and not with the other
    // modifiers below.
    float su = u, sv = v;
    if (spec.has("warp")) {
        const Json &w = spec["warp"];
        const float amount = number_or(spec, "warp_amount",
                                       w.type() == Json::Type::Number ? 0.0f : 0.15f);
        if (w.type() == Json::Type::Object) {
            const float dx = sample_pattern(w, u, v, error) - 0.5f;
            // Offset the second lookup so the two axes do not use the
            // same number, which would only slide the image along the
            // diagonal.
            const float dy = sample_pattern(w, u + 0.37f, v + 0.11f, error) - 0.5f;
            su += dx * amount;
            sv += dy * amount;
        }
    }

    float value = 0.0f;
    if (spec.has("op")) {
        const std::string op = spec["op"].string();
        const Json &of = spec["of"];
        if (op != "blend" && op != "layer") {
            *error = "no texture operation called \"" + op +
                     "\" -- it should be \"blend\"";
            return 0.0f;
        }
        if (of.type() != Json::Type::Array || of.size() == 0) {
            *error = "\"" + op + "\" needs \"of\": a list of patterns";
            return 0.0f;
        }
        const std::string mode = spec["mode"].type() == Json::Type::String
                                     ? spec["mode"].string()
                                     : "mix";
        bool known = false;
        for (const char *m : kBlendModes)
            if (mode == m) known = true;
        if (!known) {
            std::vector<std::string> names(std::begin(kBlendModes),
                                           std::end(kBlendModes));
            *error = "no blend mode called \"" + mode + "\"";
            const std::vector<std::string> near = suggest(mode, names, 3);
            if (!near.empty()) *error += " -- did you mean \"" + near[0] + "\"?";
            return 0.0f;
        }
        const float amount = number_or(spec, "amount", 1.0f);
        value = sample_pattern(of[0], su, sv, error);
        for (size_t i = 1; i < of.size() && error->empty(); i++)
            value = blend(mode, value, sample_pattern(of[i], su, sv, error), amount);
    } else if (spec.has("pattern")) {
        value = pattern_value(spec, spec["pattern"].string(), su, sv, error);
    } else {
        *error = "a pattern needs \"pattern\" (one of the built-in ones) or "
                 "\"op\" (a combination)";
        return 0.0f;
    }
    if (!error->empty()) return 0.0f;

    // --- shaping, in the order they read ---------------------------
    if (spec.has("invert") && spec["invert"].boolean()) value = 1.0f - value;
    if (spec.has("contrast")) {
        const float c = number_or(spec, "contrast", 1.0f);
        value = std::clamp((value - 0.5f) * c + 0.5f, 0.0f, 1.0f);
    }
    if (spec.has("brightness"))
        value = std::clamp(value + number_or(spec, "brightness", 0.0f), 0.0f, 1.0f);
    if (spec.has("power"))
        value = std::pow(std::max(value, 0.0f), std::max(number_or(spec, "power", 1.0f), 0.01f));
    if (spec.has("threshold")) {
        const float t = number_or(spec, "threshold", 0.5f);
        const float soft = std::max(number_or(spec, "soft", 0.01f), 0.0001f);
        value = smoothstep_between(t - soft, t + soft, value);
    }
    if (spec.has("range")) {
        // Remap into a narrower band, which is how a pattern becomes
        // a roughness or a subtle variation rather than full swing.
        const Json &r = spec["range"];
        if (r.type() == Json::Type::Array && r.size() == 2)
            value = float(r[0].number()) +
                    value * float(r[1].number() - r[0].number());
    }
    return std::clamp(value, 0.0f, 1.0f);
}

Image render_height(const Json &spec, uint32_t size, std::string *error) {
    std::string ignored;
    if (!error) error = &ignored;
    error->clear();
    size = std::clamp(size, 4u, 4096u);
    Image out(size, size);
    for (uint32_t y = 0; y < size && error->empty(); y++) {
        // Sampled at the pixel CENTRE. At the corner, the first and
        // last column would both sample u = 0 and u = 1, which are
        // the same point on a tiling texture -- so the tile would be
        // one pixel wider than it should be and the seam would show
        // as a doubled column.
        const float v = (float(y) + 0.5f) / float(size);
        for (uint32_t x = 0; x < size; x++) {
            const float u = (float(x) + 0.5f) / float(size);
            const float h = sample_pattern(spec, u, v, error);
            if (!error->empty()) break;
            const uint8_t g = uint8_t(std::clamp(h, 0.0f, 1.0f) * 255.0f + 0.5f);
            uint8_t *px = out.at(x, y);
            px[0] = px[1] = px[2] = g;
            px[3] = 255;
        }
    }
    if (!error->empty()) return Image();
    return out;
}

Image normal_from_height(const Image &height, float strength) {
    if (height.empty()) return Image();
    Image out(height.width, height.height);
    // Sobel, which is a little more work than a two-tap difference
    // and much less prone to the diagonal stair-stepping that shows
    // up on anything with a hard edge in it.
    for (uint32_t y = 0; y < height.height; y++) {
        for (uint32_t x = 0; x < height.width; x++) {
            auto h = [&](int dx, int dy) {
                return float(height.wrapped(int32_t(x) + dx, int32_t(y) + dy)[0]) /
                       255.0f;
            };
            const float gx = (h(1, -1) + 2.0f * h(1, 0) + h(1, 1)) -
                             (h(-1, -1) + 2.0f * h(-1, 0) + h(-1, 1));
            const float gy = (h(-1, 1) + 2.0f * h(0, 1) + h(1, 1)) -
                             (h(-1, -1) + 2.0f * h(0, -1) + h(1, -1));
            // Scaled by the resolution, so the same height field
            // gives the same apparent depth at any texture size.
            const float k = strength * float(height.width) / 256.0f;
            Vec3 n(-gx * k, -gy * k, 1.0f);
            n = n.normalized();
            uint8_t *px = out.at(x, y);
            px[0] = uint8_t((n.x * 0.5f + 0.5f) * 255.0f + 0.5f);
            px[1] = uint8_t((n.y * 0.5f + 0.5f) * 255.0f + 0.5f);
            px[2] = uint8_t((n.z * 0.5f + 0.5f) * 255.0f + 0.5f);
            px[3] = 255;
        }
    }
    return out;
}

namespace {

// A colour ramp: stops at positions, linearly interpolated. Written
// as [[0, "#332211"], [1, "#aa8866"]], or as a bare list of colours
// spread evenly, because that is what somebody writes when they do
// not care where the stops are.
struct Ramp {
    std::vector<std::pair<float, Color>> stops;

    Color at(float t) const {
        if (stops.empty()) return Color(t, t, t, 1);
        if (t <= stops.front().first) return stops.front().second;
        if (t >= stops.back().first) return stops.back().second;
        for (size_t i = 1; i < stops.size(); i++) {
            if (t > stops[i].first) continue;
            const float span = stops[i].first - stops[i - 1].first;
            const float k = span > 1e-6f ? (t - stops[i - 1].first) / span : 0.0f;
            const Color &a = stops[i - 1].second;
            const Color &b = stops[i].second;
            return Color(a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k,
                         a.b + (b.b - a.b) * k, a.a + (b.a - a.a) * k);
        }
        return stops.back().second;
    }
};

bool read_colour(const Json &j, Color *out) {
    if (j.type() == Json::Type::String) {
        const std::string &h = j.string();
        const char *c = h.c_str() + (h[0] == '#' ? 1 : 0);
        char *end = nullptr;
        const unsigned long bits = std::strtoul(c, &end, 16);
        if (size_t(end - c) != 6 || *end) return false;
        *out = Color::hex(uint32_t(bits));
        return true;
    }
    if (j.type() == Json::Type::Array && j.size() >= 3) {
        *out = Color(float(j[0].number()), float(j[1].number()),
                     float(j[2].number()),
                     j.size() > 3 ? float(j[3].number()) : 1.0f);
        return true;
    }
    return false;
}

bool read_ramp(const Json &j, Ramp *out, std::string *error) {
    if (j.type() != Json::Type::Array || j.size() == 0) {
        *error = "colours should be a list, either of colours or of "
                 "[position, colour] pairs";
        return false;
    }
    for (size_t i = 0; i < j.size(); i++) {
        const Json &e = j[i];
        Color c;
        if (e.type() == Json::Type::Array && e.size() == 2 &&
            e[0].type() == Json::Type::Number) {
            if (!read_colour(e[1], &c)) {
                *error = "colour " + std::to_string(i) + " is not a colour";
                return false;
            }
            out->stops.push_back({float(e[0].number()), c});
        } else {
            if (!read_colour(e, &c)) {
                *error = "colour " + std::to_string(i) + " is not a colour";
                return false;
            }
            const float t = j.size() == 1 ? 0.0f : float(i) / float(j.size() - 1);
            out->stops.push_back({t, c});
        }
    }
    std::sort(out->stops.begin(), out->stops.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    return true;
}

// Linear to sRGB, for writing a colour into an 8-bit albedo map.
uint8_t encode_srgb(float linear) {
    const float c = std::clamp(linear, 0.0f, 1.0f);
    const float s = c <= 0.0031308f ? c * 12.92f
                                    : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return uint8_t(s * 255.0f + 0.5f);
}

}  // namespace

SurfaceImages render_surface(const Json &spec, uint32_t size, std::string *error) {
    std::string ignored;
    if (!error) error = &ignored;
    error->clear();
    SurfaceImages out;

    out.height = render_height(spec, size, error);
    if (!error->empty()) return {};

    Ramp ramp;
    if (spec.has("colours") || spec.has("colors")) {
        if (!read_ramp(spec.has("colours") ? spec["colours"] : spec["colors"], &ramp,
                       error))
            return {};
    }

    const float bump = number_or(spec, "bump", 1.0f);
    out.normal = normal_from_height(out.height, bump);

    // Roughness from the same field. The default runs the other way
    // -- dark is rough -- because that is true of nearly everything:
    // mortar, rust, dirt and wear are all darker and rougher than
    // what they sit on.
    float rough_lo = 0.85f, rough_hi = 0.35f;
    if (spec.has("roughness")) {
        const Json &r = spec["roughness"];
        if (r.type() == Json::Type::Number) {
            rough_lo = rough_hi = float(r.number());
        } else if (r.type() == Json::Type::Array && r.size() == 2) {
            rough_lo = float(r[0].number());
            rough_hi = float(r[1].number());
        }
    }
    const float metallic = std::clamp(number_or(spec, "metallic", 0.0f), 0.0f, 1.0f);
    const float occlusion = std::clamp(number_or(spec, "occlusion", 0.6f), 0.0f, 1.0f);

    out.albedo = Image(size, size);
    out.orm = Image(size, size);
    for (uint32_t y = 0; y < size; y++) {
        for (uint32_t x = 0; x < size; x++) {
            const float h = float(out.height.at(x, y)[0]) / 255.0f;
            const Color c = ramp.stops.empty() ? Color(h, h, h, 1) : ramp.at(h);
            uint8_t *a = out.albedo.at(x, y);
            // The ramp's colours are linear (Color::hex decodes
            // them); an albedo map is sRGB, and the shader will
            // decode it again.
            a[0] = encode_srgb(c.r);
            a[1] = encode_srgb(c.g);
            a[2] = encode_srgb(c.b);
            a[3] = uint8_t(std::clamp(c.a, 0.0f, 1.0f) * 255.0f + 0.5f);

            uint8_t *o = out.orm.at(x, y);
            // Occlusion: the low ground of the field, shaded down by
            // however much was asked for.
            const float ao = 1.0f - (1.0f - h) * (1.0f - occlusion);
            o[0] = uint8_t(std::clamp(ao, 0.0f, 1.0f) * 255.0f + 0.5f);
            o[1] = uint8_t(std::clamp(rough_lo + (rough_hi - rough_lo) * h, 0.0f, 1.0f) *
                               255.0f + 0.5f);
            o[2] = uint8_t(metallic * 255.0f + 0.5f);
            o[3] = 255;
        }
    }
    return out;
}

Ref<Material> make_material(rhi::Device *device, const Json &spec, uint32_t size,
                            std::string *error) {
    std::string ignored;
    if (!error) error = &ignored;
    const SurfaceImages images = render_surface(spec, size, error);
    if (!error->empty()) return Ref<Material>();
    if (!device) {
        *error = "no render device: a material needs one to upload its maps to";
        return Ref<Material>();
    }

    Ref<Material> m(new Material());
    m->albedo_map = Texture::from_pixels(device, images.albedo.pixels.data(),
                                         images.albedo.width, images.albedo.height,
                                         rhi::Format::RGBA8_SRGB, true, "procedural albedo");
    m->normal_map = Texture::from_pixels(device, images.normal.pixels.data(),
                                         images.normal.width, images.normal.height,
                                         rhi::Format::RGBA8, true, "procedural normal");
    m->orm_map = Texture::from_pixels(device, images.orm.pixels.data(),
                                      images.orm.width, images.orm.height,
                                      rhi::Format::RGBA8, true, "procedural orm");
    m->roughness = 1.0f;   // the map carries it
    m->metallic = 1.0f;    // likewise
    m->normal_scale = number_or(spec, "bump", 1.0f);

    // TRIPLANAR BY DEFAULT, because of what this is for.
    //
    // A procedural material goes on procedural geometry, and
    // procedural geometry has no UVs anybody chose -- a contoured
    // shape gets a planar guess, which stretches badly down every
    // vertical face. Projecting from three directions needs no UVs
    // and gets the scale right everywhere. `tile` is then repeats
    // per metre rather than repeats per mesh, which is also the more
    // useful thing to be able to say.
    const float tile = number_or(spec, "tile", 1.0f);
    if (spec.has("uv") && spec["uv"].boolean()) {
        m->uv_scale = Vec2(tile, tile);
    } else {
        m->triplanar = tile;
        m->triplanar_sharpness = number_or(spec, "projection_sharpness", 4.0f);
    }
    return m;
}

Json texture_grammar() {
    struct Doc { const char *name; const char *params; const char *note; };
    static const Doc kPatternDocs[] = {
        {"noise", "scale=4, octaves=4, seed=1", "fractal Perlin; the general-purpose one"},
        {"ridged", "scale=4, octaves=4, seed=1", "creases rather than hills: veins, cracks, grain"},
        {"cells", "scale=4, seed=1", "Worley; scales, pebbles, cracked mud"},
        {"checker", "size=8", ""},
        {"bricks", "rows=8, columns=4, mortar=0.04, stagger=0.5, variation=0.25, seed=1",
         "staggered courses, a grooved mortar line and a shade per brick"},
        {"stripes", "count=8, width=0.5, soft=0.02, axis=x|y", ""},
        {"gradient", "axis=x|y|diagonal", ""},
        {"radial", "radius=1", "bright in the middle"},
        {"dots", "count=6, radius=0.3", ""},
        {"wave", "count=4, axis=x|y", "a sine, for corrugation and ripples"},
        {"constant", "value=0.5", ""},
    };
    static const Doc kOpDocs[] = {
        {"blend", "of=[...], mode=mix, amount=1",
         "mix, multiply, add, subtract, min, max, screen, difference, overlay"},
    };
    static const Doc kShapeDocs[] = {
        {"warp", "a pattern, plus warp_amount=0.15",
         "push the lookup around with another pattern -- stripes become wood, "
         "noise becomes marble"},
        {"invert", "true", ""},
        {"contrast", "number", "1 leaves it alone"},
        {"brightness", "number", ""},
        {"power", "number", "gamma; above 1 darkens the midtones"},
        {"threshold", "number, plus soft=0.01", "make it two-tone"},
        {"range", "[low, high]", "squeeze into a band, for a subtle variation"},
    };
    static const Doc kSurfaceDocs[] = {
        {"colours", "[\"#332211\", \"#aa8866\"] or [[0, \"#332211\"], [1, \"#aa8866\"]]",
         "the ramp the field is coloured through"},
        {"roughness", "number, or [at black, at white]",
         "defaults to dark being rougher, which is true of mortar, rust and wear"},
        {"metallic", "0 to 1", ""},
        {"occlusion", "0 to 1", "how much the field's low ground is shaded"},
        {"bump", "number", "how deep the normal map reads"},
        {"tile", "number", "texture repeats per metre"},
        {"uv", "true", "use the mesh's own UVs instead of projecting from three "
         "directions; only worth it on geometry somebody unwrapped"},
    };
    auto list = [](const Doc *docs, size_t n) {
        Json out = Json::array();
        for (size_t i = 0; i < n; i++) {
            Json j = Json::object();
            j.set("name", docs[i].name);
            j.set("params", docs[i].params);
            if (docs[i].note[0]) j.set("note", docs[i].note);
            out.push(j);
        }
        return out;
    };
    Json j = Json::object();
    j.set("about",
          "A surface is one scalar field over the unit square, coloured at the "
          "end. The same field becomes the albedo through a colour ramp, the "
          "normal map through its slope, and the roughness through its value -- "
          "so the mortar comes out rougher than the brick without anybody "
          "saying so twice. Everything tiles.");
    j.set("example",
          R"({"pattern":"bricks","rows":6,"columns":3,"mortar":0.05,)"
          R"("colours":[["#3b2b24"],["#9c6b4f"]],"bump":1.2})");
    j.set("patterns", list(kPatternDocs, sizeof(kPatternDocs) / sizeof(kPatternDocs[0])));
    j.set("operations", list(kOpDocs, sizeof(kOpDocs) / sizeof(kOpDocs[0])));
    j.set("shaping", list(kShapeDocs, sizeof(kShapeDocs) / sizeof(kShapeDocs[0])));
    j.set("surface", list(kSurfaceDocs, sizeof(kSurfaceDocs) / sizeof(kSurfaceDocs[0])));
    return j;
}

}  // namespace wr::gen
