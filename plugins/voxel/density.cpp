#include "density.h"

#include <algorithm>

#include "noise.h"

namespace wr::voxel {

// --------------------------------------------------------- TerrainDensity

float TerrainDensity::ground_height(float x, float z) const {
    const TerrainParams &t = params_;
    const Vec3 flat(x, 0.0f, z);

    // Continents: one big slow wave deciding where land is at all.
    float continent = fbm(flat / t.continent_scale, t.seed, 4);
    float h = t.sea_level + continent * t.continent_height;

    // Hills everywhere.
    h += fbm(flat / t.hill_scale, t.seed + 101u, t.hill_octaves) * t.hill_height;

    // Mountains only where the mask says. Smoothstepped, so a range
    // rises out of the hills instead of appearing at a contour line.
    float mask = fbm(flat / t.mountain_mask_scale, t.seed + 202u, 3) * 0.5f + 0.5f;
    if (mask > t.mountain_threshold) {
        float amount = smoothstep(t.mountain_threshold,
                                  t.mountain_threshold + 0.35f, mask);
        h += ridged(flat / t.mountain_scale, t.seed + 303u, 6) * t.mountain_height *
             amount;
    }
    return h;
}

float TerrainDensity::surface_height(float x, float z) const {
    return ground_height(x, z);
}

Sample TerrainDensity::sample(const Vec3 &p) const {
    const TerrainParams &t = params_;

    // THE WARP IS WHAT MAKES OVERHANGS.
    //
    // A height field can only ever be a landscape seen from above: no
    // cliff can lean out, no arch can form. Displacing the sample
    // point by a 3D noise before asking the height field turns the
    // whole surface into something genuinely three-dimensional, at
    // the cost of one more noise lookup.
    Vec3 warped = p;
    if (t.warp_strength > 0.0f) {
        Vec3 w(fbm(p / t.warp_scale, t.seed + 404u, 3),
               fbm((p + Vec3(31.4f, 17.2f, 9.1f)) / t.warp_scale, t.seed + 505u, 3),
               fbm((p + Vec3(-9.7f, 41.3f, 23.8f)) / t.warp_scale, t.seed + 606u, 3));
        warped += w * t.warp_strength;
    }

    const float ground = ground_height(warped.x, warped.z);
    // Positive above the ground. Not a true distance -- a steep slope
    // makes it an overestimate -- which the mesher tolerates because
    // it only interpolates between neighbouring samples.
    float d = p.y - ground;

    Sample s;
    s.distance = d;
    if (d > 0.0f) {
        s.material = 0;
    } else {
        const float depth = -d;
        if (p.y > t.snow_height)
            s.material = t.material_snow;
        else if (std::fabs(p.y - t.sea_level) < t.sand_band && depth < t.dirt_depth)
            s.material = t.material_sand;
        else if (depth < 0.9f)
            s.material = t.material_grass;
        else if (depth < t.dirt_depth)
            s.material = t.material_dirt;
        else
            s.material = t.material_rock;
    }

    // CAVES, CARVED AFTER THE FACT.
    //
    // A second field thresholded into tunnels, subtracted from the
    // rock. Kept below the surface, because a cave that breaks
    // through everywhere turns the ground into lace.
    if (t.caves && d < 0.0f && p.y < ground + t.cave_ceiling) {
        float c = std::fabs(fbm(p / t.cave_scale, t.seed + 707u, 3));
        float tunnel = t.cave_threshold - c;
        if (tunnel > 0.0f) {
            // Fade the caves out as they approach the surface, so
            // they open onto it rather than slicing it.
            float fade = clampf((ground + t.cave_ceiling - p.y) / 8.0f, 0.0f, 1.0f);
            float carve = tunnel * 12.0f * fade;
            if (carve > -s.distance) {
                s.distance = carve;
                s.material = 0;
            }
        }
    }
    return s;
}

MaterialId TerrainDensity::surface_material(const Vec3 &p,
                                            const Vec3 &normal) const {
    const TerrainParams &t = params_;
    // A slow wobble on every boundary, so grass does not stop at a
    // contour line and snow does not begin at one.
    const float wobble =
        fbm(p * (1.0f / 90.0f), t.seed + 808u, 3) * t.material_noise;

    const float flatness = dot(normal.normalized(), Vec3::up());
    if (p.y + wobble > t.snow_height)
        // Snow does not settle on a cliff.
        return flatness > 0.45f ? t.material_snow : t.material_rock;
    if (flatness < t.rock_slope) return t.material_rock;
    if (std::fabs(p.y - t.sea_level) < t.sand_band + wobble * 0.3f)
        return t.material_sand;
    return t.material_grass;
}

Vec3 TerrainDensity::gradient(const Vec3 &p, float h) const {
    // The warp and the caves make an analytic derivative long and
    // fragile; central differences on a field this smooth are
    // accurate and are only six samples.
    return DensitySource::gradient(p, h);
}

// HOW FAR THIS BOX IS FROM ANY SURFACE.
//
// The streamer uses this to throw away whole chunks without meshing
// them, and almost every chunk in a world is thrown away -- so a
// loose bound is not a small inefficiency, it is the difference
// between a terrain that streams and one that queues a thousand
// chunks and never catches up.
//
// The global worst case (the tallest mountain the parameters allow)
// is useless: it spans four hundred metres and rejects nothing. So
// the ground is sampled over the box's own footprint instead, and
// the answer is the distance from the box to that band of heights,
// widened by what the sampling might have missed between samples.
float TerrainDensity::bound(const AABB &box) const {
    if (!box.valid()) return 0.0f;
    const TerrainParams &t = params_;
    float lo = 1e30f, hi = -1e30f;
    // A 3x3 grid over the footprint. The margin below covers what
    // falls between the samples.
    for (int i = 0; i <= 2; i++)
        for (int j = 0; j <= 2; j++) {
            const float x = lerp(box.min.x, box.max.x, float(i) * 0.5f);
            const float z = lerp(box.min.z, box.max.z, float(j) * 0.5f);
            const float h = ground_height(x, z);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
    // What the samples could have missed: the warp displaces the
    // lookup, and the hills can rise between two of them. Generous,
    // because a bound that is too tight loses geometry and a bound
    // that is too loose only costs time.
    const float slack = t.warp_strength + t.hill_height * 0.5f +
                        (box.max.x - box.min.x) * 0.5f;
    lo -= slack;
    hi += slack;
    if (box.min.y > hi) return box.min.y - hi;   // entirely in the air
    if (box.max.y < lo) return lo - box.max.y;   // entirely in the rock
    return 0.0f;                                 // may contain a surface
}

// ------------------------------------------------------------------ edits

AABB Edit::bounds() const {
    if (shape == Shape::Sphere) {
        float r = radius + smooth;
        return {centre - Vec3(r), centre + Vec3(r)};
    }
    Vec3 h = half_extents + Vec3(smooth);
    return {centre - h, centre + h};
}

float Edit::field(const Vec3 &p) const {
    if (shape == Shape::Sphere) return (p - centre).length() - radius;
    // Rounded box: the classic distance, exact outside and a good
    // approximation within.
    Vec3 d = (p - centre).abs() - half_extents + Vec3(smooth);
    Vec3 outside = vmax(d, Vec3());
    float inside = std::min(d.max_axis_value(), 0.0f);
    return outside.length() + inside - smooth;
}

// ---------------------------------------------------------- EditedDensity

void EditedDensity::add_edit(const Edit &e) {
    edits_.push_back(e);
    bounds_.push_back(e.bounds());
}

void EditedDensity::clear_edits() {
    edits_.clear();
    bounds_.clear();
}

void EditedDensity::edits_in(const AABB &box, std::vector<const Edit *> &out) const {
    for (size_t i = 0; i < edits_.size(); i++)
        if (bounds_[i].intersects(box)) out.push_back(&edits_[i]);
}

Sample EditedDensity::sample(const Vec3 &p) const {
    Sample s = base_ ? base_->sample(p) : Sample{};
    for (size_t i = 0; i < edits_.size(); i++) {
        if (!bounds_[i].contains(p)) continue;
        const Edit &e = edits_[i];
        const float f = e.field(p);
        switch (e.op) {
            case Edit::Op::Subtract:
                // Max of the field and the negated edit: the union of
                // the empty spaces, which is the difference of the
                // solids.
                if (-f > s.distance) {
                    s.distance = -f;
                    if (s.distance > 0.0f) s.material = 0;
                }
                break;
            case Edit::Op::Add:
                if (f < s.distance) {
                    s.distance = f;
                    if (s.distance <= 0.0f) s.material = e.material;
                }
                break;
            case Edit::Op::Paint:
                if (f <= 0.0f && s.distance <= 0.0f) s.material = e.material;
                break;
        }
    }
    return s;
}

Vec3 EditedDensity::gradient(const Vec3 &p, float h) const {
    return DensitySource::gradient(p, h);
}

MaterialId EditedDensity::surface_material(const Vec3 &p,
                                           const Vec3 &normal) const {
    // An added lump is made of what it was added as; everything else
    // is whatever the world underneath says.
    for (size_t i = 0; i < edits_.size(); i++) {
        if (!bounds_[i].contains(p)) continue;
        const Edit &e = edits_[i];
        if (e.op == Edit::Op::Subtract) continue;
        if (e.field(p) <= 0.35f) return e.material;
    }
    return base_ ? base_->surface_material(p, normal) : MaterialId(0);
}

float EditedDensity::bound(const AABB &box) const {
    // An edit anywhere near means the base's bound no longer holds.
    for (const AABB &b : bounds_)
        if (b.intersects(box)) return 0.0f;
    return base_ ? base_->bound(box) : INF;
}

}  // namespace wr::voxel
