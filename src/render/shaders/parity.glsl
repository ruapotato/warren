// Manifold -- the backend parity probe.
//
// Deliberately exercises the things the two backends are most likely to
// disagree about: which way +Y points, which way a triangle winds,
// whether depth is reversed, and whether the stencil test means the
// same thing. If Vulkan and OpenGL produce the same pixels for this,
// they agree about the conventions the whole engine rests on.
#include "common.glsl"

#pragma stage vertex

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_colour;
layout(location = 0) out vec4 v_colour;

void main() {
    v_colour = in_colour * push.tint;
    gl_Position = view.view_proj * push.model * vec4(in_position, 1.0);
}

#pragma stage fragment

layout(location = 0) in vec4 v_colour;
layout(location = 0) out vec4 out_colour;

void main() {
    out_colour = v_colour;
}
