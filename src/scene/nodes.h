// Manifold -- the nodes a scene is actually made of.
#pragma once

#include <vector>

#include "render/material.h"
#include "render/mesh.h"
#include "scene/node.h"

namespace mf {

// A CAMERA IS A TRANSFORM AND A PROJECTION MATRIX.
//
// Not a field of view and two clip planes. Those are a convenient way
// to ASK for a projection and this class offers them, but what the
// camera holds and what the renderer reads is the matrix -- because a
// portal's virtual camera needs one that no fov-and-near-plane API can
// describe. `set_custom_projection` is not an escape hatch bolted on
// the side; it is the plain form of what every other setter is a
// shortcut for.
class Camera3D : public Node3D {
    MF_CLASS(Camera3D, Node3D)

public:
    enum class Mode : uint8_t {
        Perspective,
        // No far plane. Under reverse-Z this has strictly better depth
        // precision than a finite one, so it is the right default for
        // anything outdoors.
        PerspectiveInfinite,
        Orthographic,
        Custom,
    };

    Camera3D() = default;

    Mode mode() const { return mode_; }
    void set_mode(Mode m) { mode_ = m; }

    float fov() const { return fov_; }
    void set_fov(float radians) { fov_ = clampf(radians, deg2rad(1.0f), deg2rad(179.0f)); }
    float fov_degrees() const { return rad2deg(fov_); }
    void set_fov_degrees(float d) { set_fov(deg2rad(d)); }

    // CLIP PLANES FOLLOW THE CAMERA'S SCALE.
    //
    // A camera parented to something that changed size -- a character
    // who walked through a portal into a larger one -- should see at
    // the same proportions it always did. Its near plane is five
    // centimetres of ITS world, not of the one it started in. Off for
    // a camera that must keep absolute clip distances.
    bool scale_clip_planes = true;
    float world_scale() const { return global_transform().basis.uniform_scale(); }

    float near_plane() const { return near_; }
    void set_near(float n) { near_ = n > 1e-4f ? n : 1e-4f; }
    float far_plane() const { return far_; }
    void set_far(float f) { far_ = f; }
    float ortho_height() const { return ortho_height_; }
    void set_ortho_height(float h) { ortho_height_ = h > 1e-4f ? h : 1e-4f; }
    // Off-centre projection, in units of half-extent. Lens shift, and
    // how a sub-rectangle of the screen gets its own camera.
    Vec2 frustum_offset() const { return offset_; }
    void set_frustum_offset(const Vec2 &o) { offset_ = o; }

    // The matrix, built from whichever mode is set -- or the one that
    // was handed over, if any.
    Projection projection(float aspect) const;
    void set_custom_projection(const Projection &p) {
        custom_ = p;
        mode_ = Mode::Custom;
    }
    const Projection &custom_projection() const { return custom_; }

    // World to view. The inverse of the global transform, with the
    // scale removed: a camera inside a scaled subtree should see the
    // world at its true size, and a scaled view matrix is a fisheye.
    Transform3D view_matrix() const;

    uint32_t cull_mask() const { return cull_mask_; }
    void set_cull_mask(uint32_t m) { cull_mask_ = m; }
    bool current() const { return current_; }
    void make_current();
    // A CAMERA THAT LEAVES THE TREE STOPS BEING THE ACTIVE ONE.
    //
    // The tree holds the active camera as a raw pointer, and
    // replacing a scene frees the camera along with everything else
    // under it -- leaving the renderer and the audio system reading
    // a dead node on the next frame. Clearing it here rather than
    // where the scene is replaced means it is also right for a
    // camera deleted on its own, or moved to another tree.
    void on_exit_tree() override;
    void on_enter_tree() override;

    // A ray through a point in [0,1] screen space, in world space.
    void screen_ray(const Vec2 &screen_uv, float aspect, Vec3 *origin,
                    Vec3 *direction) const;
    // And back: where a world point lands, in [0,1], with w < 0 for
    // anything behind the camera.
    Vec3 world_to_screen(const Vec3 &world, float aspect, bool *in_front) const;
    // The same, for scripts and for anything that does not want an out
    // parameter: z is the reversed depth, and z <= 0 means the point is
    // at or beyond the far plane or behind the camera.
    Vec3 project_point(const Vec3 &world, float aspect) const;

private:
    Mode mode_ = Mode::Perspective;
    float fov_ = deg2rad(70.0f);
    float near_ = 0.05f;
    float far_ = 4000.0f;
    float ortho_height_ = 10.0f;
    Vec2 offset_{0, 0};
    Projection custom_ = Projection::identity();
    uint32_t cull_mask_ = 0xFFFFFFFF;
    bool current_ = false;
};

// Geometry in the world.
class MeshInstance3D : public Node3D {
    MF_CLASS(MeshInstance3D, Node3D)

public:
    MeshInstance3D() = default;

    Ref<Mesh> mesh;
    // One per submesh. A missing or null entry falls back to the
    // engine's default material rather than dropping the draw.
    std::vector<Ref<Material>> materials;

    bool cast_shadows = true;
    bool receive_shadows = true;
    Color tint = Color::white();

    void set_mesh(Mesh *m) { mesh = Ref<Mesh>(m); }
    Mesh *get_mesh() const { return mesh.get(); }
    void set_material(int slot, Material *m);
    Material *first_material() const {
        return materials.empty() ? nullptr : materials[0].get();
    }
    void set_first_material(Material *m) { set_material(0, m); }
    Material *get_material(int slot) const;
    int material_count() const { return int(materials.size()); }

    // In world space, for culling.
    AABB world_bounds() const;
};

// --------------------------------------------------------------- lights

class Light3D : public Node3D {
    MF_CLASS(Light3D, Node3D)

public:
    Color colour = Color::white();
    // Directional lights are in lux, punctual ones in candela. Keeping
    // them in real units means a scene lit for one time of day does not
    // need every light re-tuned for another.
    float energy = 1.0f;
    bool shadows = true;
    float shadow_bias = 0.02f;
    float shadow_normal_bias = 1.5f;
};

class DirectionalLight3D : public Light3D {
    MF_CLASS(DirectionalLight3D, Light3D)

public:
    DirectionalLight3D() { energy = 4.0f; }
    // How far the shadow cascades reach. Beyond this there are no
    // shadows, which for a sun is a quality setting and not a bug.
    float shadow_distance = 120.0f;
    // Where each cascade ends, as a fraction of shadow_distance.
    // Logarithmic by default, which is what matches a perspective
    // camera's own distribution.
    Vec4 cascade_splits{0.06f, 0.16f, 0.38f, 1.0f};
    int cascade_count = 4;
    // The direction light travels: the node's own forward.
    Vec3 direction() const { return forward(); }
};

class OmniLight3D : public Light3D {
    MF_CLASS(OmniLight3D, Light3D)

public:
    float range = 10.0f;
    // Physical falloff is inverse square; this scales the cut-off so a
    // light can be made to end sooner without changing its brightness.
    float attenuation = 1.0f;
    float radius = 0.05f;  // a soft source rather than a point
};

class SpotLight3D : public OmniLight3D {
    MF_CLASS(SpotLight3D, OmniLight3D)

public:
    float angle = deg2rad(35.0f);
    float angle_softness = 0.2f;
};

}  // namespace mf
