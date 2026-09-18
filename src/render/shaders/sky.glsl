// Manifold -- the sky.
//
// A procedural gradient with a sun, drawn as one triangle at the far
// plane after the opaque pass. Depth-tested but not depth-written, so
// it fills exactly the pixels nothing else reached -- including,
// because it is stencil-tested like everything else, the pixels inside
// a portal that nothing inside the portal reached.
#include "common.glsl"

#pragma stage vertex

layout(location = 0) out vec3 v_direction;

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vec2 ndc = uv * 2.0 - 1.0;
    // Unproject the far plane corner to get a world-space ray. Under
    // reverse-Z the far plane is z = 0.
    vec4 far_point = view.inv_proj * vec4(ndc, 0.0, 1.0);
    v_direction = mat3(view.inv_view) * (far_point.xyz / far_point.w);
    // Exactly at the far plane, so the depth test keeps it behind
    // everything that was drawn.
    gl_Position = vec4(ndc, 0.0, 1.0);
}

#pragma stage fragment

layout(location = 0) in vec3 v_direction;
layout(location = 0) out vec4 out_colour;

void main() {
    vec3 dir = normalize(v_direction);
    vec3 sun = normalize(frame.sun_direction.xyz);

    // A physically-flavoured gradient: more scattering towards the
    // horizon, warmer near the sun, darker overhead.
    float up = clamp(dir.y * 0.5 + 0.5, 0.0, 1.0);
    float horizon = pow(1.0 - abs(dir.y), 4.0);

    vec3 zenith = frame.ambient.rgb * 1.4;
    vec3 ground = frame.fog.rgb * 0.5;
    vec3 sky = mix(ground, zenith, smoothstep(0.42, 0.62, up));
    sky = mix(sky, frame.fog.rgb, horizon * 0.85);

    // Forward scattering around the sun, then the disc itself.
    float cos_sun = max(dot(dir, sun), 0.0);
    sky += frame.sun_colour.rgb * pow(cos_sun, 8.0) * 0.25;
    sky += frame.sun_colour.rgb * pow(cos_sun, 900.0) * 6.0;
    float disc = smoothstep(0.9993, 0.9997, cos_sun);
    sky += frame.sun_colour.rgb * disc * frame.sun_colour.a * 12.0;

    // Dither, because a smooth gradient in 8 bits bands visibly and
    // this is the cheapest fix there is.
    sky += (hash12(gl_FragCoord.xy) - 0.5) * (1.0 / 255.0);

    out_colour = vec4(sky, 1.0);
}
