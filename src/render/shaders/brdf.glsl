// Warren -- the reflectance model, in one place.
//
// Used by the surface shader to light a pixel and by the environment
// prefilter to blur a cubemap into the same lobe. Two copies of a GGX
// distribution drift, and when they do a rough surface's reflection
// no longer matches its highlight -- which reads as "the IBL is
// wrong" rather than as the duplicated function it is.
#ifndef WR_BRDF_GLSL
#define WR_BRDF_GLSL

#include "common.glsl"

// Trowbridge-Reitz. `rough` is perceptual roughness; the square of it
// is the GGX alpha, which is the convention glTF uses and therefore
// the one an imported material already speaks.
float d_ggx(float n_dot_h, float rough) {
    float a = rough * rough;
    float a2 = a * a;
    float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

// The same distribution, named apart for the prefilter's mip
// selection so that reading it there says what it is for.
float d_ggx_prefilter(float n_dot_h, float rough) {
    return d_ggx(n_dot_h, rough);
}

float v_smith(float n_dot_v, float n_dot_l, float rough) {
    // Height-correlated Smith, the Hammon approximation -- one divide
    // and no square roots, and indistinguishable from the exact form.
    float a = rough * rough;
    float gv = n_dot_l * (n_dot_v * (1.0 - a) + a);
    float gl = n_dot_v * (n_dot_l * (1.0 - a) + a);
    return 0.5 / max(gv + gl, 1e-6);
}

vec3 f_schlick(vec3 f0, float v_dot_h) {
    float f = pow(1.0 - v_dot_h, 5.0);
    return f0 + (1.0 - f0) * f;
}

// Fresnel with a roughness term, for image-based lighting. A rough
// surface's grazing reflection is dimmer than a smooth one's, and
// plain Schlick does not know that -- so a rough wall lit only by the
// environment gets a bright rim it should not have.
vec3 f_schlick_roughness(vec3 f0, float n_dot_v, float rough) {
    vec3 ceiling = max(vec3(1.0 - rough), f0);
    return f0 + (ceiling - f0) * pow(clamp(1.0 - n_dot_v, 0.0, 1.0), 5.0);
}

vec3 brdf(vec3 n, vec3 v, vec3 l, vec3 albedo, float metallic, float rough) {
    vec3 h = normalize(v + l);
    float n_dot_v = max(dot(n, v), 1e-4);
    float n_dot_l = max(dot(n, l), 0.0);
    float n_dot_h = max(dot(n, h), 0.0);
    float v_dot_h = max(dot(v, h), 0.0);

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 diffuse = albedo * (1.0 - metallic) / PI;
    vec3 spec = f_schlick(f0, v_dot_h) * d_ggx(n_dot_h, rough) *
                v_smith(n_dot_v, n_dot_l, rough);
    return (diffuse + spec) * n_dot_l;
}

// THE SPLIT-SUM'S SECOND HALF, WITHOUT A LOOKUP TABLE.
//
// The usual implementation bakes this into a 2D texture at start-up.
// Lazarov's analytic fit is four multiply-adds, accurate to well
// inside what an 8-bit display can show, and saves a texture, a
// binding, a render pass and the chance of sampling it with the
// wrong filter. Returns the scale and bias to apply to F0.
vec2 env_brdf(float n_dot_v, float rough) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

#endif  // WR_BRDF_GLSL
