#include "material.h"

#include <algorithm>

#include "core/log.h"

namespace wr {

Material::~Material() { release(); }

void Material::release() {
    if (!device_) return;
    if (group_.valid()) device_->destroy(group_);
    if (ubo_.valid()) device_->destroy(ubo_);
    group_ = {};
    ubo_ = {};
    device_ = nullptr;
}

MaterialUniforms Material::uniforms() const {
    MaterialUniforms u;
    u.albedo = albedo.rgba();
    u.emissive = Vec4(emissive.rgb(), emissive_strength);
    u.params = Vec4(metallic, roughness, normal_scale, occlusion_strength);
    u.uv_transform = Vec4(uv_scale.x, uv_scale.y, uv_offset.x, uv_offset.y);
    u.flags = Vec4(pass == MaterialPass::AlphaCutout ? alpha_cutoff : 0.0f,
                   normal_map ? 1.0f : 0.0f, orm_map ? 1.0f : 0.0f,
                   unlit ? 1.0f : 0.0f);
    u.extra = Vec4(triplanar, std::max(triplanar_sharpness, 0.1f), 0.0f, 0.0f);
    return u;
}

bool Material::prepare(rhi::Device *dev, rhi::BindGroupLayoutH layout) {
    using namespace rhi;
    if (!dev) return false;
    if (device_ && device_ != dev) release();
    device_ = dev;

    // The four slots always resolve to something, so a shader never
    // samples an unbound texture -- which is a black screen on one
    // driver, garbage on another and a validation error on a third.
    Texture *maps[4] = {
        albedo_map ? albedo_map.get() : Texture::white(dev),
        normal_map ? normal_map.get() : Texture::flat_normal(dev),
        orm_map ? orm_map.get() : Texture::default_orm(dev),
        emissive_map ? emissive_map.get() : Texture::white(dev),
    };

    bool textures_changed = false;
    for (int i = 0; i < 4; i++)
        if (bound_[i] != maps[i]->handle()) {
            bound_[i] = maps[i]->handle();
            textures_changed = true;
        }

    if (!ubo_.valid()) {
        BufferDesc bd;
        bd.size = sizeof(MaterialUniforms);
        bd.usage = BufferUsage::Uniform;
        bd.access = MemoryAccess::CpuToGpu;
        bd.name = "material";
        ubo_ = dev->create_buffer(bd);
        dirty_ = true;
    }
    if (dirty_) {
        MaterialUniforms u = uniforms();
        dev->write_buffer(ubo_, &u, sizeof(u));
    }

    if (!group_.valid() || textures_changed || layout_ != layout) {
        BindGroupDesc gd;
        gd.layout = layout;
        gd.name = "material";
        BindGroupEntry e;
        e.binding = 16;
        e.buffer = ubo_;
        gd.entries.push_back(e);
        for (int i = 0; i < 4; i++) {
            BindGroupEntry t;
            t.binding = uint32_t(17 + i);
            t.texture = maps[i]->handle();
            t.sampler = maps[i]->sampler();
            gd.entries.push_back(t);
        }
        if (group_.valid() && layout_ == layout)
            dev->update_bind_group(group_, gd);
        else
            group_ = dev->create_bind_group(gd);
        layout_ = layout;
    }
    dirty_ = false;
    return group_.valid();
}

// Pass in the top bits so the renderer's sort puts opaque first, then
// cutout, then transparency, and within each groups identical pipelines
// together.
uint64_t Material::sort_key() const {
    uint64_t k = uint64_t(pass) << 56;
    k |= uint64_t(double_sided ? 1 : 0) << 55;
    k |= uint64_t(unlit ? 1 : 0) << 54;
    // The bind group's slot is a good proxy for "the same material".
    k |= uint64_t(group_.index & 0xFFFFFF) << 24;
    k |= uint64_t(bound_[0].index & 0xFFFF) << 8;
    return k;
}

Ref<Material> Material::make(const Color &a, float rough, float metal) {
    Ref<Material> m(new Material());
    m->albedo = a;
    m->roughness = rough;
    m->metallic = metal;
    return m;
}

static void register_material_class() {
    ClassBuilder<Material>()
        .field("albedo", &Material::albedo)
        .field("metallic", &Material::metallic, "range:0,1")
        .field("roughness", &Material::roughness, "range:0,1")
        .field("emissive", &Material::emissive)
        .field("emissive_strength", &Material::emissive_strength, "range:0,32")
        .field("normal_scale", &Material::normal_scale, "range:0,4")
        .field("occlusion_strength", &Material::occlusion_strength, "range:0,1")
        .field("uv_scale", &Material::uv_scale)
        .field("uv_offset", &Material::uv_offset)
        .field("alpha_cutoff", &Material::alpha_cutoff, "range:0,1")
        .field("unlit", &Material::unlit)
        .field("double_sided", &Material::double_sided)
        .field("cast_shadows", &Material::cast_shadows)
        .field("shader", &Material::shader)
        .method("touch", &Material::touch);
}
WR_REGISTER(register_material_class)

Ref<Material> Material::duplicate() const {
    Ref<Material> m = new Material();
    m->albedo = albedo;
    m->metallic = metallic;
    m->roughness = roughness;
    m->emissive = emissive;
    m->emissive_strength = emissive_strength;
    m->normal_scale = normal_scale;
    m->occlusion_strength = occlusion_strength;
    m->uv_scale = uv_scale;
    m->uv_offset = uv_offset;
    // The textures are shared, not copied: a texture is immutable
    // once uploaded, and duplicating one to change a material flag
    // would be a megabyte of waste per variant.
    m->albedo_map = albedo_map;
    m->normal_map = normal_map;
    m->orm_map = orm_map;
    m->emissive_map = emissive_map;
    m->pass = pass;
    m->alpha_cutoff = alpha_cutoff;
    m->unlit = unlit;
    m->double_sided = double_sided;
    m->cast_shadows = cast_shadows;
    m->shader = shader;
    // Deliberately NOT copied: the uniform buffer and bind group.
    // They belong to the original, and prepare() makes this one its
    // own on first use.
    return m;
}

}  // namespace wr
