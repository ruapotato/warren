// Warren -- what every shader in the engine can assume.
//
// BINDINGS ARE GLOBALLY UNIQUE: binding = set * 8 + slot. Vulkan reads
// the (set, binding) pair; OpenGL only gets the binding, because
// cross-compiling to GLSL flattens the sets away. Making them unique by
// construction means the flattening cannot collide, which it silently
// would otherwise -- two uniform blocks landing on binding 0 and one
// of them reading the other's bytes.
#ifndef WR_COMMON_GLSL
#define WR_COMMON_GLSL

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
    ivec4 counts;           // lights, unused, unused, unused
    vec4 env;               // specular mips, intensity, unused, unused
} frame;

// ------------------------------------------------------- punctual lights
//
// CLUSTERED, AND CLUSTERED PER VIEW.
//
// The screen is divided into a grid of froxels -- tiles in x and y,
// exponential slices in z -- and each one holds the indices of the
// lights that reach it. A fragment looks up its own froxel and shades
// against those, so the cost is the lights that actually touch the
// pixel rather than every light in the level.
//
// Per VIEW, not per frame, and that is the portal engine's version of
// this. A portal view is a different camera looking at a different
// part of the world through the same pixels; its froxels contain
// different lights. The grids for every view in the frame live in one
// buffer end to end and each view carries the index of where its own
// block starts.
#define CLUSTER_X 16
#define CLUSTER_Y 9
#define CLUSTER_Z 24
#define CLUSTER_COUNT (CLUSTER_X * CLUSTER_Y * CLUSTER_Z)
// Per froxel. A froxel that more lights than this reach keeps the
// nearest; the binder sorts by distance so what is dropped is what
// contributed least.
#define CLUSTER_MAX_LIGHTS 16

#define LIGHT_OMNI 0
#define LIGHT_SPOT 1

struct Light {
    vec4 position_range;    // xyz world, w = range in metres
    vec4 colour_energy;     // rgb colour, a = energy in candela
    vec4 direction_cone;    // xyz the way it points, w = cos(outer angle)
    vec4 params;            // cos(inner), source radius, type, shadow near
    // WHERE ITS SHADOW LIVES IN THE ATLAS, as a tile index rather
    // than a rectangle: an omni takes six consecutive tiles and they
    // may wrap onto the next row, so a rectangle could not describe
    // it while an index always can.
    vec4 shadow;            // base tile, tiles per row, tile uv size, unused
    // ONE MATRIX PER FACE, UPLOADED RATHER THAN DERIVED.
    //
    // A spot uses [0]; an omni uses [face]. Deriving a cube face's
    // basis in the shader from a forward and an up vector looked like
    // an easy saving and is not: the standard texel-to-direction
    // mapping is left-handed with respect to a right-handed camera,
    // so a face rendered by an ordinary view matrix is mirrored
    // relative to the mapping that reads it -- and the symptom is a
    // hard seam where two faces meet, which reads as a bias problem.
    //
    // This atlas is a plain 2D texture, not a hardware cubemap, so
    // there is no convention that has to be matched: the CPU picks
    // the frustum, uploads exactly the matrix it rendered with, and
    // the shader projects. 384 bytes a light that the scene will
    // never notice.
    mat4 shadow_view_proj[6];
};

layout(set = SET_FRAME, binding = B_FRAME(2), std430) readonly buffer Lights {
    Light lights[];
};
// One count per froxel, for every view in the frame.
layout(set = SET_FRAME, binding = B_FRAME(3), std430) readonly buffer Clusters {
    uint cluster_count[];
};
// CLUSTER_MAX_LIGHTS indices per froxel, at cluster * CLUSTER_MAX_LIGHTS.
layout(set = SET_FRAME, binding = B_FRAME(4), std430) readonly buffer LightIndex {
    uint light_index[];
};

// --------------------------------------------------- the environment
//
// Two cubemaps baked from the sky at start-up and whenever the sun
// moves: the cosine-convolved irradiance a diffuse surface receives,
// and the GGX-prefiltered radiance a specular one reflects, one mip
// per roughness. frame.env.x is the number of mips in the specular
// one; zero means there is no environment and the shader falls back.
layout(set = SET_FRAME, binding = B_FRAME(5)) uniform samplerCube env_irradiance;
layout(set = SET_FRAME, binding = B_FRAME(6)) uniform samplerCube env_specular;

// One atlas for every punctual light that casts. A spot takes one
// tile, an omni six.
layout(set = SET_FRAME, binding = B_FRAME(7)) uniform sampler2DShadow shadow_atlas;

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
    // Where this view's froxel grid starts, and how to find a slice.
    // slice = log2(z) * cluster.z + cluster.w, clamped.
    vec4 cluster;           // base cluster (as float), unused, scale, bias
} view;

// Which froxel a fragment is in. `frag` is gl_FragCoord.xy, which is
// measured from the TOP-LEFT on both backends -- see the note on
// viewports in docs/conventions.md -- and so is the binder's tile
// numbering, or the two would index different halves of the screen.
int cluster_of(vec2 frag, float view_depth) {
    ivec2 tile = ivec2(frag * frame.screen.zw * vec2(CLUSTER_X, CLUSTER_Y));
    tile = clamp(tile, ivec2(0), ivec2(CLUSTER_X - 1, CLUSTER_Y - 1));
    int slice = int(log2(max(view_depth, 1e-4)) * view.cluster.z + view.cluster.w);
    slice = clamp(slice, 0, CLUSTER_Z - 1);
    return int(view.cluster.x) +
           (slice * CLUSTER_Y + tile.y) * CLUSTER_X + tile.x;
}

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
    // Straight from Projection::frustum, which puts
    //   ndc = near * (far - z) / (z * (far - near))
    // and inverts to this. The 1.0 - depth_ndc that used to be here
    // was an extra reversal on top of a reverse-Z matrix, so it
    // returned the far plane for the near one; nothing in the engine
    // called it yet, which is the only reason it had not been seen.
    return (near * far) / max(mix(near, far, depth_ndc), 1e-9);
}

// The other direction: a distance along the view axis, as the depth a
// reverse-Z projection with these planes would have written. What a
// cube shadow's lookup needs, since the face it reads recorded
// exactly this.
float depth_from_linear(float z, float near, float far) {
    if (far <= 0.0) return near / max(z, 1e-9);
    return (near * (far - z)) / max(z * (far - near), 1e-9);
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

// HOW A PUNCTUAL LIGHT FALLS OFF.
//
// Inverse square, because that is what light does and because keeping
// the units physical is what lets a scene lit for noon still work at
// dusk. The windowing term is Karis's: it takes the tail of the
// inverse square, which never reaches zero, and eases it to exactly
// zero at `range` -- so a light can be culled at a finite distance
// without a visible edge where it stops.
float distance_attenuation(float dist_sq, float range, float radius) {
    // The source radius keeps the divide finite at the centre and
    // makes a light behave like a small sphere rather than a point.
    float d2 = max(dist_sq, radius * radius);
    float factor = dist_sq / max(range * range, 1e-6);
    float smooth_factor = clamp(1.0 - factor * factor, 0.0, 1.0);
    return (smooth_factor * smooth_factor) / d2;
}

// WHICH OF THE SIX FRUSTA A DIRECTION FALLS IN: the dominant axis,
// and nothing subtler. The order matches the CPU's face table, and
// that is the whole of the agreement between them -- where on the
// face it lands comes from that face's own matrix.
int cube_face_of(vec3 dir) {
    vec3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z) return dir.x > 0.0 ? 0 : 1;
    if (a.y >= a.z) return dir.y > 0.0 ? 2 : 3;
    return dir.z > 0.0 ? 4 : 5;
}

// The atlas uv for a tile index and a position within it, with the
// sample pulled a texel inside the tile so a filter tap cannot read
// its neighbour's shadow.
vec2 atlas_uv(float base_tile, float face, float per_row, float tile_uv,
              vec2 uv) {
    float tile = base_tile + face;
    vec2 cell = vec2(mod(tile, per_row), floor(tile / per_row));
    vec2 inset = vec2(1.5 / max(textureSize(shadow_atlas, 0).x, 1));
    uv = clamp(uv, inset / tile_uv, 1.0 - inset / tile_uv);
    return (cell + uv) * tile_uv;
}

// Hash and noise, for dithering and for anything that wants a stable
// random per pixel.
float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

#endif  // WR_COMMON_GLSL
