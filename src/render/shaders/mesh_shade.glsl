// Warren -- the standard surface, shared by the skinned and unskinned
// programs.
//
// ONE COPY OF THE SHADING.
//
// A skinned mesh and a static one differ in exactly one thing: where
// the vertex ends up. Everything after that -- the material, the
// triplanar projection, the lighting, the shadows, the environment --
// is the same code, and the version of this engine that had two
// copies of it had two copies of every lighting bug as well. So the
// fragment stage lives here, both programs include it, and neither
// has a fragment stage of its own worth the name.
//
// No `#pragma stage` in this file: that is how gen_shaders tells an
// include from a program.
#ifndef WR_MESH_SHADE_GLSL
#define WR_MESH_SHADE_GLSL

#include "brdf.glsl"

layout(set = SET_MATERIAL, binding = B_MATERIAL(0), std140) uniform MaterialData {
    vec4 albedo;
    vec4 emissive;          // rgb, a = strength
    vec4 params;            // metallic, roughness, normal_scale, occlusion
    vec4 uv_transform;      // scale.xy, offset.xy
    vec4 flags;             // alpha_cutoff, has_normal_map, has_orm, unlit
    vec4 extra;             // triplanar scale (0 = off), sharpness, -, -
} material;

layout(set = SET_MATERIAL, binding = B_MATERIAL(1)) uniform sampler2D tex_albedo;
layout(set = SET_MATERIAL, binding = B_MATERIAL(2)) uniform sampler2D tex_normal;
layout(set = SET_MATERIAL, binding = B_MATERIAL(3)) uniform sampler2D tex_orm;
layout(set = SET_MATERIAL, binding = B_MATERIAL(4)) uniform sampler2D tex_emissive;

layout(set = SET_FRAME, binding = B_FRAME(1)) uniform sampler2DArrayShadow shadow_map;


// EVERYTHING BELOW IS THE FRAGMENT STAGE, and it has to say so.
//
// gen_shaders prepends a file's preamble to EVERY stage it compiles,
// and this whole file is preamble as far as the two programs that
// include it are concerned. Unguarded, the vertex stage gets the
// fragment's inputs, its outputs and its gl_FragCoord, and fails to
// compile with four errors that name none of that.
#if defined(WR_STAGE_FRAGMENT)

layout(location = 0) in vec3 v_world;
layout(location = 1) in vec3 v_normal;
layout(location = 2) in vec4 v_tangent;
layout(location = 3) in vec2 v_uv;
layout(location = 4) in vec4 v_colour;

layout(location = 0) out vec4 out_colour;

// The reflectance model lives in brdf.glsl, shared with the
// environment prefilter so the two cannot disagree.

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

// ------------------------------------------ shadows for punctual lights

// How much of light L reaches `world`. 1 when it casts no shadow.
//
// AN OMNI NEEDS NO MATRIX HERE, and that is the whole reason this is
// short. Its six faces were rendered with 90-degree frusta sharing
// one near and one far, so the depth stored for a texel is a function
// of the distance along that face's dominant axis and nothing else --
// which the fragment can work out for itself. Only a spot, whose
// frustum is its own shape, carries a matrix.
float shadow_punctual(uint li, vec3 world, vec3 n, vec3 l) {
    float near = lights[li].params.w;
    if (near <= 0.0) return 1.0;            // no tile was allocated

    // Normal-offset again: the lookup moves along the surface rather
    // than the stored depth moving away from it, for the same reason
    // as the cascades.
    float n_dot_l = clamp(dot(n, l), 0.0, 1.0);
    vec3 p = world + n * (1.0 - n_dot_l) * 0.06 + n * 0.015;

    // ONE PATH FOR BOTH. A spot is face 0 of a light that has one
    // face; an omni picks the face its dominant axis lands in.
    // Either way the matrix is the one the CPU rendered that tile
    // with, so there is nothing here that can disagree with the pass
    // -- which is not true of deriving a face's basis from a forward
    // and an up vector, and the seam that produces reads as a bias
    // problem rather than as the mirrored face it is.
    int face = int(lights[li].params.z) == LIGHT_SPOT
                   ? 0
                   : cube_face_of(p - lights[li].position_range.xyz);
    vec4 c = lights[li].shadow_view_proj[face] * vec4(p, 1.0);
    if (c.w <= 0.0) return 1.0;
    c /= c.w;
    // Sampling a render target: v runs the other way. The same flip,
    // for the same reason, as every other lookup into something the
    // engine drew.
    vec2 uv = vec2(c.x, -c.y) * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 1.0;
    float ref = c.z;
    if (ref <= 0.0 || ref >= 1.0) return 1.0;

    // 2x2 PCF. Punctual shadows are small on screen and a wider
    // kernel costs more than it shows.
    vec2 texel = 1.0 / vec2(textureSize(shadow_atlas, 0));
    float sum = 0.0;
    for (int y = 0; y <= 1; y++)
        for (int x = 0; x <= 1; x++) {
            vec2 a = atlas_uv(lights[li].shadow.x, float(face),
                              lights[li].shadow.y, lights[li].shadow.z, uv);
            sum += texture(shadow_atlas,
                           vec3(a + (vec2(x, y) - 0.5) * texel, ref));
        }
    return sum * 0.25;
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
        // FIELD BY FIELD, NOT `Light L = lights[k]`.
        //
        // The struct carries six matrices for the cube faces, so a
        // copy is 464 bytes pulled through the cache for every light
        // touching every pixel -- which cost this scene 177 fps down
        // to 49 the moment the matrices were added. Read what is
        // needed and the shadow lookup reads the one matrix it wants.
        uint k = light_index[uint(c) * uint(CLUSTER_MAX_LIGHTS) + i];

        vec4 pos_range = lights[k].position_range;
        vec3 to_light = pos_range.xyz - world;
        float dist_sq = dot(to_light, to_light);
        float range = pos_range.w;
        if (dist_sq > range * range) continue;

        vec4 params = lights[k].params;
        float atten = distance_attenuation(dist_sq, range, params.y);
        if (atten <= 0.0) continue;
        vec3 l = to_light * inversesqrt(max(dist_sq, 1e-12));

        // A spot is an omni with the cone taken out of it. Smoothed
        // between the inner and outer angles, so the edge is a falloff
        // rather than a cut.
        if (int(params.z) == LIGHT_SPOT) {
            vec4 cone = lights[k].direction_cone;
            float cd = dot(-l, cone.xyz);
            float t = (cd - cone.w) / max(params.x - cone.w, 1e-4);
            t = clamp(t, 0.0, 1.0);
            atten *= t * t;
            if (atten <= 0.0) continue;
        }

        if (params.w > 0.0) {
            atten *= shadow_punctual(k, world, n, l);
            if (atten <= 0.0) continue;
        }

        vec4 ce = lights[k].colour_energy;
        sum += brdf(n, v, l, albedo, metallic, rough) * ce.rgb * ce.a * atten;
    }
    return sum;
}

// ------------------------------------------------------- triplanar
//
// WHY A SURFACE SHADER NEEDS THIS.
//
// A texture needs UVs, and UVs need someone to unwrap the mesh. A
// voxel chunk has no unwrapper and neither does a shape contoured
// from a formula -- they are geometry that appeared without an
// artist, which is the entire point of both. Projecting the texture
// from three directions and blending by the normal needs no UVs at
// all: the price is three samples instead of one, and a slight
// swim on surfaces at forty-five degrees.
//
// Blended by the normal raised to a power, so a face that clearly
// points one way takes that projection alone and only the corners
// mix.
vec3 triplanar_weights(vec3 n) {
    vec3 w = pow(abs(n), vec3(material.extra.y));
    return w / max(w.x + w.y + w.z, 1e-5);
}

vec4 triplanar_sample(sampler2D tex, vec3 world, vec3 w, float scale) {
    // Each plane is mirrored on the side facing away, which keeps
    // the projection from running backwards across a shape.
    return texture(tex, world.zy * scale) * w.x +
           texture(tex, world.xz * scale) * w.y +
           texture(tex, world.xy * scale) * w.z;
}

// The normal map has to be reoriented, not just blended: a tangent
// normal read off the XZ plane means something different from the
// same bytes read off the XY plane. Whiteout blending -- add the
// plane's own axis into the sampled normal's z and swizzle into
// world space -- is the cheap version that does not need a tangent
// frame, which contoured geometry does not reliably have.
vec3 triplanar_normal(vec3 world, vec3 n, vec3 w, float scale, float strength) {
    vec3 nx = texture(tex_normal, world.zy * scale).xyz * 2.0 - 1.0;
    vec3 ny = texture(tex_normal, world.xz * scale).xyz * 2.0 - 1.0;
    vec3 nz = texture(tex_normal, world.xy * scale).xyz * 2.0 - 1.0;
    nx.xy *= strength;
    ny.xy *= strength;
    nz.xy *= strength;
    vec3 an = abs(n);
    nx = vec3(nx.z * sign(n.x), nx.y, nx.x);
    ny = vec3(ny.x, ny.z * sign(n.y), ny.y);
    nz = vec3(nz.x, nz.y, nz.z * sign(n.z));
    return normalize(nx * w.x + ny * w.y + nz * w.z + n * 0.001);
}

// ---------------------------------------------------------------- main

void main() {
    float tri = material.extra.x;
    vec3 tri_w = tri > 0.0 ? triplanar_weights(normalize(v_normal)) : vec3(0.0);

    vec4 base = (tri > 0.0 ? triplanar_sample(tex_albedo, v_world, tri_w, tri)
                           : texture(tex_albedo, v_uv)) *
                material.albedo * v_colour;
    if (material.flags.x > 0.0 && base.a < material.flags.x) discard;

    vec3 n = normalize(v_normal);
    if (material.flags.y > 0.5) {
        if (tri > 0.0) {
            n = triplanar_normal(v_world, n, tri_w, tri, material.params.z);
        } else {
            vec3 t = normalize(v_tangent.xyz - n * dot(n, v_tangent.xyz));
            vec3 b = cross(n, t) * v_tangent.w;
            vec3 tn = texture(tex_normal, v_uv).xyz * 2.0 - 1.0;
            tn.xy *= material.params.z;
            n = normalize(mat3(t, b, n) * tn);
        }
    }
    // A back face lit as though it faced forward is black; flipping the
    // normal makes single-sided geometry survive being seen from
    // behind, which happens constantly through a portal.
    if (!gl_FrontFacing) n = -n;

    float metallic = material.params.x;
    float rough = material.params.y;
    float ao = 1.0;
    if (material.flags.z > 0.5) {
        vec3 orm = (tri > 0.0 ? triplanar_sample(tex_orm, v_world, tri_w, tri)
                              : texture(tex_orm, v_uv)).rgb;
        ao = mix(1.0, orm.r, material.params.w);
        rough *= orm.g;
        metallic *= orm.b;
    }
    rough = clamp(rough, 0.03, 1.0);

    vec3 emissive = (tri > 0.0 ? triplanar_sample(tex_emissive, v_world, tri_w, tri)
                               : texture(tex_emissive, v_uv))
                        .rgb *
                    material.emissive.rgb * material.emissive.a;

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

    // ------------------------------------------------ the environment
    //
    // Split-sum image-based lighting: a cosine-convolved cube for the
    // diffuse half and a GGX-prefiltered one for the specular, with
    // the second factor of the split solved analytically rather than
    // read from a lookup table (see env_brdf).
    vec3 ambient;
    if (frame.env.x > 0.0) {
        float n_dot_v = max(dot(n, v), 1e-4);
        vec3 f0 = mix(vec3(0.04), base.rgb, metallic);
        vec3 f = f_schlick_roughness(f0, n_dot_v, rough);

        // Energy that is not reflected is available to scatter, and
        // none of it is inside a metal.
        vec3 kd = (1.0 - f) * (1.0 - metallic);
        vec3 diffuse = texture(env_irradiance, n).rgb * base.rgb * kd;

        // The mip IS the roughness. reflect() wants the incident
        // direction, which is -v.
        vec3 r = reflect(-v, n);
        float lod = rough * (frame.env.x - 1.0);
        vec3 prefiltered = textureLod(env_specular, r, lod).rgb;
        vec2 ab = env_brdf(n_dot_v, rough);
        vec3 specular = prefiltered * (f * ab.x + ab.y);

        ambient = (diffuse + specular) * frame.env.y * ao;
    } else {
        // No environment baked: a hemisphere ambient, sky above and
        // bounced ground below. A fair guess for an overcast day and
        // wrong for every other one, which is why the cubemaps exist.
        float up = n.y * 0.5 + 0.5;
        ambient = mix(frame.ambient.rgb * 0.35, frame.ambient.rgb, up) *
                  frame.ambient.a * ao * base.rgb * (1.0 - metallic * 0.6);
    }

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

#endif  // WR_STAGE_FRAGMENT

#endif  // WR_MESH_SHADE_GLSL
