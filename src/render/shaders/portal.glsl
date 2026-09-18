// Manifold -- the portal surface.
//
// The same quad is drawn three times per portal per recursion level,
// with three different pipelines and one shader:
//
//   1. MARK. Colour and depth writes off, depth test on, stencil
//      incremented where it passes. This is what says "the hole is
//      here, and it is not behind a wall".
//   2. RESTORE. After the inner view has been drawn, the quad is drawn
//      again with depth writes on to put the portal's own depth back,
//      and the stencil decremented to undo the mark. Without this,
//      anything drawn afterwards in the outer view sorts against the
//      inner room's depth instead of against the hole.
//   3. RIM. A thin lit border so the player can tell there is a portal
//      there at all, and which of the pair they are looking at.
//
// There is no view texture here, and that is the point: a stencilled
// portal composites nothing. The inner view is drawn straight into the
// same framebuffer, at full resolution, with the same anti-aliasing --
// so there is no seam to get wrong, no texture to size, and no
// resolution ladder to tune.
#include "common.glsl"

layout(set = SET_MATERIAL, binding = B_MATERIAL(0), std140) uniform PortalData {
    vec4 edge_colour;    // rgb, a = unused
    vec4 edge_params;    // width, open, aspect, unused
} portal;

#pragma stage vertex

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;
layout(location = 4) in vec4 in_colour;

layout(location = 0) out vec2 v_uv;

void main() {
    v_uv = in_uv;
    vec3 p = in_position;
    // WHILE OPENING, THE APERTURE GROWS FROM THE MIDDLE. A portal that
    // faded in would read as a decal; one that widens reads as a hole
    // being torn.
    float open = clamp(portal.edge_params.y, 0.0, 1.0);
    p.xy *= mix(0.02, 1.0, open * open);
    gl_Position = view.view_proj * push.model * vec4(p, 1.0);
}

#pragma stage fragment

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_colour;

void main() {
    // The mark and restore passes have colour writes masked off, so
    // what this returns only matters for the rim pass.
    vec2 d = abs(v_uv - vec2(0.5)) * 2.0;
    // Corrected for the aperture's aspect, so a wide portal's rim is
    // the same thickness all the way round instead of stretched.
    float aspect = max(portal.edge_params.z, 1e-3);
    d.x *= aspect;
    float edge = max(d.x / max(aspect, 1e-3), d.y);
    float w = max(portal.edge_params.x, 1e-4);
    float rim = smoothstep(1.0 - w, 1.0 - w * 0.25, edge);
    float open = clamp(portal.edge_params.y, 0.0, 1.0);
    out_colour = vec4(portal.edge_colour.rgb * (1.0 + 2.0 * open),
                      rim * (0.35 + 0.65 * open));
}
