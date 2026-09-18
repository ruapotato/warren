// Warren -- the standard surface shader.
//
// Forward, physically based, one directional light with cascaded
// shadows plus clustered punctual lights. Forward rather than deferred
// because a portal is a stencilled region of the SAME framebuffer as
// everything around it: a deferred renderer would need a G-buffer per
// recursion level, or a way to tell which level wrote each pixel, and
// both cost more than the deferred lighting saves.
#include "mesh_shade.glsl"

#pragma stage vertex

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;
layout(location = 4) in vec4 in_colour;

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec4 v_tangent;
layout(location = 3) out vec2 v_uv;
layout(location = 4) out vec4 v_colour;

void main() {
    vec4 world = push.model * vec4(in_position, 1.0);
    v_world = world.xyz;
    // Derived, not passed: see the note on the push block.
    mat3 nm = normal_basis(push.model);
    v_normal = normalize(nm * in_normal);
    v_tangent = vec4(normalize(nm * in_tangent.xyz), in_tangent.w);
    v_uv = in_uv * material.uv_transform.xy + material.uv_transform.zw;
    v_colour = in_colour * push.tint;
    gl_Position = view.view_proj * world;
}

#pragma stage fragment
