// Manifold -- the sky, as a function of direction.
//
// ONE DEFINITION, TWO CONSUMERS. sky.glsl draws it on screen and
// skycube.glsl bakes it into the cubemap that lights the scene. If
// those were two copies of the gradient, the lighting would drift
// away from the backdrop the first time anyone tuned one of them, and
// the symptom -- a world lit slightly wrong for the sky behind it --
// is one nobody traces back to a duplicated shader.
#ifndef MF_SKY_MODEL_GLSL
#define MF_SKY_MODEL_GLSL

#include "common.glsl"

// Radiance from `dir`, in linear light. Not tonemapped, not dithered:
// the sun is worth several hundred and the point of an HDR pipeline
// is that it stays that way until the tonemap.
vec3 sky_radiance(vec3 dir) {
    dir = normalize(dir);
    vec3 sun = normalize(frame.sun_direction.xyz);

    // A physically-flavoured gradient: more scattering towards the
    // horizon, warmer near the sun, darker overhead.
    float up = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    float horizon = pow(1.0 - abs(dir.y), 4.0);

    vec3 zenith = frame.ambient.rgb * 1.4;
    vec3 ground = frame.fog.rgb * 0.5;
    vec3 sky = mix(ground, zenith, smoothstep(0.42, 0.62, up));
    sky = mix(sky, frame.fog.rgb, horizon * 0.85);

    // Forward scattering around the sun, then the disc itself.
    float cos_sun = max(dot(dir, sun), 0.0);
    sky += frame.sun_colour.rgb * pow(cos_sun, 8.0) * 0.25;
    sky += frame.sun_colour.rgb * pow(cos_sun, 900.0) * 6.0;
    float disc = smoothstep(0.9993, 0.9997, cos_sun);
    sky += frame.sun_colour.rgb * disc * frame.sun_colour.a * 12.0;
    return sky;
}

// THE DIRECTION A CUBEMAP TEXEL LOOKS IN.
//
// `st` is the face's texture coordinate in [0,1] with t increasing
// DOWNWARDS, which is what a render target gives: row 0 is the top on
// both backends. The six cases are the standard cube face mapping and
// there is no room to be creative with them -- get one sign wrong and
// the world is lit by a sky reflected in an axis, which looks almost
// right. tests/test_ibl samples the baked cube against this same
// function to make sure it is not almost right.
vec3 cube_direction(int face, vec2 st) {
    vec2 uv = st * 2.0 - 1.0;
    if (face == 0) return vec3(1.0, -uv.y, -uv.x);
    if (face == 1) return vec3(-1.0, -uv.y, uv.x);
    if (face == 2) return vec3(uv.x, 1.0, uv.y);
    if (face == 3) return vec3(uv.x, -1.0, -uv.y);
    if (face == 4) return vec3(uv.x, -uv.y, 1.0);
    return vec3(-uv.x, -uv.y, -1.0);
}

#endif  // MF_SKY_MODEL_GLSL
