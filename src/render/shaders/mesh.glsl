// Manifold -- the standard surface shader.
//
// Forward, physically based, one directional light with cascaded
// shadows plus clustered punctual lights. Forward rather than deferred
// because a portal is a stencilled region of the SAME framebuffer as
// everything around it: a deferred renderer would need a G-buffer per
// recursion level, or a way to tell which level wrote each pixel, and
// both cost more than the deferred lighting saves.
#include "common.glsl"

layout(set = SET_MATERIAL, binding = B_MATERIAL(0), std140) uniform MaterialData {
    vec4 albedo;
    vec4 emissive;          // rgb, a = strength
    vec4 params;            // metallic, roughness, normal_scale, occlusion
    vec4 uv_transform;      // scale.xy, offset.xy
    vec4 flags;             // alpha_cutoff, has_normal_map, has_orm, unlit
} material;

layout(set = SET_MATERIAL, binding = B_MATERIAL(1)) uniform sampler2D tex_albedo;
layout(set = SET_MATERIAL, binding = B_MATERIAL(2)) uniform sampler2D tex_normal;
layout(set = SET_MATERIAL, binding = B_MATERIAL(3)) uniform sampler2D tex_orm;
layout(set = SET_MATERIAL, binding = B_MATERIAL(4)) uniform sampler2D tex_emissive;

layout(set = SET_FRAME, binding = B_FRAME(1)) uniform sampler2DArrayShadow shadow_map;

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

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec4 v_tangent;
layout(location = 3) in vec2 v_uv;
layout(location = 4) in vec4 v_colour;

layout(location = 0) out vec4 out_colour;

// ------------------------------------------------------------ lighting

float d_ggx(float n_dot_h, float rough) {
    float a = rough * rough;
    float a2 = a * a;
    float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

float v_smith(float n_dot_v, float n_dot_l, float rough) {
    // Height-correlated Smith, the Hammon approximation -- one divide
    // and no square roots, and indistinguishable from the exact form.
    float a = rough * rough;
    float gv = n_dot_l * (n_dot_v * (1.0 - a) + a);
    float gl = n_dot_v * (n_dot_l * (1.0 - a) + a);
    return 0.5 / max(gv + gl, 1e-6);
}

vec3 f_schlick(vec3 f0, float v_dot_h) {
    float f = pow(1.0 - v_dot_h, 5.0);
    return f0 + (1.0 - f0) * f;
}

vec3 brdf(vec3 n, vec3 v, vec3 l, vec3 albedo, float metallic, float rough) {
    vec3 h = normalize(v + l);
    float n_dot_v = max(dot(n, v), 1e-4);
    float n_dot_l = max(dot(n, l), 0.0);
    float n_dot_h = max(dot(n, h), 0.0);
    float v_dot_h = max(dot(v, h), 0.0);

    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 diffuse = albedo * (1.0 - metallic) / PI;
    vec3 spec = f_schlick(f0, v_dot_h) * d_ggx(n_dot_h, rough) *
                v_smith(n_dot_v, n_dot_l, rough);
    return (diffuse + spec) * n_dot_l;
}

// ------------------------------------------------------------- shadows

int pick_cascade(float view_depth) {
    // cascade_splits holds the far distance of each cascade.
    for (int i = 0; i < 3; i++)
        if (view_depth < frame.cascade_splits[i]) return i;
    return 3;
}

float sample_shadow(vec3 world, vec3 n, float view_depth) {
    int c = pick_cascade(view_depth);
    // Normal-offset bias: move the lookup along the surface normal
    // rather than along the light, which removes peter-panning at
    // grazing angles without the acne that a constant depth bias
    // trades it for. The offset grows with the cascade's texel size.
    float texel_world = frame.cascade_splits[3] * (float(c) + 1.0) * 0.0015;
    vec3 p = world + n * texel_world * 1.5;

    vec4 lc = frame.sun_view_proj[c] * vec4(p, 1.0);
    lc /= lc.w;
    vec2 uv = lc.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 1.0;

    // 3x3 PCF. The comparison is GREATER_EQUAL because the whole engine
    // is reverse-Z, the shadow map included.
    float sum = 0.0;
    vec2 texel = 1.0 / vec2(textureSize(shadow_map, 0).xy);
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            sum += texture(shadow_map,
                           vec4(uv + vec2(x, y) * texel, float(c), lc.z));
    return sum / 9.0;
}

// ---------------------------------------------------------------- main

void main() {
    vec4 base = texture(tex_albedo, v_uv) * material.albedo * v_colour;
    if (material.flags.x > 0.0 && base.a < material.flags.x) discard;

    vec3 n = normalize(v_normal);
    if (material.flags.y > 0.5) {
        vec3 t = normalize(v_tangent.xyz - n * dot(n, v_tangent.xyz));
        vec3 b = cross(n, t) * v_tangent.w;
        vec3 tn = texture(tex_normal, v_uv).xyz * 2.0 - 1.0;
        tn.xy *= material.params.z;
        n = normalize(mat3(t, b, n) * tn);
    }
    // A back face lit as though it faced forward is black; flipping the
    // normal makes single-sided geometry survive being seen from
    // behind, which happens constantly through a portal.
    if (!gl_FrontFacing) n = -n;

    float metallic = material.params.x;
    float rough = material.params.y;
    float ao = 1.0;
    if (material.flags.z > 0.5) {
        vec3 orm = texture(tex_orm, v_uv).rgb;
        ao = mix(1.0, orm.r, material.params.w);
        rough *= orm.g;
        metallic *= orm.b;
    }
    rough = clamp(rough, 0.03, 1.0);

    vec3 emissive = texture(tex_emissive, v_uv).rgb * material.emissive.rgb *
                    material.emissive.a;

    if (material.flags.w > 0.5) {
        out_colour = vec4(base.rgb + emissive, base.a);
        return;
    }

    vec3 v = normalize(view.eye.xyz - v_world);
    vec3 l = normalize(frame.sun_direction.xyz);
    float view_depth = -(view.view * vec4(v_world, 1.0)).z;

    float shadow = sample_shadow(v_world, n, view_depth);
    vec3 lit = brdf(n, v, l, base.rgb, metallic, rough) *
               frame.sun_colour.rgb * frame.sun_colour.a * shadow;

    // Hemisphere ambient: sky above, bounced ground below. Cheap, and
    // enough to keep shadowed geometry readable until image-based
    // lighting lands.
    float up = n.y * 0.5 + 0.5;
    vec3 ambient = mix(frame.ambient.rgb * 0.35, frame.ambient.rgb, up) *
                   frame.ambient.a * ao * base.rgb * (1.0 - metallic * 0.6);

    vec3 colour = lit + ambient + emissive;

    // Height fog, applied in view space so it is the same through a
    // portal as around it.
    float fog_amount = 1.0 - exp(-view_depth * frame.fog.a);
    fog_amount *= exp(-max(v_world.y - frame.fog_params.x, 0.0) *
                      frame.fog_params.z);
    colour = mix(colour, frame.fog.rgb, clamp(fog_amount, 0.0, 1.0));

    out_colour = vec4(colour, base.a);
}
