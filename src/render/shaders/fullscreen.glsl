// Manifold -- one triangle covering the screen.
//
// Three vertices, no vertex buffer, no index buffer: the positions come
// from gl_VertexIndex. A single triangle rather than two because the
// diagonal seam of a quad makes the GPU shade the pixels along it
// twice.
//
// Used for the sky, for post-processing, and -- the reason it is in
// the portal renderer -- to CLEAR DEPTH INSIDE A STENCILLED REGION.
// There is no API for "clear the depth buffer where the stencil says
// so"; drawing a triangle at the far plane with depth writes on and
// the stencil test set is that operation.
#include "common.glsl"

#pragma stage vertex

layout(location = 0) out vec2 v_uv;

void main() {
    // (0,0) (2,0) (0,2) in UV, which is a triangle twice the size of
    // the screen; the half outside is clipped for free.
    v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    // params.x is the depth to write. Under reverse-Z the far plane is
    // 0, which is what the depth-clear pass wants.
    gl_Position = vec4(v_uv * 2.0 - 1.0, push.params.x, 1.0);
}

#pragma stage fragment

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_colour;

void main() {
    out_colour = push.tint;
}
