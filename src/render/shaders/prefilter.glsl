// Manifold -- the specular half of image-based lighting.
//
// One mip of the environment cube per roughness: mip 0 is the sharp
// reflection, the last one is nearly diffuse. A rough surface then
// reads a blurry mip and a mirror reads mip 0, and the blur is a GGX
// lobe rather than a box, so the highlight has the right shape.
//
// Importance sampled towards the lobe, which is what makes a few
// dozen samples enough where uniform sampling would need thousands.
#include "sky_model.glsl"
#include "brdf.glsl"

layout(set = SET_MATERIAL, binding = B_MATERIAL(1)) uniform samplerCube env;

#pragma stage vertex

layout(location = 0) out vec2 v_st;

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_st = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}

#pragma stage fragment

layout(location = 0) in vec2 v_st;
layout(location = 0) out vec4 out_colour;

// Hammersley: a low-discrepancy sequence, which for this many samples
// is visibly less noisy than a random one and costs a bit reversal.
float radical_inverse(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec3 importance_ggx(vec2 xi, vec3 n, float rough) {
    float a = rough * rough;
    float phi = 2.0 * PI * xi.x;
    float cos_theta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sin_theta = sqrt(1.0 - cos_theta * cos_theta);
    vec3 h = vec3(sin_theta * cos(phi), sin_theta * sin(phi), cos_theta);

    vec3 up = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tx = normalize(cross(up, n));
    vec3 ty = cross(n, tx);
    return normalize(tx * h.x + ty * h.y + n * h.z);
}

void main() {
    vec2 st = vec2(v_st.x, 1.0 - v_st.y);
    // params.x = face, params.y = roughness, params.z = source size.
    vec3 n = normalize(cube_direction(int(push.params.x), st));
    float rough = push.params.y;
    // THE VIEW IS ASSUMED TO BE THE NORMAL, which is the standard
    // approximation and the reason a prefiltered cube cannot show
    // grazing-angle stretch. It is what everyone ships, and the
    // alternative is a second parameter and a much larger table.
    vec3 v = n;

    const uint samples = 64u;
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (uint i = 0u; i < samples; i++) {
        vec2 xi = vec2(float(i) / float(samples), radical_inverse(i));
        vec3 h = importance_ggx(xi, n, rough);
        vec3 l = normalize(2.0 * dot(v, h) * h - v);
        float n_dot_l = dot(n, l);
        if (n_dot_l <= 0.0) continue;

        // MIP SELECTION ON THE SOURCE, which is what stops the
        // result sparkling. A rough lobe spreads 64 samples over a
        // large solid angle, so each one stands for a patch of sky
        // much bigger than a texel; reading mip 0 there samples 64
        // random points out of thousands and the variance shows up
        // as fireflies that flicker when the sun moves.
        float n_dot_h = max(dot(n, h), 0.0);
        float d = d_ggx_prefilter(n_dot_h, rough);
        float pdf = d * n_dot_h / (4.0 * max(dot(v, h), 1e-4)) + 1e-4;
        float texels = 4.0 * PI / (6.0 * push.params.z * push.params.z);
        float sample_solid_angle = 1.0 / (float(samples) * pdf + 1e-4);
        float lod = rough <= 0.0 ? 0.0
                                 : 0.5 * log2(sample_solid_angle / texels);

        sum += textureLod(env, l, max(lod, 0.0)).rgb * n_dot_l;
        weight += n_dot_l;
    }
    out_colour = vec4(sum / max(weight, 1e-4), 1.0);
}
