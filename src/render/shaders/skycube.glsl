// Warren -- the sky, baked into a cubemap face.
//
// The same sky_radiance the backdrop uses, evaluated for the
// direction each texel of one cube face looks in. Six of these and
// the scene has an environment to be lit by that cannot disagree with
// the sky behind it.
#include "sky_model.glsl"

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

void main() {
    // The full-screen triangle's uv has v = 0 at the BOTTOM, while a
    // cube face's t runs downwards, so it is flipped here -- the same
    // flip, for the same reason, as sampling any other render target.
    vec2 st = vec2(v_st.x, 1.0 - v_st.y);
    out_colour = vec4(sky_radiance(cube_direction(int(push.params.x), st)), 1.0);
}
