// Warren -- HDR to the screen.
//
// One pass, at the very end, and the only place in the engine where
// light stops being linear. The swapchain is an sRGB format, so the
// hardware does the encode on write and this shader must NOT do it
// again -- that is the classic washed-out-picture bug, and it looks
// like a lighting problem for as long as you are willing to believe it
// is one.
#include "common.glsl"

layout(set = SET_MATERIAL, binding = B_MATERIAL(0)) uniform sampler2D hdr;

#pragma stage vertex

layout(location = 0) out vec2 v_uv;

void main() {
    v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}

#pragma stage fragment

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_colour;

// AgX-flavoured filmic curve: shoulders off gently, keeps saturation in
// the highlights instead of hue-shifting everything bright towards
// white the way Reinhard does.
vec3 tonemap_filmic(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    // SAMPLING A RENDER TARGET NEEDS V FLIPPED.
    //
    // The full-screen triangle sets uv = ndc * 0.5 + 0.5, so uv.y = 0
    // is the BOTTOM of the picture -- but a render target's texel row
    // 0 is its TOP, on both backends, because both put their
    // framebuffer origin at the upper left. One flip, the same on
    // Vulkan and OpenGL, and the two cannot disagree about it.
    vec2 uv = vec2(v_uv.x, 1.0 - v_uv.y);
    vec3 hdr_colour = texture(hdr, uv).rgb * max(push.params.x, 0.0);
    vec3 mapped = tonemap_filmic(hdr_colour);
    // NO sRGB ENCODE HERE. The swapchain format does it.
    out_colour = vec4(mapped, 1.0);
}
