// Manifold -- the sky.
//
// A procedural gradient with a sun, drawn as one triangle at the far
// plane after the opaque pass. Depth-tested but not depth-written, so
// it fills exactly the pixels nothing else reached -- including,
// because it is stencil-tested like everything else, the pixels inside
// a portal that nothing inside the portal reached.
#include "sky_model.glsl"

#pragma stage vertex

layout(location = 0) out vec3 v_direction;

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vec2 ndc = uv * 2.0 - 1.0;
    // THE NEAR PLANE, NOT THE FAR ONE.
    //
    // Unprojecting the far plane looks like the obvious way to get a
    // view ray, and for an INFINITE far plane it is a division by
    // zero: the far plane is at infinity, so its homogeneous w is
    // zero and the result is a NaN that paints the whole sky black.
    // The near plane is always finite, and a point on it is just as
    // good a direction from the eye.
    vec4 near_point = view.inv_proj * vec4(ndc, 1.0, 1.0);
    v_direction = mat3(view.inv_view) * (near_point.xyz / near_point.w);
    // Exactly at the far plane, so the depth test keeps it behind
    // everything that was drawn.
    gl_Position = vec4(ndc, 0.0, 1.0);
}

#pragma stage fragment

layout(location = 0) in vec3 v_direction;
layout(location = 0) out vec4 out_colour;

void main() {
    vec3 sky = sky_radiance(v_direction);

    // Dither, because a smooth gradient in 8 bits bands visibly and
    // this is the cheapest fix there is.
    sky += (hash12(gl_FragCoord.xy) - 0.5) * (1.0 / 255.0);

    out_colour = vec4(sky, 1.0);
}
