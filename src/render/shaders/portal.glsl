// Warren -- the portal surface.
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
    vec4 edge_params;    // width, open, aspect, corner radius
} portal;

// THE APERTURE'S SHAPE, as a signed distance: negative inside.
//
// A rounded rectangle rather than a rectangle, because a hole in
// the world with four sharp corners reads as a decal stuck to
// the wall and one with rounded ones reads as a hole. The radius
// is a fraction of the shorter half-axis, so a square portal at
// 1.0 is a circle and the same number means the same thing at
// every aspect.
//
// The MARK pass discards on this, so the stencil -- and with it
// the hole the inner view is drawn through -- takes the same
// shape. Portal3D::within_aperture computes the identical
// function, because the hole the physics believes in and the
// hole you can see have to be the same hole.
float aperture_distance(vec2 uv, float aspect, float radius) {
    vec2 half_ext = vec2(max(aspect, 1e-3), 1.0);
    vec2 p = (uv - vec2(0.5)) * 2.0 * half_ext;
    float r = clamp(radius, 0.0, 1.0) * min(half_ext.x, half_ext.y);
    vec2 q = abs(p) - (half_ext - vec2(r));
    return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
}

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
    float aspect = max(portal.edge_params.z, 1e-3);
    float open = clamp(portal.edge_params.y, 0.0, 1.0);
    float dist = aperture_distance(v_uv, aspect, portal.edge_params.w);

    // OUTSIDE THE SHAPE IS NOT THE PORTAL. Discarding here is
    // what rounds the stencil, so the corners of the quad are
    // wall in the mark pass, in the restore pass and in the rim
    // -- three passes, one shape, no way for them to disagree.
    if (dist > 0.0) discard;

    // The rim rides the same distance field, so it is the same
    // thickness all the way round including through the curves.
    // The width is in half-axis units, matched to the old
    // rectangular behaviour.
    float w = max(portal.edge_params.x, 1e-4) * 2.0;
    float rim = smoothstep(-w, -w * 0.25, dist);
    out_colour = vec4(portal.edge_colour.rgb * (1.0 + 2.0 * open),
                      rim * (0.35 + 0.65 * open));
}
