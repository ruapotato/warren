// Manifold -- what every shader in the engine can assume.
//
// BINDINGS ARE GLOBALLY UNIQUE: binding = set * 8 + slot. Vulkan reads
// the (set, binding) pair; OpenGL only gets the binding, because
// cross-compiling to GLSL flattens the sets away. Making them unique by
// construction means the flattening cannot collide, which it silently
// would otherwise -- two uniform blocks landing on binding 0 and one
// of them reading the other's bytes.
#ifndef MF_COMMON_GLSL
#define MF_COMMON_GLSL

#define SET_FRAME     0
#define SET_VIEW      1
#define SET_MATERIAL  2
#define SET_DRAW      3

#define B_FRAME(slot)     (0  + (slot))
#define B_VIEW(slot)      (8  + (slot))
#define B_MATERIAL(slot)  (16 + (slot))
#define B_DRAW(slot)      (24 + (slot))

// ---------------------------------------------------------------- frame

// Everything that is true for the whole frame, whichever camera is
// looking. Bound once and left alone.
layout(set = SET_FRAME, binding = B_FRAME(0), std140) uniform FrameData {
    vec4 time;              // seconds, sin(t), delta, frame number
    vec4 sun_direction;     // xyz towards the sun, w = 0
    vec4 sun_colour;        // rgb radiance, a = intensity
    vec4 ambient;           // rgb, a = strength
    vec4 fog;               // rgb, a = density
    vec4 fog_params;        // start, end, height falloff, unused
    mat4 sun_view_proj[4];  // shadow cascades
    vec4 cascade_splits;    // view-space distance at which each ends
    vec4 cascade_texel;     // world size of one shadow texel, per cascade
    vec4 screen;            // width, height, 1/width, 1/height
} frame;

// ----------------------------------------------------------------- view
//
// PER VIEW, NOT PER FRAME, AND THAT DISTINCTION IS THE ENGINE.
//
// A portal is a second view of the same frame: same sun, same time,
// same materials, different camera and -- crucially -- a different
// PROJECTION, because a portal view's near plane is oblique. Splitting
// these two blocks apart is what lets a recursion level be a bind
// rather than a re-upload of everything.
layout(set = SET_VIEW, binding = B_VIEW(0), std140) uniform ViewData {
    mat4 view;              // world -> view
    mat4 proj;              // view -> clip (may have an oblique near plane)
    mat4 view_proj;
    mat4 inv_view;
    mat4 inv_proj;
    vec4 eye;               // world position, w = 1
    vec4 near_far;          // near, far (far may be infinite), 1/near, unused
    // Which side of a portal this view is on, and how deep the
    // recursion has gone. Used for tinting debug views and for
    // deciding whether a portal frame draws itself.
    ivec4 portal;           // depth, portal id, flags, unused
} view;

// -------------------------------------------------------------- per draw

// NINETY-SIX BYTES, AND THAT IS THE BUDGET.
//
// Vulkan only guarantees 128 bytes of push constants, and a driver that
// offers more is not one to build on. There is no normal matrix here on
// purpose: the engine permits UNIFORM scale only -- Node3D::set_scale
// enforces it and a portal warp only ever produces it -- and for a
// uniform scale the inverse transpose is the model matrix's own 3x3
// with the scale divided out, which is one normalize in the vertex
// shader and sixty-four bytes saved on every draw.
layout(push_constant, std430) uniform Push {
    mat4 model;
    vec4 tint;
    vec4 params;            // free per-draw floats
} push;

// The rotation part of the model matrix, scale removed.
mat3 normal_basis(mat4 model) {
    mat3 m = mat3(model);
    // Uniform scale, so one column's length is the scale.
    float s = length(m[0]);
    return s > 1e-8 ? m * (1.0 / s) : m;
}

// -------------------------------------------------------------- helpers

const float PI = 3.14159265359;

// Reverse-Z: the near plane is 1 and the far plane is 0. Turning a
// depth sample back into a view-space distance therefore looks
// backwards compared to every tutorial, so it lives here and nowhere
// else.
float linear_depth(float depth_ndc, float near, float far) {
    // For an infinite far plane, far <= 0 is passed in.
    if (far <= 0.0) return near / max(depth_ndc, 1e-9);
    return (near * far) / max(mix(near, far, 1.0 - depth_ndc), 1e-9);
}

vec3 world_from_depth(vec2 uv, float depth_ndc, mat4 inv_view_proj) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth_ndc, 1.0);
    vec4 world = inv_view_proj * clip;
    return world.xyz / world.w;
}

float srgb_to_linear(float c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}
vec3 srgb_to_linear(vec3 c) {
    return vec3(srgb_to_linear(c.r), srgb_to_linear(c.g), srgb_to_linear(c.b));
}

// Hash and noise, for dithering and for anything that wants a stable
// random per pixel.
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

#endif  // MF_COMMON_GLSL
