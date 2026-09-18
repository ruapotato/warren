// Warren -- the standard surface, on a body with bones in it.
//
// The same shading as mesh.glsl, and the same everything else: the
// only difference between a skinned draw and a static one is where
// the vertex ends up, so this file is a vertex stage and an include.
// See mesh_shade.glsl.
//
// THE BONE MATRICES ARE A STORAGE BUFFER, NOT A UNIFORM ARRAY.
//
// A uniform array has to be sized at compile time, which means
// picking a maximum bone count and paying for it on every draw; a
// rig with nineteen bones would upload the space for a hundred and
// twenty-eight. A storage buffer holds every body in the frame end to
// end and each draw says where its own run begins -- so twenty
// shamblers are one buffer, one upload and no per-draw binding at all.
#include "mesh_shade.glsl"

#pragma stage vertex

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv;
layout(location = 4) in vec4 in_colour;
// The second stream, present only on a skinned mesh.
//
// JOINTS ARE uvec4, NOT vec4. The buffer holds unsigned bytes and
// the attribute format says so, and Vulkan requires the shader's
// declared type to agree: a UINT format read as float is a pipeline
// the validation layer refuses to create, which is the right answer
// -- the two disagree about what the bits mean.
layout(location = 5) in uvec4 in_joints;
layout(location = 6) in vec4 in_weights;

layout(location = 0) out vec3 v_world;
layout(location = 1) out vec3 v_normal;
layout(location = 2) out vec4 v_tangent;
layout(location = 3) out vec2 v_uv;
layout(location = 4) out vec4 v_colour;

// Three rows of four: a bone matrix has no projective part and
// storing the fourth row costs a quarter of the bandwidth for a row
// of (0,0,0,1). See Renderer::upload_bones.
struct BoneRows { vec4 r0; vec4 r1; vec4 r2; };

// SET_DRAW, which nothing used until now and which exists for exactly
// this: a resource that changes per draw rather than per frame, view
// or material. The frame set's eight slots were full.
layout(set = SET_DRAW, binding = B_DRAW(0), std430) readonly buffer Bones {
    BoneRows bones[];
};

mat4 bone_matrix(uint i) {
    BoneRows b = bones[i];
    // Column-major out of three rows.
    return mat4(vec4(b.r0.x, b.r1.x, b.r2.x, 0.0),
                vec4(b.r0.y, b.r1.y, b.r2.y, 0.0),
                vec4(b.r0.z, b.r1.z, b.r2.z, 0.0),
                vec4(b.r0.w, b.r1.w, b.r2.w, 1.0));
}

void main() {
    // Where this body's bones start in the shared buffer.
    uint base = uint(push.params.x + 0.5);

    // FOUR WEIGHTS, SUMMED AS MATRICES.
    //
    // Blending the matrices and transforming once is one transform
    // per vertex; transforming four times and blending the results is
    // four. They agree for the rigid part and differ for scale, and
    // no rig here scales a bone.
    mat4 m = bone_matrix(base + in_joints.x) * in_weights.x
           + bone_matrix(base + in_joints.y) * in_weights.y
           + bone_matrix(base + in_joints.z) * in_weights.z
           + bone_matrix(base + in_joints.w) * in_weights.w;

    vec4 posed = m * vec4(in_position, 1.0);
    vec4 world = push.model * posed;
    v_world = world.xyz;

    // THE NORMAL GOES THROUGH THE SAME BLEND, and through its 3x3
    // rather than through normal_basis: a skin matrix is not a
    // uniform scale even when every bone is, because a blend of two
    // rotations shortens. Normalising after is what fixes that, and
    // the shading is the only thing that reads it.
    mat3 sm = mat3(m);
    mat3 nm = normal_basis(push.model);
    v_normal = normalize(nm * (sm * in_normal));
    v_tangent = vec4(normalize(nm * (sm * in_tangent.xyz)), in_tangent.w);

    v_uv = in_uv * material.uv_transform.xy + material.uv_transform.zw;
    v_colour = in_colour * push.tint;
    gl_Position = view.view_proj * world;
}

#pragma stage fragment
