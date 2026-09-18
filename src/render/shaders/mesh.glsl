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

float sample_shadow(vec3 world, vec3 n, vec3 l, float view_depth) {
    if (frame.cascade_texel[3] <= 0.0) return 1.0;   // no shadow pass ran
    int c = pick_cascade(view_depth);

    // NORMAL-OFFSET BIAS. Move the lookup along the surface normal
    // rather than pushing the recorded depth, which is what removes
    // acne at grazing angles without the peter-panning a depth bias
    // large enough to do the same job would cause. One texel of the
    // cascade being sampled, widened as the light gets more oblique,
    // because that is exactly how far a texel's worth of surface can
    // slope away underneath the sample.
    float n_dot_l = clamp(dot(n, l), 0.0, 1.0);
    float slope = clamp(1.0 - n_dot_l, 0.0, 1.0);
    vec3 p = world + n * frame.cascade_texel[c] * (1.0 + 2.0 * slope) * 1.4142;

    vec4 lc = frame.sun_view_proj[c] * vec4(p, 1.0);
    if (lc.w <= 0.0) return 1.0;
    lc /= lc.w;
    // SAMPLING A RENDER TARGET NEEDS V FLIPPED -- see the same note in
    // tonemap.glsl. The shadow map is a render target like any other:
    // texel row 0 is the TOP of it on both backends, while ndc.y = +1
    // is the top of the picture, so the two run opposite ways.
    vec2 uv = vec2(lc.x, -lc.y) * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 1.0;
    // Outside the cascade's depth range there is nothing recorded.
    if (lc.z <= 0.0 || lc.z >= 1.0) return 1.0;

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

// ------------------------------------------------------- punctual lights

// Every light whose froxel this fragment lands in. The loop is over
// what reaches the pixel, not over the level.
vec3 punctual(vec3 world, vec3 n, vec3 v, vec3 albedo, float metallic,
              float rough, float view_depth) {
    vec3 sum = vec3(0.0);
    if (frame.counts.x <= 0) return sum;

    int c = cluster_of(gl_FragCoord.xy, view_depth);
    uint n_lights = min(cluster_count[c], uint(CLUSTER_MAX_LIGHTS));
    for (uint i = 0u; i < n_lights; i++) {
        Light L = lights[light_index[uint(c) * uint(CLUSTER_MAX_LIGHTS) + i]];

        vec3 to_light = L.position_range.xyz - world;
        float dist_sq = dot(to_light, to_light);
        float range = L.position_range.w;
        if (dist_sq > range * range) continue;

        float atten = distance_attenuation(dist_sq, range, L.params.y);
        if (atten <= 0.0) continue;
        vec3 l = to_light * inversesqrt(max(dist_sq, 1e-12));

        // A spot is an omni with the cone taken out of it. Smoothed
        // between the inner and outer angles, so the edge is a falloff
        // rather than a cut.
        if (int(L.params.z) == LIGHT_SPOT) {
            float cd = dot(-l, L.direction_cone.xyz);
            float t = (cd - L.direction_cone.w) /
                      max(L.params.x - L.direction_cone.w, 1e-4);
            t = clamp(t, 0.0, 1.0);
            atten *= t * t;
            if (atten <= 0.0) continue;
        }

        sum += brdf(n, v, l, albedo, metallic, rough) * L.colour_energy.rgb *
               L.colour_energy.a * atten;
    }
    return sum;
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

    float shadow = sample_shadow(v_world, n, l, view_depth);
    vec3 lit = brdf(n, v, l, base.rgb, metallic, rough) *
               frame.sun_colour.rgb * frame.sun_colour.a * shadow;

    // Hemisphere ambient: sky above, bounced ground below. Cheap, and
    // enough to keep shadowed geometry readable until image-based
    // lighting lands.
    float up = n.y * 0.5 + 0.5;
    vec3 ambient = mix(frame.ambient.rgb * 0.35, frame.ambient.rgb, up) *
                   frame.ambient.a * ao * base.rgb * (1.0 - metallic * 0.6);

    vec3 colour = lit + punctual(v_world, n, v, base.rgb, metallic, rough,
                                 view_depth) +
                  ambient + emissive;

    // Height fog, applied in view space so it is the same through a
    // portal as around it.
    float fog_amount = 1.0 - exp(-view_depth * frame.fog.a);
    fog_amount *= exp(-max(v_world.y - frame.fog_params.x, 0.0) *
                      frame.fog_params.z);
    colour = mix(colour, frame.fog.rgb, clamp(fog_amount, 0.0, 1.0));

    out_colour = vec4(colour, base.a);
}
