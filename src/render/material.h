// Manifold -- what a surface is made of.
#pragma once

#include <string>

#include "core/object.h"
#include "render/texture.h"
#include "rhi/rhi.h"

namespace mf {

// Which pass a material belongs in. The renderer sorts by this first,
// because the order the three are drawn in is not negotiable: opaque
// front-to-back for early-Z, then the sky in the gaps, then
// transparency back-to-front over the top.
enum class MaterialPass : uint8_t { Opaque, AlphaCutout, Transparent };

// Exactly the bytes of MaterialData in mesh.glsl. Kept as a struct so
// the layout is checkable rather than assembled by hand at upload.
struct MaterialUniforms {
    Vec4 albedo{1, 1, 1, 1};
    Vec4 emissive{0, 0, 0, 0};       // rgb, a = strength
    Vec4 params{0, 1, 1, 1};         // metallic, roughness, normal scale, occlusion
    Vec4 uv_transform{1, 1, 0, 0};   // scale.xy, offset.xy
    Vec4 flags{0, 0, 0, 0};          // alpha cutoff, has normal, has orm, unlit
};
static_assert(sizeof(MaterialUniforms) == 80, "must match MaterialData");

class Material : public Object {
    MF_CLASS(Material, Object)

public:
    Material() = default;
    ~Material() override;

    // --- the surface -----------------------------------------------------
    Color albedo = Color::white();
    float metallic = 0.0f;
    float roughness = 0.8f;
    Color emissive = Color(0, 0, 0, 1);
    float emissive_strength = 0.0f;
    float normal_scale = 1.0f;
    float occlusion_strength = 1.0f;
    Vec2 uv_scale{1, 1};
    Vec2 uv_offset{0, 0};

    Ref<Texture> albedo_map;
    Ref<Texture> normal_map;
    // Occlusion in red, roughness in green, metallic in blue -- the
    // glTF packing, so an imported model needs no channel shuffling.
    Ref<Texture> orm_map;
    Ref<Texture> emissive_map;

    // --- how it is drawn ---------------------------------------------------
    MaterialPass pass = MaterialPass::Opaque;
    float alpha_cutoff = 0.5f;
    bool unlit = false;
    bool double_sided = false;
    bool cast_shadows = true;
    // A material may name a shader other than the standard one. Empty
    // means "mesh".
    std::string shader = "mesh";

    // --- gpu ----------------------------------------------------------------
    // Build or rebuild the uniform buffer and the bind group. Cheap to
    // call; it only does work when something actually changed.
    bool prepare(rhi::Device *dev, rhi::BindGroupLayoutH layout);
    rhi::BindGroupH bind_group() const { return group_; }
    // Mark dirty after changing anything above.
    void touch() { dirty_ = true; }
    void release();

    MaterialUniforms uniforms() const;
    // What the renderer sorts by: same key means the same pipeline and
    // the same bind group, so the two can be batched.
    uint64_t sort_key() const;

    static Ref<Material> make(const Color &albedo, float roughness = 0.8f,
                              float metallic = 0.0f);

private:
    rhi::Device *device_ = nullptr;
    rhi::BufferH ubo_;
    rhi::BindGroupH group_;
    rhi::BindGroupLayoutH layout_;
    bool dirty_ = true;
    // Remembered so a texture swap can be detected without comparing
    // every field.
    rhi::TextureH bound_[4];
};

}  // namespace mf
