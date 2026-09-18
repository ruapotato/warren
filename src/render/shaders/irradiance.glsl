// Warren -- the diffuse half of image-based lighting.
//
// Integrating incoming light over the hemisphere around a normal,
// weighted by the cosine, gives what a Lambertian surface facing that
// way receives. Done once into a small cubemap, a diffuse lookup
// afterwards is a single texture fetch.
//
// It replaces a hemisphere ambient term -- sky colour above, a darker
// version below -- which is a fair guess for an overcast day and
// wrong for every other one. With this, a wall facing a low sun is
// warm on that side and cold on the other because the sky actually is.
#include "sky_model.glsl"

layout(set = SET_MATERIAL, binding = B_MATERIAL(1)) uniform samplerCube env;

#pragma stage vertex

layout(location = 0) out vec2 v_st;

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_st = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}

#pragma stage fragment

layout(location = 0) in vec2 v_st;
layout(location = 0) out vec4 out_colour;

void main() {
    vec2 st = vec2(v_st.x, 1.0 - v_st.y);
    vec3 n = normalize(cube_direction(int(push.params.x), st));

    // An orthonormal basis around the normal. Choosing the up vector
    // by which axis n is least aligned with keeps the cross product
    // away from zero.
    vec3 up = abs(n.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
    vec3 t = normalize(cross(up, n));
    vec3 b = cross(n, t);

    // Uniform steps in phi and theta, cosine-weighted by the sin(t)
    // cos(t) factor. Coarse on purpose: the result is a 32-pixel
    // cubemap of very low frequency data, and the sample count is
    // what makes it a start-up cost rather than a frame cost.
    const float step_phi = 0.025;
    const float step_theta = 0.025;
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (float phi = 0.0; phi < 2.0 * PI; phi += step_phi) {
        float sp = sin(phi), cp = cos(phi);
        for (float theta = 0.0; theta < 0.5 * PI; theta += step_theta) {
            float st_ = sin(theta), ct = cos(theta);
            vec3 dir = t * (st_ * cp) + b * (st_ * sp) + n * ct;
            sum += textureLod(env, dir, 0.0).rgb * ct * st_;
            weight += ct * st_;
        }
    }
    out_colour = vec4(sum / max(weight, 1e-4), 1.0);
}
