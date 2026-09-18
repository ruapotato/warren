#include "noise.h"

#include <cmath>

namespace mf::voxel {

float perlin(const Vec3 &p, uint32_t seed, Vec3 *out_gradient) {
    const int32_t xi = int32_t(std::floor(p.x));
    const int32_t yi = int32_t(std::floor(p.y));
    const int32_t zi = int32_t(std::floor(p.z));
    const float xf = p.x - float(xi);
    const float yf = p.y - float(yi);
    const float zf = p.z - float(zi);

    const float u = fade(xf), v = fade(yf), w = fade(zf);
    const float du = fade_derivative(xf);
    const float dv = fade_derivative(yf);
    const float dw = fade_derivative(zf);

    // The eight corner gradients, and the dot with the offset to each.
    float n[8];
    Vec3 g[8];
    for (int i = 0; i < 8; i++) {
        const int ox = i & 1, oy = (i >> 1) & 1, oz = (i >> 2) & 1;
        g[i] = gradient(hash3(xi + ox, yi + oy, zi + oz, seed));
        n[i] = g[i].x * (xf - float(ox)) + g[i].y * (yf - float(oy)) +
               g[i].z * (zf - float(oz));
    }

    // Trilinear blend of the eight.
    const float nx00 = lerp(n[0], n[1], u);
    const float nx10 = lerp(n[2], n[3], u);
    const float nx01 = lerp(n[4], n[5], u);
    const float nx11 = lerp(n[6], n[7], u);
    const float nxy0 = lerp(nx00, nx10, v);
    const float nxy1 = lerp(nx01, nx11, v);
    const float value = lerp(nxy0, nxy1, w);

    if (out_gradient) {
        // The derivative of the blend, by the product rule. Each term
        // is the gradient carried through the interpolation plus the
        // interpolation weight's own derivative times the difference.
        Vec3 gx00 = lerp(g[0], g[1], u);
        Vec3 gx10 = lerp(g[2], g[3], u);
        Vec3 gx01 = lerp(g[4], g[5], u);
        Vec3 gx11 = lerp(g[6], g[7], u);
        Vec3 gxy0 = lerp(gx00, gx10, v);
        Vec3 gxy1 = lerp(gx01, gx11, v);
        Vec3 grad = lerp(gxy0, gxy1, w);
        grad.x += du * (lerp(n[1] - n[0], n[3] - n[2], v) * (1.0f - w) +
                        lerp(n[5] - n[4], n[7] - n[6], v) * w);
        grad.y += dv * lerp(nx10 - nx00, nx11 - nx01, w);
        grad.z += dw * (nxy1 - nxy0);
        *out_gradient = grad;
    }
    return value;
}

float fbm(const Vec3 &p, uint32_t seed, int octaves, float lacunarity, float gain,
          Vec3 *out_gradient) {
    float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, norm = 0.0f;
    Vec3 grad;
    for (int i = 0; i < octaves; i++) {
        Vec3 g;
        sum += perlin(p * frequency, seed + uint32_t(i) * 0x9E3779B1u,
                      out_gradient ? &g : nullptr) *
               amplitude;
        if (out_gradient) grad += g * (amplitude * frequency);
        norm += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    if (norm < 1e-9f) norm = 1.0f;
    if (out_gradient) *out_gradient = grad / norm;
    return sum / norm;
}

float ridged(const Vec3 &p, uint32_t seed, int octaves, float lacunarity,
             float gain) {
    float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, norm = 0.0f;
    float previous = 1.0f;
    for (int i = 0; i < octaves; i++) {
        float n = 1.0f - std::fabs(perlin(p * frequency,
                                          seed + uint32_t(i) * 0x85EBCA77u));
        n *= n;
        // Each octave is attenuated by the one above, which is what
        // keeps ridges sharp instead of turning into noise on noise.
        n *= previous;
        previous = n;
        sum += n * amplitude;
        norm += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return norm > 1e-9f ? sum / norm : 0.0f;
}

float worley(const Vec3 &p, uint32_t seed) {
    const int32_t xi = int32_t(std::floor(p.x));
    const int32_t yi = int32_t(std::floor(p.y));
    const int32_t zi = int32_t(std::floor(p.z));
    float best = 1e30f;
    for (int dz = -1; dz <= 1; dz++)
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                const int32_t cx = xi + dx, cy = yi + dy, cz = zi + dz;
                Vec3 feature(float(cx) + hash_float(cx, cy, cz, seed),
                             float(cy) + hash_float(cx, cy, cz, seed ^ 0x1234u),
                             float(cz) + hash_float(cx, cy, cz, seed ^ 0x9ABCu));
                float d = (feature - p).length_sq();
                if (d < best) best = d;
            }
    return std::sqrt(best);
}

}  // namespace mf::voxel
