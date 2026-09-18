#include "nodes.h"

#include "core/log.h"
#include "scene/scene_tree.h"

namespace mf {

// --------------------------------------------------------------- Camera3D

Projection Camera3D::projection(float aspect) const {
    if (aspect <= 0.0f) aspect = 1.0f;
    const float k = scale_clip_planes ? std::max(world_scale(), 1e-4f) : 1.0f;
    const float near_ = this->near_ * k;
    const float far_ = this->far_ * k;
    const float ortho_height_ = this->ortho_height_ * k;
    switch (mode_) {
        case Mode::Custom:
            return custom_;
        case Mode::Orthographic: {
            float h = ortho_height_ * 0.5f;
            float w = h * aspect;
            return Projection::orthographic(-w + offset_.x * w, w + offset_.x * w,
                                            -h + offset_.y * h, h + offset_.y * h,
                                            near_, far_);
        }
        case Mode::PerspectiveInfinite:
            // No off-axis variant of the infinite projection yet; a
            // shifted lens on an infinite camera is a vanishingly rare
            // combination and would be silently wrong, so say so.
            if (offset_ != Vec2()) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    MF_WARN("Camera3D: frustum_offset is ignored on an infinite "
                            "projection");
                }
            }
            return Projection::perspective_infinite(fov_, aspect, near_);
        case Mode::Perspective:
        default:
            return offset_ == Vec2()
                       ? Projection::perspective(fov_, aspect, near_, far_)
                       : Projection::perspective_offset(fov_, aspect, near_, far_,
                                                        offset_);
    }
}

Transform3D Camera3D::view_matrix() const {
    Transform3D g = global_transform();
    // THE SCALE COMES OUT.
    //
    // A camera under a scaled parent -- or, more to the point, a
    // portal's virtual camera, whose warp carries the ratio between two
    // apertures -- would otherwise scale the whole world in view space.
    // For a perspective projection a uniform view-space scale is
    // invisible, since it divides out; but the near and far planes are
    // in view-space units and would move, and an orthographic camera
    // would zoom. The position is what carries the portal's
    // magnification, and the position is untouched by this.
    g.basis = g.basis.orthonormalized();
    return g.inverse_orthonormal();
}

void Camera3D::make_current() {
    current_ = true;
    if (tree()) tree()->set_active_camera(this);
}

void Camera3D::on_exit_tree() {
    if (tree() && tree()->active_camera() == this)
        tree()->set_active_camera(nullptr);
}

void Camera3D::on_enter_tree() {
    // Symmetrically: a camera that was current before it was moved
    // is current again where it lands, so re-parenting a rig does
    // not silently switch the view to whatever else is around.
    if (current_ && tree()) tree()->set_active_camera(this);
}

void Camera3D::screen_ray(const Vec2 &screen_uv, float aspect, Vec3 *origin,
                          Vec3 *direction) const {
    // Screen UV has y down; NDC has y up.
    Vec2 ndc(screen_uv.x * 2.0f - 1.0f, 1.0f - screen_uv.y * 2.0f);
    Projection p = projection(aspect);
    Vec3 o, d;
    p.ray_at(ndc, &o, &d);
    Transform3D g = global_transform();
    g.basis = g.basis.orthonormalized();
    if (origin) *origin = g.xform(o);
    if (direction) *direction = g.basis.xform(d).normalized();
}

Vec3 Camera3D::world_to_screen(const Vec3 &world, float aspect,
                               bool *in_front) const {
    Projection vp = projection(aspect) * to_projection(view_matrix());
    Vec4 clip = vp.xform(Vec4(world, 1.0f));
    if (in_front) *in_front = clip.w > 0.0f;
    if (std::fabs(clip.w) < EPS) return {};
    Vec3 ndc = clip.xyz() / clip.w;
    return {ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f, ndc.z};
}

Vec3 Camera3D::project_point(const Vec3 &world, float aspect) const {
    bool in_front = false;
    Vec3 s = world_to_screen(world, aspect, &in_front);
    if (!in_front) s.z = -1.0f;
    return s;
}

// ---------------------------------------------------------- MeshInstance3D

void MeshInstance3D::set_material(int slot, Material *m) {
    if (slot < 0) return;
    if (int(materials.size()) <= slot) materials.resize(size_t(slot) + 1);
    materials[size_t(slot)] = Ref<Material>(m);
}

Material *MeshInstance3D::get_material(int slot) const {
    if (slot < 0 || slot >= int(materials.size())) return nullptr;
    return materials[size_t(slot)].get();
}

AABB MeshInstance3D::world_bounds() const {
    if (!mesh) return {};
    return mesh->bounds().transformed(global_transform());
}

// ------------------------------------------------------------ reflection

static void register_scene_nodes() {
    ClassBuilder<Camera3D>()
        .prop("fov", &Camera3D::fov_degrees, &Camera3D::set_fov_degrees, "range:1,179")
        .prop("near", &Camera3D::near_plane, &Camera3D::set_near)
        .prop("far", &Camera3D::far_plane, &Camera3D::set_far)
        .prop("ortho_height", &Camera3D::ortho_height, &Camera3D::set_ortho_height)
        .prop("frustum_offset", &Camera3D::frustum_offset,
              &Camera3D::set_frustum_offset)
        .prop("cull_mask", &Camera3D::cull_mask, &Camera3D::set_cull_mask)
        .field("scale_clip_planes", &Camera3D::scale_clip_planes)
        .prop("mode", &Camera3D::mode, &Camera3D::set_mode)
        .prop_ro("current", &Camera3D::current)
        .method("make_current", &Camera3D::make_current)
        .method("get_projection", &Camera3D::projection)
        .method("set_custom_projection", &Camera3D::set_custom_projection).args("projection")
        .method("get_view_matrix", &Camera3D::view_matrix)
        .method("project_point", &Camera3D::project_point).args("world", "aspect");

    ClassBuilder<MeshInstance3D>()
        .prop("mesh", &MeshInstance3D::get_mesh, &MeshInstance3D::set_mesh)
        // Slot zero, as a property, so a scene file and an inspector
        // can reach the common case. A mesh with several material
        // slots still needs set_material, and a scene saving only
        // slot zero says so rather than pretending.
        .prop("material", &MeshInstance3D::first_material,
              &MeshInstance3D::set_first_material)
        .field("cast_shadows", &MeshInstance3D::cast_shadows)
        .field("receive_shadows", &MeshInstance3D::receive_shadows)
        .field("tint", &MeshInstance3D::tint)
        .method("set_material", &MeshInstance3D::set_material).args("slot", "material")
        .method("get_material", &MeshInstance3D::get_material).args("slot")
        .method("get_material_count", &MeshInstance3D::material_count)
        .method("get_world_bounds", &MeshInstance3D::world_bounds);

    ClassBuilder<Light3D>(false)
        .field("colour", &Light3D::colour)
        .field("energy", &Light3D::energy, "range:0,64")
        .field("shadows", &Light3D::shadows)
        .field("shadow_bias", &Light3D::shadow_bias)
        .field("shadow_normal_bias", &Light3D::shadow_normal_bias);

    ClassBuilder<DirectionalLight3D>()
        .field("shadow_distance", &DirectionalLight3D::shadow_distance)
        .field("cascade_splits", &DirectionalLight3D::cascade_splits)
        .field("cascade_count", &DirectionalLight3D::cascade_count, "range:1,4")
        .method("direction", &DirectionalLight3D::direction);

    ClassBuilder<OmniLight3D>()
        .field("range", &OmniLight3D::range)
        .field("attenuation", &OmniLight3D::attenuation)
        .field("radius", &OmniLight3D::radius);

    ClassBuilder<SpotLight3D>()
        .field("angle", &SpotLight3D::angle)
        .field("angle_softness", &SpotLight3D::angle_softness);
}
MF_REGISTER(register_scene_nodes)

}  // namespace mf
