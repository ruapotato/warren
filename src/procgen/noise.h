// Warren -- noise.
//
// Self-contained and deterministic: the same seed and the same
// coordinate give the same value on every machine, every run, and at
// every level of detail. That last one is the important one -- a
// chunk meshed at a coarse level and then again at a fine one must
// agree about where the ground is, or the world visibly changes shape
// as you walk towards it.
#pragma once

#include <cstdint>

#include "core/math/vector.h"

namespace wr::gen {

// A good integer hash (Wang / Murmur finaliser). Fast, and its low
// bits are as good as its high ones, which a shift-and-xor hash's are
// not.
inline uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

inline uint32_t hash3(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint32_t h = seed;
    h = hash_u32(h ^ uint32_t(x) * 0x9E3779B1u);
    h = hash_u32(h ^ uint32_t(y) * 0x85EBCA77u);
    h = hash_u32(h ^ uint32_t(z) * 0xC2B2AE3Du);
    return h;
}

inline float hash_float(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    return float(hash3(x, y, z, seed) & 0xFFFFFF) / float(0xFFFFFF);
}

// Gradient noise. Twelve gradients on the edge midpoints of a cube,
// which is Perlin's improved set: no axis bias and no square roots.
inline Vec3 gradient(uint32_t h) {
    switch (h & 15) {
        case 0: return {1, 1, 0};
        case 1: return {-1, 1, 0};
        case 2: return {1, -1, 0};
        case 3: return {-1, -1, 0};
        case 4: return {1, 0, 1};
        case 5: return {-1, 0, 1};
        case 6: return {1, 0, -1};
        case 7: return {-1, 0, -1};
        case 8: return {0, 1, 1};
        case 9: return {0, -1, 1};
        case 10: return {0, 1, -1};
        case 11: return {0, -1, -1};
        case 12: return {1, 1, 0};
        case 13: return {-1, 1, 0};
        case 14: return {0, -1, 1};
        default: return {0, -1, -1};
    }
}

inline float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
inline float fade_derivative(float t) { return 30.0f * t * t * (t * (t - 2.0f) + 1.0f); }

// Perlin noise in [-1, 1], with its ANALYTIC DERIVATIVE.
//
// The derivative is not a luxury: the surface normal of a noise-built
// terrain is its gradient, and computing that by sampling the noise
// six more times costs six times as much and is noisier than the
// exact answer.
float perlin(const Vec3 &p, uint32_t seed, Vec3 *out_gradient = nullptr);

// Fractional Brownian motion: octaves of Perlin at doubling frequency
// and halving amplitude. `out_gradient` accumulates likewise.
float fbm(const Vec3 &p, uint32_t seed, int octaves, float lacunarity = 2.0f,
          float gain = 0.5f, Vec3 *out_gradient = nullptr);

// Ridged: 1 - |noise|, squared. Makes mountain ridges and canyon
// walls rather than rolling hills.
float ridged(const Vec3 &p, uint32_t seed, int octaves, float lacunarity = 2.0f,
             float gain = 0.5f);

// Worley / cellular, returning the distance to the nearest feature
// point. For caves and for rock.
float worley(const Vec3 &p, uint32_t seed);

// TILEABLE VERSIONS, for textures.
//
// A texture that does not tile is a texture nobody can use on a
// wall, and Perlin noise on an unbounded lattice does not tile. The
// fix is to wrap the LATTICE rather than the sample point: corner
// (period, y, z) hashes to the same gradient as corner (0, y, z), so
// the field is exactly periodic with no blending and no loss of
// quality. `period` must be a whole number of cells, which is why it
// is an int.
//
// Terrain does not want this -- a planet that repeats every 64
// metres is a worse planet -- so it is a separate entry point rather
// than a parameter everything has to pass zero for.
float perlin_tiled(const Vec3 &p, uint32_t seed, int period);
float fbm_tiled(const Vec3 &p, uint32_t seed, int octaves, int period,
                float lacunarity = 2.0f, float gain = 0.5f);
float worley_tiled(const Vec3 &p, uint32_t seed, int period);

}  // namespace wr::gen
