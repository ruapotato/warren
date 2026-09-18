// Warren -- the depth-only pass a shadow cascade is built from.
//
// THE LIGHT IS A VIEW. A cascade is uploaded through the same ViewData
// block a camera uses, with an orthographic projection and the sun's
// orientation, so this shader needs nothing a normal draw does not
// already have and the cascade matrix in FrameData is literally the
// same matrix this pass rendered with. Nothing can drift between the
// pass that writes the shadow map and the shader that reads it,
// because there is one matrix and one upload path.
//
// Reverse-Z applies here too: the map clears to 0, the depth test is
// GREATER_EQUAL, and the comparison sampler that reads it back is set
// the same way. A shadow map is a depth buffer, so it gets the depth
// buffer's convention -- there is no second convention in this engine.
#include "common.glsl"

// Alpha-cutout geometry must cast a cutout shadow: a leaf that is a
// texture on a quad casts a leaf, not a quad. The material set is
// bound for every draw anyway, so this costs one texture fetch on the
// materials that ask for it and a branch that is uniform per draw on
// the ones that do not.
layout(set = SET_MATERIAL, binding = B_MATERIAL(0), std140) uniform MaterialData {
    vec4 albedo;
    vec4 emissive;
    vec4 params;
    vec4 uv_transform;
    vec4 flags;             // alpha_cutoff, has_normal_map, has_orm, unlit
} material;

layout(set = SET_MATERIAL, binding = B_MATERIAL(1)) uniform sampler2D tex_albedo;

#pragma stage vertex

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;
layout(location = 4) in vec4 in_colour;

layout(location = 0) out vec2 v_uv;

void main() {
    vec4 world = push.model * vec4(in_position, 1.0);
    v_uv = in_uv * material.uv_transform.xy + material.uv_transform.zw;
    gl_Position = view.view_proj * world;
}

#pragma stage fragment

layout(location = 0) in vec2 v_uv;

void main() {
    // No colour attachment: depth is the entire output. The discard is
    // the only reason this stage exists at all.
    if (material.flags.x > 0.0) {
        float a = texture(tex_albedo, v_uv).a * material.albedo.a;
        if (a < material.flags.x) discard;
    }
}
