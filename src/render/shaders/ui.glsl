// Warren -- the editor's geometry.
//
// One pipeline, one texture, one triangle list. Everything the UI
// draws -- a panel, a slider, a letter -- is a coloured quad, and the
// font atlas has a white texel in it so a solid rectangle and a glyph
// are the same draw call. The whole editor is therefore one bind and
// one indexed draw per scissor rectangle.
//
// DELIBERATELY NOT INCLUDING common.glsl. The scene's binding scheme
// exists so that a frame, a view, a material and a draw can be bound
// independently at different rates; the UI has one of each for ever,
// and inheriting three descriptor sets to use one texture would mean
// binding two empty ones on every panel. It declares the push block
// it needs and nothing else.

layout(push_constant, std430) uniform Push {
    mat4 unused_model;
    vec4 tint;
    vec4 params;            // framebuffer width, height
} push;

layout(set = 0, binding = 0) uniform sampler2D ui_atlas;

#pragma stage vertex

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_colour;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_colour;

void main() {
    v_uv = in_uv;
    v_colour = in_colour;
    // Pixels to clip space, with y down: the UI thinks in screen
    // coordinates, like every other rectangle in this engine.
    vec2 ndc = in_position / push.params.xy * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}

#pragma stage fragment

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_colour;
layout(location = 0) out vec4 out_colour;

float to_linear(float c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

void main() {
    // The theme's colours are sRGB and the target is linear HDR, so
    // they have to be converted or every panel comes out washed out
    // against the scene behind it.
    vec4 c = v_colour * texture(ui_atlas, v_uv);
    out_colour = vec4(to_linear(c.r), to_linear(c.g), to_linear(c.b), c.a);
}
