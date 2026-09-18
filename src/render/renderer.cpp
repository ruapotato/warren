#include "renderer.h"

#include "stb/stb_image_write.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "platform/window.h"
#include "render/shaders/generated/shaders.h"
#include "scene/animated.h"
#include "scene/scene_tree.h"

namespace wr {
namespace {

using namespace rhi;

// Matches FrameData in common.glsl.
struct FrameUniforms {
    Vec4 time;
    Vec4 sun_direction;
    Vec4 sun_colour;
    Vec4 ambient;
    Vec4 fog;
    Vec4 fog_params;
    Projection sun_view_proj[4];
    Vec4 cascade_splits;
    Vec4 cascade_texel;
    Vec4 screen;
    int32_t counts[4];
    Vec4 env;
};

// Matches ViewData.
struct ViewUniforms {
    Projection view;
    Projection proj;
    Projection view_proj;
    Projection inv_view;
    Projection inv_proj;
    Vec4 eye;
    Vec4 near_far;
    int32_t portal[4];
    Vec4 cluster;
};

// Matches PortalData in portal.glsl.
struct PortalUniforms {
    Vec4 edge_colour;
    Vec4 edge_params;
};

// Matches the Push block.
struct PushUniforms {
    Projection model;
    Vec4 tint;
    Vec4 params;
};
static_assert(sizeof(PushUniforms) == 96, "push constants must fit the budget");

constexpr uint32_t kViewRing = 128;    // views per frame, across all recursion
constexpr uint32_t kPortalRing = 128;

// Must match common.glsl. A mismatch is silent and looks like lights
// that flicker on and off as the camera turns, so it is asserted where
// the grid is built.
constexpr int kClusterX = 16;
constexpr int kClusterY = 9;
constexpr int kClusterZ = 24;
constexpr int kClusterCount = kClusterX * kClusterY * kClusterZ;
constexpr int kClusterMaxLights = 8;
// The far end of the froxel grid. Beyond it everything lands in the
// last slice, which is correct but coarse -- and punctual lights that
// reach further than this are not punctual lights.
constexpr float kClusterFar = 400.0f;

uint32_t align_up(uint32_t v, uint32_t a) { return a ? ((v + a - 1) / a) * a : v; }

// A sphere-versus-frustum test, which is all the culling an AABB needs
// to be worth: the exact box test costs three times as much and
// rejects a few percent more.
bool visible_in(const Plane frustum[6], const AABB &box) {
    if (!box.valid()) return true;
    Vec3 c = box.center();
    float r = box.radius();
    for (int i = 0; i < 6; i++)
        if (frustum[i].distance_to(c) < -r) return false;
    return true;
}

}  // namespace

Renderer::~Renderer() { shutdown(); }

// ------------------------------------------------------------------ init

bool Renderer::init(rhi::Device *dev, const RenderSettings &s) {
    device_ = dev;
    settings_ = s;
    samples_ = uint32_t(std::max(1, s.msaa));
    if (samples_ > dev->caps().max_samples) {
        WR_WARN("renderer: %ux MSAA asked for, %ux available", samples_,
                dev->caps().max_samples);
        samples_ = dev->caps().max_samples;
    }

    const uint32_t ubo_align = std::max(16u, dev->caps().uniform_buffer_alignment);
    view_stride_ = align_up(uint32_t(sizeof(ViewUniforms)), ubo_align);
    portal_stride_ = align_up(uint32_t(sizeof(PortalUniforms)), ubo_align);

    // --- bind group layouts
    BindGroupLayoutDesc fl;
    fl.entries.push_back({0, BindingType::UniformBuffer, true, true, false, 1});
    fl.entries.push_back({1, BindingType::SampledTexture, false, true, false, 1});
    fl.name = "frame";
    // The clustered light data: the lights themselves, the froxel
    // counts and the index list. Storage buffers rather than uniform
    // ones because the sizes are decided by the scene, and read in the
    // fragment stage only.
    for (uint32_t b = 2; b <= 4; b++)
        fl.entries.push_back({b, BindingType::StorageBuffer, false, true, false, 1});
    // The baked environment: irradiance, then prefiltered specular.
    fl.entries.push_back({5, BindingType::SampledTexture, false, true, false, 1});
    fl.entries.push_back({6, BindingType::SampledTexture, false, true, false, 1});
    fl.entries.push_back({7, BindingType::SampledTexture, false, true, false, 1});
    frame_layout_ = dev->create_bind_group_layout(fl);

    // The bake's own set: one cubemap in, one face out. It borrows
    // the material slot so the prefilter shaders can be written
    // against a binding the standard layout already defines.
    // THE BONES. One storage buffer holding every skinned body in the
    // frame; a draw says where its own run starts through a push
    // constant. See mesh_skinned.glsl.
    BindGroupLayoutDesc bl;
    bl.entries.push_back({24, BindingType::StorageBuffer, true, false, false, 1});
    bl.name = "bones";
    bone_layout_ = dev->create_bind_group_layout(bl);

    BindGroupLayoutDesc el;
    el.entries.push_back({17, BindingType::SampledTexture, false, true, false, 1});
    el.name = "environment source";
    env_layout_ = dev->create_bind_group_layout(el);

    BindGroupLayoutDesc vl;
    vl.entries.push_back({8, BindingType::UniformBufferDynamic, true, true, false, 1});
    vl.name = "view";
    view_layout_ = dev->create_bind_group_layout(vl);

    BindGroupLayoutDesc ml;
    ml.entries.push_back({16, BindingType::UniformBuffer, true, true, false, 1});
    for (uint32_t i = 17; i <= 20; i++)
        ml.entries.push_back({i, BindingType::SampledTexture, false, true, false, 1});
    ml.name = "material";
    material_layout_ = dev->create_bind_group_layout(ml);

    BindGroupLayoutDesc pl;
    pl.entries.push_back({16, BindingType::UniformBufferDynamic, true, true, false, 1});
    pl.name = "portal";
    portal_layout_ = dev->create_bind_group_layout(pl);

    BindGroupLayoutDesc tl;
    tl.entries.push_back({16, BindingType::SampledTexture, false, true, false, 1});
    tl.name = "tonemap";
    tonemap_layout_ = dev->create_bind_group_layout(tl);

    // --- uniform buffers
    BufferDesc bd;
    bd.usage = BufferUsage::Uniform;
    bd.access = MemoryAccess::CpuToGpu;
    bd.size = align_up(uint32_t(sizeof(FrameUniforms)), ubo_align);
    bd.name = "frame ubo";
    frame_ubo_ = dev->create_buffer(bd);
    bd.size = uint64_t(view_stride_) * kViewRing;
    bd.name = "view ring";
    view_ubo_ = dev->create_buffer(bd);
    bd.size = uint64_t(portal_stride_) * kPortalRing;
    bd.name = "portal ring";
    portal_ubo_ = dev->create_buffer(bd);

    {
        const int slots = std::max(1, settings_.max_clustered_views);
        cluster_counts_.assign(size_t(slots) * kClusterCount, 0u);
        cluster_indices_.assign(
            size_t(slots) * kClusterCount * kClusterMaxLights, 0u);
        BufferDesc lb;
        lb.usage = BufferUsage::Storage;
        lb.access = MemoryAccess::CpuToGpu;
        lb.size = uint64_t(std::max(1, settings_.max_lights)) * sizeof(LightGpu);
        lb.name = "lights";
        light_buffer_ = dev->create_buffer(lb);
        lb.size = cluster_counts_.size() * sizeof(uint32_t);
        lb.name = "light clusters";
        cluster_buffer_ = dev->create_buffer(lb);
        lb.size = cluster_indices_.size() * sizeof(uint32_t);
        lb.name = "light indices";
        light_index_buffer_ = dev->create_buffer(lb);
    }

    {
        // RGBA16F, not 8-bit: the sun in this sky is worth several
        // hundred and the whole point of prefiltering it is that a
        // mirror can still see it.
        TextureDesc ec;
        ec.width = ec.height = std::clamp(settings_.env_size, 16u, 1024u);
        ec.dim = TextureDim::TexCube;
        ec.format = Format::RGBA16F;
        ec.usage = TextureUsage::Sampled | TextureUsage::ColourTarget;
        // Mips all the way down on the source too, so the prefilter
        // can read a blurred version for its rough lobes instead of
        // sparkling.
        ec.mips = 0;
        ec.name = "environment";
        env_cube_ = dev->create_texture(ec);

        ec.mips = 0;
        ec.name = "environment specular";
        env_specular_ = dev->create_texture(ec);
        env_mips_ = dev->texture_desc(env_specular_).mips;

        ec.width = ec.height =
            std::clamp(settings_.env_irradiance_size, 8u, 128u);
        ec.mips = 1;
        ec.name = "environment irradiance";
        env_irradiance_ = dev->create_texture(ec);
    }

    {
        // ONE ATLAS FOR EVERY PUNCTUAL SHADOW. Six separate cubemaps
        // would be six textures to bind and a cube array is a
        // dimension the RHI does not carry; a 2D atlas of square
        // tiles is one texture, one pass and one clear, and an omni
        // is simply six consecutive tiles in it.
        const uint32_t as = std::clamp(settings_.shadow_atlas_size, 256u, 8192u);
        const uint32_t ts = std::clamp(settings_.shadow_tile_size, 64u, as);
        atlas_tiles_per_row_ = std::max(1u, as / ts);
        TextureDesc ad;
        ad.width = ad.height = atlas_tiles_per_row_ * ts;
        ad.format = Format::D32F;
        ad.usage = TextureUsage::Sampled | TextureUsage::DepthTarget |
                   TextureUsage::TransferSrc;
        ad.name = "punctual shadow atlas";
        shadow_atlas_ = dev->create_texture(ad);
    }

    // THE SHADOW MAP IS ONE ARRAY, ONE LAYER PER CASCADE. An atlas in
    // a single 2D texture would work too, but then every filter tap
    // has to be clamped inside its tile by hand or a cascade bleeds
    // into its neighbour at the seam; with an array the hardware's own
    // clamp is per layer and the bleed cannot happen.
    shadow_size_ = std::clamp(settings_.shadow_map_size, 256u, 8192u);
    TextureDesc sd;
    sd.width = sd.height = shadow_size_;
    sd.layers = 4;
    sd.dim = TextureDim::Tex2DArray;
    sd.format = Format::D32F;
    // TransferSrc so dump_shadow_map can read it back; a debug path
    // nobody can run is a debug path nobody has.
    sd.usage = TextureUsage::Sampled | TextureUsage::DepthTarget |
               TextureUsage::TransferSrc;
    sd.name = "shadow map";
    shadow_map_ = dev->create_texture(sd);

    // A one-texel stand-in, bound while the real map is the depth
    // attachment. See the note on the member.
    TextureDesc dd = sd;
    dd.width = dd.height = 1;
    dd.name = "shadow map (stand-in)";
    shadow_dummy_ = dev->create_texture(dd);

    TextureDesc ad2;
    ad2.width = ad2.height = 1;
    ad2.format = Format::D32F;
    ad2.usage = TextureUsage::Sampled | TextureUsage::DepthTarget;
    ad2.name = "shadow atlas (stand-in)";
    shadow_atlas_dummy_ = dev->create_texture(ad2);

    BindGroupDesc fg;
    fg.layout = frame_layout_;
    fg.name = "frame";
    {
        BindGroupEntry e;
        e.binding = 0;
        e.buffer = frame_ubo_;
        fg.entries.push_back(e);
        BindGroupEntry t;
        t.binding = 1;
        t.texture = shadow_map_;
        t.sampler = SamplerCache::shadow(dev);
        fg.entries.push_back(t);
        BindGroupEntry l;
        l.binding = 2;
        l.buffer = light_buffer_;
        fg.entries.push_back(l);
        l.binding = 3;
        l.buffer = cluster_buffer_;
        fg.entries.push_back(l);
        l.binding = 4;
        l.buffer = light_index_buffer_;
        fg.entries.push_back(l);
        BindGroupEntry env;
        env.binding = 5;
        env.texture = env_irradiance_;
        env.sampler = SamplerCache::linear_clamp(dev);
        fg.entries.push_back(env);
        env.binding = 6;
        env.texture = env_specular_;
        fg.entries.push_back(env);
        BindGroupEntry at;
        at.binding = 7;
        at.texture = shadow_atlas_;
        at.sampler = SamplerCache::shadow(dev);
        fg.entries.push_back(at);
    }
    frame_group_ = dev->create_bind_group(fg);

    // The same frame data with the stand-in in place of the real map,
    // for the pass that renders the real map.
    fg.entries[1].texture = shadow_dummy_;
    // And the atlas, for the pass that renders the atlas.
    for (BindGroupEntry &e : fg.entries)
        if (e.binding == 7) e.texture = shadow_atlas_dummy_;
    fg.name = "frame (shadow pass)";
    frame_group_no_shadow_ = dev->create_bind_group(fg);

    {
        BindGroupDesc eg;
        eg.layout = env_layout_;
        eg.name = "environment source";
        BindGroupEntry e;
        e.binding = 17;
        e.texture = env_cube_;
        e.sampler = SamplerCache::linear_clamp(dev);
        eg.entries.push_back(e);
        env_group_ = dev->create_bind_group(eg);
    }

    BindGroupDesc vg;
    vg.layout = view_layout_;
    vg.name = "view";
    {
        BindGroupEntry e;
        e.binding = 8;
        e.buffer = view_ubo_;
        e.range = sizeof(ViewUniforms);
        vg.entries.push_back(e);
    }
    view_group_ = dev->create_bind_group(vg);

    BindGroupDesc pg;
    pg.layout = portal_layout_;
    pg.name = "portal";
    {
        BindGroupEntry e;
        e.binding = 16;
        e.buffer = portal_ubo_;
        e.range = sizeof(PortalUniforms);
        pg.entries.push_back(e);
    }
    portal_group_ = dev->create_bind_group(pg);

    portal_quad_ = Mesh::quad({1, 1});
    portal_quad_->upload(dev, "portal quad");

    default_material_ = Material::make(Color(0.8f, 0.8f, 0.8f), 0.7f, 0.0f);
    default_material_->prepare(dev, material_layout_);

    if (!create_pipelines()) return false;
    WR_INFO("renderer: %ux MSAA, portals to depth %d", samples_,
            settings_.max_portal_depth);
    return true;
}

void Renderer::shutdown() {
    if (!device_) return;
    destroy_targets();
    for (PipelineH *p : {&pipe_.mesh_opaque, &pipe_.mesh_opaque_ds, &pipe_.mesh_cutout,
                         &pipe_.mesh_cutout_ds, &pipe_.mesh_blend, &pipe_.mesh_blend_ds,
                         &pipe_.sky, &pipe_.portal_mark, &pipe_.portal_depth_clear,
                         &pipe_.portal_restore, &pipe_.portal_rim, &pipe_.tonemap})
        if (p->valid()) device_->destroy(*p);
    pipe_ = Pipelines();
    for (BindGroupH *g : {&frame_group_, &view_group_, &portal_group_, &tonemap_group_})
        if (g->valid()) device_->destroy(*g);
    for (BufferH *b : {&frame_ubo_, &view_ubo_, &portal_ubo_})
        if (b->valid()) device_->destroy(*b);
    if (shadow_map_.valid()) device_->destroy(shadow_map_);
    if (shadow_dummy_.valid()) device_->destroy(shadow_dummy_);
    atlas_valid_ = false;
    if (shadow_atlas_.valid()) device_->destroy(shadow_atlas_);
    if (shadow_atlas_dummy_.valid()) device_->destroy(shadow_atlas_dummy_);
    if (light_buffer_.valid()) device_->destroy(light_buffer_);
    if (cluster_buffer_.valid()) device_->destroy(cluster_buffer_);
    if (light_index_buffer_.valid()) device_->destroy(light_index_buffer_);
    if (env_cube_.valid()) device_->destroy(env_cube_);
    if (env_irradiance_.valid()) device_->destroy(env_irradiance_);
    if (env_specular_.valid()) device_->destroy(env_specular_);
    for (BindGroupLayoutH *l : {&frame_layout_, &view_layout_, &material_layout_,
                                &portal_layout_, &tonemap_layout_})
        if (l->valid()) device_->destroy(*l);
    portal_quad_.reset();
    default_material_.reset();
    Texture::release_defaults(device_);
    device_ = nullptr;
}

// -------------------------------------------------------------- targets

bool Renderer::create_targets(uint32_t w, uint32_t h) {
    destroy_targets();
    width_ = w;
    height_ = h;
    if (!w || !h) return false;

    TextureDesc cd;
    cd.width = w;
    cd.height = h;
    cd.format = Format::RGBA16F;
    cd.samples = samples_;
    cd.usage = TextureUsage::ColourTarget | TextureUsage::Sampled;
    cd.name = "hdr colour";
    colour_hdr_ = device_->create_texture(cd);

    if (samples_ > 1) {
        TextureDesc rd = cd;
        rd.samples = 1;
        rd.usage = TextureUsage::ColourTarget | TextureUsage::Sampled;
        rd.name = "hdr resolve";
        colour_resolve_ = device_->create_texture(rd);
    }

    TextureDesc dd;
    dd.width = w;
    dd.height = h;
    // Float depth for reverse-Z, stencil for portals. See docs.
    dd.format = Format::D32F_S8;
    dd.samples = samples_;
    dd.usage = TextureUsage::DepthTarget;
    dd.name = "depth stencil";
    depth_stencil_ = device_->create_texture(dd);

    BindGroupDesc tg;
    tg.layout = tonemap_layout_;
    tg.name = "tonemap";
    BindGroupEntry e;
    e.binding = 16;
    e.texture = samples_ > 1 ? colour_resolve_ : colour_hdr_;
    e.sampler = SamplerCache::linear_clamp(device_);
    tg.entries.push_back(e);
    if (tonemap_group_.valid())
        device_->update_bind_group(tonemap_group_, tg);
    else
        tonemap_group_ = device_->create_bind_group(tg);

    return colour_hdr_.valid() && depth_stencil_.valid();
}

void Renderer::destroy_targets() {
    if (!device_) return;
    for (TextureH *t : {&colour_hdr_, &colour_resolve_, &depth_stencil_})
        if (t->valid()) device_->destroy(*t);
    colour_hdr_ = {};
    colour_resolve_ = {};
    depth_stencil_ = {};
}

void Renderer::resize(uint32_t w, uint32_t h) {
    if (w == width_ && h == height_) return;
    device_->wait_idle();
    create_targets(w, h);
}

// ------------------------------------------------------------ pipelines

bool Renderer::create_pipelines() {
    auto shader = [&](const char *name, ShaderStage stage) -> ShaderH {
        const shaders::Blob *b = shaders::find(name, stage);
        if (!b) {
            WR_ERROR("renderer: shader '%s' is missing from the build", name);
            return {};
        }
        return device_->create_shader(shaders::desc(*b));
    };

    ShaderH mesh_vs = shader("mesh", ShaderStage::Vertex);
    ShaderH mesh_fs = shader("mesh", ShaderStage::Fragment);
    ShaderH skin_vs = shader("mesh_skinned", ShaderStage::Vertex);
    ShaderH sky_vs = shader("sky", ShaderStage::Vertex);
    ShaderH sky_fs = shader("sky", ShaderStage::Fragment);
    ShaderH portal_vs = shader("portal", ShaderStage::Vertex);
    ShaderH portal_fs = shader("portal", ShaderStage::Fragment);
    ShaderH full_vs = shader("fullscreen", ShaderStage::Vertex);
    ShaderH full_fs = shader("fullscreen", ShaderStage::Fragment);
    ShaderH shadow_vs = shader("shadow", ShaderStage::Vertex);
    ShaderH shadow_fs = shader("shadow", ShaderStage::Fragment);
    ShaderH skycube_vs = shader("skycube", ShaderStage::Vertex);
    ShaderH skycube_fs = shader("skycube", ShaderStage::Fragment);
    ShaderH irr_vs = shader("irradiance", ShaderStage::Vertex);
    ShaderH irr_fs = shader("irradiance", ShaderStage::Fragment);
    ShaderH pre_vs = shader("prefilter", ShaderStage::Vertex);
    ShaderH pre_fs = shader("prefilter", ShaderStage::Fragment);
    if (!mesh_vs.valid() || !portal_vs.valid() || !full_vs.valid()) return false;

    // EVERY PIPELINE TESTS THE STENCIL. There is no "outside a portal"
    // pipeline and an "inside a portal" one: the outer view is simply
    // recursion depth zero, testing against a reference of zero. One
    // set of pipelines serves every level, which is what makes the
    // recursion cheap enough to be worth having.
    StencilFace test_equal;
    test_equal.compare = CompareOp::Equal;
    test_equal.pass = StencilOp::Keep;
    test_equal.write_mask = 0;  // the geometry passes never write stencil

    PipelineDesc base;
    base.vertex = mesh_vs;
    base.fragment = mesh_fs;
    base.vertex_layout = standard_vertex_layout();
    const Format hdr_format = Format::RGBA16F;
    base.colour_formats = {hdr_format};
    base.depth_format = Format::D32F_S8;
    base.samples = samples_;
    base.bind_group_layouts = {frame_layout_, view_layout_, material_layout_};
    base.push_constant_size = sizeof(PushUniforms);
    base.depth_stencil.stencil_test = true;
    base.depth_stencil.front = test_equal;
    base.depth_stencil.back = test_equal;
    base.blend = {BlendState::opaque()};

    // ------------------------------------------------------- shadows
    //
    // No colour attachment, no stencil, no MSAA, and a depth format
    // without a stencil aspect: a shadow map is depth and nothing
    // else. The bias is slope-scaled -- see the note in
    // RenderSettings.
    if (shadow_vs.valid() && shadow_fs.valid()) {
        PipelineDesc sp;
        sp.vertex = shadow_vs;
        sp.fragment = shadow_fs;
        sp.vertex_layout = standard_vertex_layout();
        sp.colour_formats = {};
        sp.depth_format = Format::D32F;
        sp.samples = 1;
        sp.bind_group_layouts = {frame_layout_, view_layout_, material_layout_};
        sp.push_constant_size = sizeof(PushUniforms);
        sp.depth_stencil.depth_test = true;
        sp.depth_stencil.depth_write = true;
        sp.depth_stencil.stencil_test = false;
        sp.depth_stencil.depth_bias_enable = true;
        // Reverse-Z inverts which way "further from the light" is, so
        // the bias that pushes a caster away has to invert with it.
        sp.depth_stencil.depth_bias_constant = -settings_.shadow_bias_constant;
        sp.depth_stencil.depth_bias_slope = -settings_.shadow_bias_slope;
        // BACK FACES CULLED, the same as the camera pass, and that is
        // not the obvious choice.
        //
        // The usual advice is to cull FRONT faces when rendering a
        // shadow map: recording the far side of each object puts the
        // stored depth behind the surface being lit, and the acne
        // goes away without any bias at all. It works beautifully on
        // closed, outward-facing, watertight geometry -- and a room
        // is none of those. A room is a box turned inside out, so
        // every one of its faces is wound the other way, and culling
        // front faces records the CEILING across the room's whole
        // footprint. Every floor pixel then sits metres behind the
        // recorded depth and the entire room goes black; the only
        // lit surface left is the ceiling itself.
        //
        // So: cull the same faces the camera culls, and deal with
        // acne where acne is actually solvable -- the normal-offset
        // bias in mesh.glsl, which moves the LOOKUP rather than the
        // stored depth and so cannot detach a shadow from its caster.
        sp.raster.cull = CullMode::Back;
        sp.name = "shadow";
        pipe_.shadow = device_->create_pipeline(sp);
        sp.raster.cull = CullMode::None;
        sp.name = "shadow (two sided)";
        pipe_.shadow_ds = device_->create_pipeline(sp);

        // THE PUNCTUAL ATLAS GETS NO POLYGON OFFSET.
        //
        // A slope-scaled depth bias is defined against "the smallest
        // resolvable difference", which for a float depth buffer is
        // a per-primitive quantity on Vulkan and an implementation's
        // own business on OpenGL. Under an orthographic cascade the
        // two land close enough; under the perspective frusta of a
        // cube face they do not, and coplanar surfaces -- which a
        // room built out of boxes has at every corner -- resolved
        // differently on the two backends. 22% of the atlas's texels
        // differed, and none of them had to.
        //
        // The punctual lookup biases along the surface normal in the
        // shader instead, which is not the driver's business at all.
        sp.depth_stencil.depth_bias_enable = false;
        sp.depth_stencil.depth_bias_constant = 0.0f;
        sp.depth_stencil.depth_bias_slope = 0.0f;
        sp.raster.cull = CullMode::Back;
        sp.name = "punctual shadow";
        pipe_.punctual = device_->create_pipeline(sp);
        sp.raster.cull = CullMode::None;
        sp.name = "punctual shadow (two sided)";
        pipe_.punctual_ds = device_->create_pipeline(sp);
    }

    // ------------------------------------------- the environment bake
    //
    // No depth, no stencil, no vertex buffer: a full-screen triangle
    // into one cube face. Run at start-up and when the sun moves,
    // never inside a normal frame's budget.
    {
        PipelineDesc ep;
        ep.vertex = skycube_vs;
        ep.fragment = skycube_fs;
        ep.colour_formats = {Format::RGBA16F};
        ep.depth_format = Format::Undefined;
        ep.samples = 1;
        ep.raster.cull = CullMode::None;
        ep.push_constant_size = sizeof(PushUniforms);
        ep.blend = {BlendState::opaque()};
        ep.bind_group_layouts = {frame_layout_, view_layout_};
        ep.name = "sky to cube";
        if (skycube_vs.valid()) pipe_.skycube = device_->create_pipeline(ep);

        ep.bind_group_layouts = {frame_layout_, view_layout_, env_layout_};
        ep.vertex = irr_vs;
        ep.fragment = irr_fs;
        ep.name = "irradiance";
        if (irr_vs.valid()) pipe_.irradiance = device_->create_pipeline(ep);
        ep.vertex = pre_vs;
        ep.fragment = pre_fs;
        ep.name = "prefilter";
        if (pre_vs.valid()) pipe_.prefilter = device_->create_pipeline(ep);
    }

    base.name = "mesh opaque";
    pipe_.mesh_opaque = device_->create_pipeline(base);

    // THE SKINNED VARIANT: the same state, a second vertex stream and
    // a fourth bind group. It shares every line of its shading with
    // the static one -- see mesh_shade.glsl -- so the two cannot
    // drift apart, which two copies of a lighting model always do.
    if (skin_vs.valid()) {
        PipelineDesc sk = base;
        sk.vertex = skin_vs;
        sk.vertex_layout = skinned_vertex_layout();
        sk.bind_group_layouts = {frame_layout_, view_layout_, material_layout_,
                                 bone_layout_};
        sk.name = "mesh skinned";
        pipe_.mesh_skinned = device_->create_pipeline(sk);
        sk.raster.cull = CullMode::None;
        sk.name = "mesh skinned (two sided)";
        pipe_.mesh_skinned_ds = device_->create_pipeline(sk);
    }
    base.raster.cull = CullMode::None;
    base.name = "mesh opaque (two sided)";
    pipe_.mesh_opaque_ds = device_->create_pipeline(base);
    base.raster.cull = CullMode::Back;

    // Cutout is the same pipeline; the shader discards. Named apart so
    // a capture shows which pass a draw came from.
    base.name = "mesh cutout";
    pipe_.mesh_cutout = device_->create_pipeline(base);
    base.raster.cull = CullMode::None;
    base.name = "mesh cutout (two sided)";
    pipe_.mesh_cutout_ds = device_->create_pipeline(base);
    base.raster.cull = CullMode::Back;

    base.blend = {BlendState::alpha()};
    base.depth_stencil.depth_write = false;
    base.name = "mesh transparent";
    pipe_.mesh_blend = device_->create_pipeline(base);
    base.raster.cull = CullMode::None;
    base.name = "mesh transparent (two sided)";
    pipe_.mesh_blend_ds = device_->create_pipeline(base);
    base.raster.cull = CullMode::Back;
    base.depth_stencil.depth_write = true;
    base.blend = {BlendState::opaque()};

    // --- sky: fills whatever the opaque pass did not reach
    {
        PipelineDesc d;
        d.vertex = sky_vs;
        d.fragment = sky_fs;
        d.colour_formats = {hdr_format};
        d.depth_format = Format::D32F_S8;
        d.samples = samples_;
        d.bind_group_layouts = {frame_layout_, view_layout_};
        d.push_constant_size = sizeof(PushUniforms);
        d.raster.cull = CullMode::None;
        d.depth_stencil.depth_write = false;
        // Drawn at the far plane, which under reverse-Z is zero, with
        // GREATER_EQUAL: it passes only where the depth buffer is still
        // at its cleared value.
        d.depth_stencil.depth_compare = CompareOp::GreaterEqual;
        d.depth_stencil.stencil_test = true;
        d.depth_stencil.front = test_equal;
        d.depth_stencil.back = test_equal;
        d.blend = {BlendState::opaque()};
        d.name = "sky";
        pipe_.sky = device_->create_pipeline(d);
    }

    // --- portal: mark
    {
        PipelineDesc d;
        d.vertex = portal_vs;
        d.fragment = portal_fs;
        d.vertex_layout = standard_vertex_layout();
        d.colour_formats = {hdr_format};
        d.depth_format = Format::D32F_S8;
        d.samples = samples_;
        d.bind_group_layouts = {frame_layout_, view_layout_, portal_layout_};
        d.push_constant_size = sizeof(PushUniforms);
        // Seen from the front only: the back of a portal is a wall.
        d.raster.cull = CullMode::Back;
        // Depth TESTED so a portal behind a wall does not mark, and
        // depth NOT WRITTEN so the inner view can clear it freely.
        d.depth_stencil.depth_test = true;
        d.depth_stencil.depth_write = false;
        d.depth_stencil.stencil_test = true;
        StencilFace mark;
        mark.compare = CompareOp::Equal;
        mark.pass = StencilOp::IncrementClamp;
        mark.fail = StencilOp::Keep;
        mark.depth_fail = StencilOp::Keep;
        d.depth_stencil.front = mark;
        d.depth_stencil.back = mark;
        d.blend = {BlendState::no_colour()};
        d.name = "portal mark";
        pipe_.portal_mark = device_->create_pipeline(d);

        // --- portal: restore. Depth written back to the portal's own
        // surface, stencil decremented, in one pass.
        StencilFace restore;
        restore.compare = CompareOp::Equal;
        restore.pass = StencilOp::DecrementClamp;
        restore.fail = StencilOp::Keep;
        restore.depth_fail = StencilOp::Keep;
        d.depth_stencil.front = restore;
        d.depth_stencil.back = restore;
        d.depth_stencil.depth_test = true;
        d.depth_stencil.depth_compare = CompareOp::Always;
        d.depth_stencil.depth_write = true;
        d.name = "portal restore";
        pipe_.portal_restore = device_->create_pipeline(d);

        // --- portal: the visible rim
        d.depth_stencil.front = test_equal;
        d.depth_stencil.back = test_equal;
        d.depth_stencil.depth_compare = CompareOp::GreaterEqual;
        d.depth_stencil.depth_write = false;
        d.blend = {BlendState::alpha()};
        d.name = "portal rim";
        pipe_.portal_rim = device_->create_pipeline(d);
    }

    // --- portal: clear depth inside the marked region.
    {
        PipelineDesc d;
        d.vertex = full_vs;
        d.fragment = full_fs;
        d.colour_formats = {hdr_format};
        d.depth_format = Format::D32F_S8;
        d.samples = samples_;
        d.bind_group_layouts = {frame_layout_, view_layout_};
        d.push_constant_size = sizeof(PushUniforms);
        d.raster.cull = CullMode::None;
        d.depth_stencil.depth_test = true;
        d.depth_stencil.depth_compare = CompareOp::Always;
        d.depth_stencil.depth_write = true;
        d.depth_stencil.stencil_test = true;
        d.depth_stencil.front = test_equal;
        d.depth_stencil.back = test_equal;
        d.blend = {BlendState::no_colour()};
        d.name = "portal depth clear";
        pipe_.portal_depth_clear = device_->create_pipeline(d);

        // --- tonemap, straight to the swapchain
        //
        // ITS OWN VERTEX STAGE, not the shared full-screen one.
        // fullscreen.glsl writes push.params.x into gl_Position.z,
        // because the portal pass needs a triangle at a chosen depth
        // to clear depth inside a stencil. The tonemap's fragment
        // stage reads that same push slot as the exposure. Sharing
        // the vertex shader therefore made the exposure the
        // clip-space Z as well, and anything above 1.0 put the
        // triangle outside the clip volume and deleted the frame.
        // See docs/known-issues.md for the hunt.
        PipelineDesc t;
        t.vertex = shader("tonemap", ShaderStage::Vertex);
        t.fragment = shader("tonemap", ShaderStage::Fragment);
        t.colour_formats = {device_->swapchain_format()};
        t.depth_format = Format::Undefined;
        t.samples = 1;
        t.bind_group_layouts = {frame_layout_, view_layout_, tonemap_layout_};
        t.push_constant_size = sizeof(PushUniforms);
        t.raster.cull = CullMode::None;
        t.depth_stencil.depth_test = false;
        t.depth_stencil.depth_write = false;
        t.blend = {BlendState::opaque()};
        t.name = "tonemap";
        pipe_.tonemap = device_->create_pipeline(t);
    }

    return pipe_.mesh_opaque.valid() && pipe_.portal_mark.valid() &&
           pipe_.portal_restore.valid() && pipe_.tonemap.valid();
}

// ------------------------------------------------------------ uniforms

void Renderer::upload_frame() {
    FrameUniforms f{};
    f.time = Vec4(0, 0, 0, 0);
    f.sun_direction = Vec4(-sun_dir_used_.normalized(), 0.0f);
    f.sun_colour = Vec4(sun_colour_used_.rgb(), sun_energy_used_);
    f.ambient = Vec4(ambient.rgb(), ambient_energy);
    f.fog = Vec4(fog_colour.rgb(), fog_density);
    f.fog_params = Vec4(0.0f, 0.0f, fog_height_falloff, 0.0f);
    if (settings_.shadows && cascade_count_ > 0) {
        for (int i = 0; i < 4; i++) f.sun_view_proj[i] = cascade_view_proj_[i];
        f.cascade_splits = cascade_splits_;
        f.cascade_texel = cascade_texel_;
    } else {
        // Splits past any possible view depth put every fragment in
        // the last cascade, whose matrix is the identity -- so the
        // lookup lands outside [0,1] and sample_shadow returns lit.
        for (int i = 0; i < 4; i++) f.sun_view_proj[i] = Projection::identity();
        f.cascade_splits = Vec4(1e9f, 1e9f, 1e9f, 1e9f);
        f.cascade_texel = Vec4(0, 0, 0, 0);
    }
    f.screen = Vec4(float(width_), float(height_),
                    width_ ? 1.0f / float(width_) : 0.0f,
                    height_ ? 1.0f / float(height_) : 0.0f);
    f.counts[0] = int32_t(lights_.size());
    f.env = Vec4(settings_.image_based_lighting && env_baked_
                     ? float(env_mips_)
                     : 0.0f,
                 settings_.env_intensity, 0.0f, 0.0f);
    device_->write_buffer(frame_ubo_, &f, sizeof(f));
}

uint32_t Renderer::upload_view(const View &v) {
    if (view_cursor_ >= kViewRing) {
        WR_WARN("renderer: more than %u views in a frame; the rest are dropped",
                kViewRing);
        return 0;
    }
    ViewUniforms u{};
    Transform3D cam = v.camera;
    cam.basis = cam.basis.orthonormalized();
    Transform3D view_matrix = cam.inverse_orthonormal();
    u.view = to_projection(view_matrix);
    u.proj = v.projection;
    u.view_proj = v.projection * u.view;
    u.inv_view = to_projection(cam);
    u.inv_proj = v.projection.inverse();
    u.eye = Vec4(cam.origin, 1.0f);
    float zf = v.projection.get_z_far();
    u.near_far = Vec4(v.projection.get_z_near(), std::isfinite(zf) ? zf : -1.0f,
                      0.0f, 0.0f);
    u.portal[0] = v.depth;
    u.portal[1] = v.portal_id;
    // The froxel grid for this view, built on the way past. A view
    // that gets no grid points at block zero with a zero count, so
    // the shader's loop runs zero times rather than reading someone
    // else's lights.
    if (v.clustered && settings_.punctual_lights && !lights_.empty() &&
        clustered_views_ < settings_.max_clustered_views) {
        const int base = cluster_view(v, clustered_views_++);
        const float near = std::max(v.projection.get_z_near(), 1e-3f);
        const float log_ratio = std::log2(kClusterFar / near);
        u.cluster = Vec4(float(base), 0.0f, float(kClusterZ) / log_ratio,
                         -float(kClusterZ) * std::log2(near) / log_ratio);
    } else {
        u.cluster = Vec4(0, 0, 0, 0);
    }
    uint32_t offset = view_cursor_ * view_stride_;
    device_->write_buffer(view_ubo_, &u, sizeof(u), offset);
    view_cursor_++;
    return offset;
}

uint32_t Renderer::upload_portal(const Portal3D *p) {
    if (portal_cursor_ >= kPortalRing) return 0;
    PortalUniforms u{};
    u.edge_colour = p->edge_colour.rgba();
    float aspect = p->height > 1e-4f ? p->width / p->height : 1.0f;
    u.edge_params = Vec4(p->edge_width, p->open, aspect, 0.0f);
    uint32_t offset = portal_cursor_ * portal_stride_;
    device_->write_buffer(portal_ubo_, &u, sizeof(u), offset);
    portal_cursor_++;
    return offset;
}

// ------------------------------------------------------------ collection

void Renderer::collect(SceneTree *tree, uint32_t cull_mask) {
    renderables_.clear();
    bone_rows_.clear();
    portals_.clear();
    lights_.clear();
    // The renderer's own sun fields are the default, not the law: a
    // DirectionalLight3D in the tree is what a scene author reaches
    // for, and until now the renderer ignored it entirely.
    sun_dir_used_ = sun_direction;
    sun_colour_used_ = sun_colour;
    sun_energy_used_ = sun_energy;
    if (!tree || !tree->root()) return;

    std::vector<Node *> stack{tree->root()};
    while (!stack.empty()) {
        Node *n = stack.back();
        stack.pop_back();
        for (const auto &c : n->children())
            if (c) stack.push_back(c.get());

        if (Portal3D *p = n->cast_to<Portal3D>()) {
            if (p->active && p->linked() && p->visible_in_tree()) portals_.push_back(p);
            continue;
        }
        if (Light3D *l = n->cast_to<Light3D>()) {
            if (!l->visible_in_tree() || l->energy <= 0.0f) continue;
            if (DirectionalLight3D *d = n->cast_to<DirectionalLight3D>()) {
                // The first one in the tree wins. A second sun is a
                // scene mistake, not a feature to support.
                sun_dir_used_ = d->direction();
                sun_colour_used_ = d->colour;
                sun_energy_used_ = d->energy;
                continue;
            }
            OmniLight3D *o = n->cast_to<OmniLight3D>();
            if (!o || o->range <= 0.0f) continue;
            if (int(lights_.size()) >= settings_.max_lights) continue;
            SpotLight3D *sp = n->cast_to<SpotLight3D>();
            LightGpu g{};
            const Transform3D xf = o->global_transform();
            g.position_range = Vec4(xf.origin, o->range);
            g.colour_energy = Vec4(o->colour.rgb(), o->energy);
            const Vec3 dir = o->forward();
            const float outer = sp ? std::cos(sp->angle) : -1.0f;
            const float inner =
                sp ? std::cos(sp->angle * (1.0f - clampf(sp->angle_softness,
                                                         0.0f, 0.95f)))
                   : 1.0f;
            g.direction_cone = Vec4(dir, outer);
            g.params = Vec4(inner, std::max(o->radius, 1e-3f),
                            sp ? 1.0f : 0.0f, 0.0f);
            lights_.push_back(g);
            continue;
        }
        MeshInstance3D *mi = n->cast_to<MeshInstance3D>();
        if (!mi || !mi->mesh || !mi->visible_in_tree()) continue;
        if (!(mi->layers() & cull_mask)) continue;
        Mesh *mesh = mi->mesh.get();
        if (!mesh->uploaded() && !mesh->upload(device_, mi->name().c_str())) continue;

        // A SKINNED BODY WRITES ITS BONES INTO THE FRAME'S ONE BUFFER
        // and remembers where. Done here rather than at draw time
        // because a body is drawn once per submesh and once per
        // portal view, and its pose is the same every time.
        int bone_base = -1;
        if (Skinned3D *sk = mi->cast_to<Skinned3D>()) {
            const Pose &pose = sk->pose();
            if (pose.valid() && mesh->skinned()) {
                bone_base = int(bone_rows_.size() / 3);
                for (const Transform3D &m : pose.skin) {
                    // Three rows of four. The fourth row of a bone
                    // matrix is always (0,0,0,1) and storing it costs
                    // a quarter of the bandwidth for nothing.
                    const Basis &b = m.basis;
                    bone_rows_.push_back(Vec4(b.col[0].x, b.col[1].x, b.col[2].x,
                                              m.origin.x));
                    bone_rows_.push_back(Vec4(b.col[0].y, b.col[1].y, b.col[2].y,
                                              m.origin.y));
                    bone_rows_.push_back(Vec4(b.col[0].z, b.col[1].z, b.col[2].z,
                                              m.origin.z));
                }
            }
        }

        Transform3D model = mi->global_transform();
        for (const SubMesh &sm : mesh->submeshes) {
            Renderable r;
            r.mesh = mesh;
            r.sub = &sm;
            Material *mat = mi->get_material(sm.material_slot);
            r.material = mat ? mat : default_material_.get();
            r.material->prepare(device_, material_layout_);
            r.model = model;
            r.bounds = sm.bounds.transformed(model);
            r.tint = mi->tint;
            r.key = r.material->sort_key();
            r.bone_base = bone_base;
            renderables_.push_back(r);
        }
    }
    stats_.visible_meshes = uint32_t(renderables_.size());
    stats_.lights = uint32_t(lights_.size());
    stats_.skinned_bones = uint32_t(bone_rows_.size() / 3);
    upload_bones();
}


// EVERY BODY'S BONES, ONCE A FRAME.
//
// The buffer grows and never shrinks: a round with thirty shamblers
// in it is followed by another one, and freeing the storage between
// them to reallocate it a second later is work for no reason. It is
// three vec4s per bone -- thirty bodies on a nineteen-bone rig is
// twenty-seven kilobytes.
void Renderer::upload_bones() {
    if (bone_rows_.empty()) return;
    const uint32_t need = uint32_t(bone_rows_.size());
    if (need > bone_capacity_) {
        if (bone_buffer_.valid()) device_->destroy(bone_buffer_);
        if (bone_group_.valid()) device_->destroy(bone_group_);
        // Rounded up generously, so a crowd that grows by one body
        // does not reallocate.
        bone_capacity_ = std::max(need * 2u, 3u * 256u);
        rhi::BufferDesc bd;
        bd.size = uint64_t(bone_capacity_) * sizeof(Vec4);
        bd.usage = rhi::BufferUsage::Storage;
        bd.access = rhi::MemoryAccess::CpuToGpu;
        bd.name = "bones";
        bone_buffer_ = device_->create_buffer(bd);
        rhi::BindGroupDesc gd;
        gd.layout = bone_layout_;
        rhi::BindGroupEntry e;
        e.binding = 24;
        e.buffer = bone_buffer_;
        gd.entries.push_back(e);
        gd.name = "bones";
        bone_group_ = device_->create_bind_group(gd);
    }
    device_->write_buffer(bone_buffer_, bone_rows_.data(),
                          uint64_t(need) * sizeof(Vec4));
}

// --------------------------------------------------------------- drawing

void Renderer::draw_geometry(rhi::CommandList *cmd, const View &view,
                             uint32_t stencil_ref, MaterialPass pass,
                             const Plane frustum[6]) {
    // Sorted so that a run of draws shares a bind group. Transparency
    // additionally needs back-to-front, which is done by distance
    // below rather than by key.
    struct Item {
        const Renderable *r;
        float distance;
    };
    std::vector<Item> items;
    items.reserve(renderables_.size());
    Vec3 eye = view.camera.origin;
    for (const Renderable &r : renderables_) {
        if (r.material->pass != pass) continue;
        if (!visible_in(frustum, r.bounds)) continue;
        items.push_back({&r, distance_sq(r.bounds.center(), eye)});
    }
    if (items.empty()) return;

    if (pass == MaterialPass::Transparent) {
        std::sort(items.begin(), items.end(),
                  [](const Item &a, const Item &b) { return a.distance > b.distance; });
    } else {
        // Front to back within a material, so early-Z rejects as much
        // as possible before shading.
        std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
            if (a.r->key != b.r->key) return a.r->key < b.r->key;
            return a.distance < b.distance;
        });
    }

    PipelineH pipeline;
    PipelineH last;
    BindGroupH last_group;
    for (const Item &it : items) {
        const Renderable &r = *it.r;
        bool ds = r.material->double_sided;
        // A SKINNED BODY IS A DIFFERENT PIPELINE, not a flag.
        //
        // It has a second vertex stream and a fourth bind group, and
        // both of those are baked into a pipeline. Only the opaque
        // and cutout passes have a skinned variant -- a transparent
        // character is not a thing this engine needs and pretending
        // otherwise would be two more pipelines that are never used.
        const bool skinned = r.bone_base >= 0 && pipe_.mesh_skinned.valid()
                && pass != MaterialPass::Transparent;
        switch (pass) {
            case MaterialPass::Opaque:
                pipeline = skinned ? (ds ? pipe_.mesh_skinned_ds : pipe_.mesh_skinned)
                                   : (ds ? pipe_.mesh_opaque_ds : pipe_.mesh_opaque);
                break;
            case MaterialPass::AlphaCutout:
                pipeline = skinned ? (ds ? pipe_.mesh_skinned_ds : pipe_.mesh_skinned)
                                   : (ds ? pipe_.mesh_cutout_ds : pipe_.mesh_cutout);
                break;
            case MaterialPass::Transparent:
                pipeline = ds ? pipe_.mesh_blend_ds : pipe_.mesh_blend;
                break;
        }
        if (!pipeline.valid()) continue;
        if (pipeline != last) {
            cmd->bind_pipeline(pipeline);
            cmd->set_stencil_reference(stencil_ref);
            last = pipeline;
            last_group = {};
        }
        BindGroupH mg = r.material->bind_group();
        if (mg != last_group) {
            cmd->bind_group(2, mg);
            last_group = mg;
        }
        PushUniforms pu{};
        pu.model = to_projection(r.model);
        pu.tint = r.tint.rgba();
        // Where this body's bones begin in the frame's one buffer.
        if (skinned) pu.params.x = float(r.bone_base);
        cmd->push_constants(&pu, sizeof(pu));
        cmd->bind_vertex_buffer(0, r.mesh->vertex_buffer());
        if (skinned) {
            cmd->bind_vertex_buffer(1, r.mesh->skin_buffer());
            cmd->bind_group(3, bone_group_);
        }
        cmd->bind_index_buffer(r.mesh->index_buffer(), IndexType::U32);
        cmd->draw_indexed(r.sub->index_count, 1, r.sub->first_index);
        stats_.draw_calls++;
        stats_.triangles += r.sub->index_count / 3;
    }
}

// ============================================================ THE RECURSION

void Renderer::render_view(rhi::CommandList *cmd, const View &view,
                           uint32_t stencil_ref) {
    stats_.views++;
    stats_.max_depth_reached =
        std::max(stats_.max_depth_reached, uint32_t(view.depth));

    // The projection this view actually renders with: the camera's,
    // with an oblique near plane cut into it when we are looking out
    // of a portal.
    View v = view;
    if (v.has_clip) {
        Transform3D cam = v.camera;
        cam.basis = cam.basis.orthonormalized();
        Plane view_space = v.clip.transformed_orthonormal(cam.inverse_orthonormal());
        // THE ONE LINE THE ENGINE EXISTS FOR.
        v.projection = v.projection.with_oblique_near(view_space.as_vec4());
    }

    uint32_t view_offset = upload_view(v);
    Plane frustum[6];
    Transform3D cam_ortho = v.camera;
    cam_ortho.basis = cam_ortho.basis.orthonormalized();
    v.projection.frustum_planes(cam_ortho, frustum);

    if (v.has_scissor && settings_.portal_scissor)
        cmd->set_scissor(v.scissor);
    else
        cmd->set_scissor({0, 0, width_, height_});

    cmd->bind_pipeline(pipe_.mesh_opaque);
    cmd->bind_group(0, frame_group_);
    cmd->bind_group(1, view_group_, &view_offset, 1);

    draw_geometry(cmd, v, stencil_ref, MaterialPass::Opaque, frustum);
    draw_geometry(cmd, v, stencil_ref, MaterialPass::AlphaCutout, frustum);

    if (settings_.draw_sky && pipe_.sky.valid()) {
        cmd->bind_pipeline(pipe_.sky);
        cmd->set_stencil_reference(stencil_ref);
        cmd->bind_group(0, frame_group_);
        cmd->bind_group(1, view_group_, &view_offset, 1);
        PushUniforms pu{};
        pu.model = Projection::identity();
        pu.tint = Vec4(1, 1, 1, 1);
        cmd->push_constants(&pu, sizeof(pu));
        cmd->draw(3);
        stats_.draw_calls++;
    }

    // ---------------------------------------------------- the portals
    const int limit = settings_.max_portal_depth;
    if (v.depth < limit) {
        for (size_t pi = 0; pi < portals_.size(); pi++) {
            View inner;
            rhi::Rect scissor;
            stats_.portals_considered++;
            if (!portal_child(v, pi, &inner, &scissor)) {
                stats_.portals_culled++;
                continue;
            }
            Portal3D *p = portals_[pi];

            char label[64];
            std::snprintf(label, sizeof(label), "portal %zu depth %d", pi,
                          v.depth + 1);
            cmd->push_debug_group(label);

            uint32_t portal_offset = upload_portal(p);
            Transform3D quad_model = p->global_transform() *
                                     Transform3D(Basis::scaled({p->width, p->height, 1.0f}),
                                                 Vec3());
            PushUniforms pu{};
            pu.model = to_projection(quad_model);
            pu.tint = Vec4(1, 1, 1, 1);

            // 1. MARK. The stencil now says where the hole is.
            if (settings_.portal_scissor)
                cmd->set_scissor(scissor);
            else
                cmd->set_scissor({0, 0, width_, height_});
            cmd->bind_pipeline(pipe_.portal_mark);
            cmd->set_stencil_reference(stencil_ref);
            cmd->bind_group(0, frame_group_);
            cmd->bind_group(1, view_group_, &view_offset, 1);
            cmd->bind_group(2, portal_group_, &portal_offset, 1);
            cmd->push_constants(&pu, sizeof(pu));
            cmd->bind_vertex_buffer(0, portal_quad_->vertex_buffer());
            cmd->bind_index_buffer(portal_quad_->index_buffer(), IndexType::U32);
            cmd->draw_indexed(portal_quad_->index_count());
            stats_.draw_calls++;

            // 2. CLEAR DEPTH inside it. There is no API for "clear
            //    depth where the stencil says so"; a triangle at the
            //    far plane with depth writes on is that operation.
            cmd->bind_pipeline(pipe_.portal_depth_clear);
            cmd->set_stencil_reference(stencil_ref + 1);
            cmd->bind_group(0, frame_group_);
            cmd->bind_group(1, view_group_, &view_offset, 1);
            PushUniforms cl{};
            cl.model = Projection::identity();
            cl.params = Vec4(0.0f, 0, 0, 0);  // reverse-Z far plane
            cmd->push_constants(&cl, sizeof(cl));
            cmd->draw(3);
            stats_.draw_calls++;

            // 3. RECURSE, with the warped camera and the destination
            //    aperture as the near plane -- both worked out by
            //    portal_child above.
            render_view(cmd, inner, stencil_ref + 1);

            // 4. RESTORE. The portal's own depth goes back, and the
            //    stencil comes back down, so the outer view is exactly
            //    as it was except for the picture inside the hole.
            if (settings_.portal_scissor)
                cmd->set_scissor(scissor);
            else
                cmd->set_scissor({0, 0, width_, height_});
            cmd->bind_pipeline(pipe_.portal_restore);
            cmd->set_stencil_reference(stencil_ref + 1);
            cmd->bind_group(0, frame_group_);
            cmd->bind_group(1, view_group_, &view_offset, 1);
            cmd->bind_group(2, portal_group_, &portal_offset, 1);
            cmd->push_constants(&pu, sizeof(pu));
            cmd->bind_vertex_buffer(0, portal_quad_->vertex_buffer());
            cmd->bind_index_buffer(portal_quad_->index_buffer(), IndexType::U32);
            cmd->draw_indexed(portal_quad_->index_count());
            stats_.draw_calls++;

            // 5. THE RIM, so the player can see there is a portal there.
            if (p->edge_width > 0.0f) {
                cmd->bind_pipeline(pipe_.portal_rim);
                cmd->set_stencil_reference(stencil_ref);
                cmd->bind_group(0, frame_group_);
                cmd->bind_group(1, view_group_, &view_offset, 1);
                cmd->bind_group(2, portal_group_, &portal_offset, 1);
                cmd->push_constants(&pu, sizeof(pu));
                cmd->bind_vertex_buffer(0, portal_quad_->vertex_buffer());
                cmd->bind_index_buffer(portal_quad_->index_buffer(), IndexType::U32);
                cmd->draw_indexed(portal_quad_->index_count());
                stats_.draw_calls++;
            }

            cmd->pop_debug_group();
        }
    }

    // Transparency last, so it blends over whatever the portals put
    // behind it.
    if (v.has_scissor && settings_.portal_scissor)
        cmd->set_scissor(v.scissor);
    else
        cmd->set_scissor({0, 0, width_, height_});
    cmd->bind_group(0, frame_group_);
    cmd->bind_group(1, view_group_, &view_offset, 1);
    draw_geometry(cmd, v, stencil_ref, MaterialPass::Transparent, frustum);
}





// ------------------------------------------ shadows for punctual lights

int Renderer::allocate_punctual_shadows(const Vec3 &eye) {
    shadow_tiles_.clear();
    for (LightGpu &l : lights_) {
        l.shadow = Vec4(0, 0, 0, 0);
        for (Projection &m : l.shadow_view_proj) m = Projection::identity();
        l.params.w = 0.0f;  // no tile: the shader reads this as "no shadow"
    }
    if (!settings_.punctual_shadows || !shadow_atlas_.valid()) return 0;

    const int total_tiles = int(atlas_tiles_per_row_ * atlas_tiles_per_row_);
    if (total_tiles <= 0) return 0;

    // NEAREST FIRST. There are always more lights than tiles in a
    // scene worth lighting, so the question is not whether to choose
    // but what to choose by -- and the light whose shadow the player
    // is standing in is the one closest to them.
    std::vector<uint32_t> order(lights_.size());
    for (uint32_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return (lights_[a].position_range.xyz() - eye).length_sq() <
               (lights_[b].position_range.xyz() - eye).length_sq();
    });

    const float near = std::max(settings_.punctual_shadow_near, 1e-3f);
    const float tile_uv = 1.0f / float(atlas_tiles_per_row_);
    int next = 0;

    for (uint32_t li : order) {
        LightGpu &L = lights_[li];
        const bool spot = int(L.params.z) == 1;
        const int need = spot ? 1 : 6;
        if (next + need > total_tiles) continue;
        const Vec3 pos = L.position_range.xyz();
        const float far = std::max(L.position_range.w, near * 2.0f);

        L.shadow = Vec4(float(next), float(atlas_tiles_per_row_), tile_uv, 0.0f);
        L.params.w = near;

        if (spot) {
            // The cone, with a little margin so the penumbra at the
            // edge has something recorded to read.
            const float outer = std::acos(std::clamp(L.direction_cone.w,
                                                     -0.999f, 0.999f));
            const float fov = std::min(outer * 2.0f * 1.1f, deg2rad(179.0f));
            Vec3 dir = L.direction_cone.xyz().normalized();
            Vec3 up = std::fabs(dir.y) > 0.95f ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
            Transform3D xf(Basis::looking_at(dir, up), pos);
            Projection proj = Projection::perspective(fov, 1.0f, near, far);
            L.shadow_view_proj[0] = proj * to_projection(xf.inverse_orthonormal());
            shadow_tiles_.push_back({uint32_t(next), xf, proj});
        } else {
            // SIX FACES. The order is the only thing the shader has
            // to agree with -- cube_face_of picks an index from the
            // dominant axis and then reads the matrix this loop
            // uploaded, so the bases below can be whatever an
            // ordinary right-handed camera wants and no convention
            // has to be matched.
            static const Vec3 kForward[6] = {
                {1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            static const Vec3 kUp[6] = {
                {0, 1, 0}, {0, 1, 0}, {0, 0, -1},
                {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
            Projection proj =
                Projection::perspective(deg2rad(90.0f), 1.0f, near, far);
            for (int f = 0; f < 6; f++) {
                Transform3D xf(Basis::looking_at(kForward[f], kUp[f]), pos);
                L.shadow_view_proj[f] =
                    proj * to_projection(xf.inverse_orthonormal());
                shadow_tiles_.push_back({uint32_t(next + f), xf, proj});
            }
        }
        if (getenv("WR_TRACE_TILES"))
            WR_INFO("tile: light %u %s at (%.2f %.2f %.2f) range %.1f -> tiles "
                    "%d..%d", li, spot ? "spot" : "omni", double(pos.x),
                    double(pos.y), double(pos.z), double(L.position_range.w),
                    next, next + need - 1);
        next += need;
        stats_.shadow_casting_lights++;
    }
    return next;
}

uint64_t Renderer::shadow_hash() const {
    // FNV-1a over the bytes that decide what the atlas should hold.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void *p, size_t n) {
        const uint8_t *b = (const uint8_t *)p;
        for (size_t i = 0; i < n; i++) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    for (const LightGpu &l : lights_) {
        if (l.params.w <= 0.0f) continue;   // casts nothing
        mix(&l.position_range, sizeof(Vec4));
        mix(&l.direction_cone, sizeof(Vec4));
        mix(&l.params, sizeof(Vec4));
        mix(&l.shadow, sizeof(Vec4));
    }
    for (const Renderable &r : renderables_) {
        if (!r.material->cast_shadows) continue;
        if (r.material->pass == MaterialPass::Transparent) continue;
        const void *mesh = r.mesh;
        mix(&mesh, sizeof(mesh));
        mix(&r.sub->first_index, sizeof(uint32_t));
        mix(&r.sub->index_count, sizeof(uint32_t));
        mix(&r.model, sizeof(Transform3D));
    }
    return h;
}

void Renderer::punctual_shadow_pass(rhi::CommandList *cmd) {
    if (shadow_tiles_.empty() || !pipe_.punctual.valid()) {
        atlas_valid_ = false;
        return;
    }

    // THE ATLAS IS NOT A PER-FRAME THING. A lamp bolted to a wall in
    // a room made of walls produces the same six depth images every
    // frame for ever, and re-rendering the scene thirteen times to
    // arrive at them again cost this engine 177 fps down to 49. The
    // hash covers every caster's mesh and transform and every
    // shadow-casting light's position and shape; equal means the
    // texture already holds the answer.
    //
    // A moving caster or a moving light changes it and the whole
    // atlas is redrawn, which is the same cost as before -- so this
    // is a saving on static scenes and never a loss on dynamic ones.
    const uint64_t hash = shadow_hash();
    if (getenv("WR_TRACE_ATLAS") && atlas_valid_ && hash != atlas_hash_)
        WR_INFO("atlas: rebaking (%zu lights, %zu casters)", lights_.size(),
                renderables_.size());
    if (atlas_valid_ && hash == atlas_hash_) {
        stats_.punctual_shadows_reused = true;
        return;
    }
    atlas_hash_ = hash;
    atlas_valid_ = true;

    WR_GPU_SCOPE(cmd, "punctual shadows");

    const uint32_t size = device_->texture_desc(shadow_atlas_).width;
    const uint32_t tile = size / std::max(1u, atlas_tiles_per_row_);

    // ONE PASS, MANY VIEWPORTS. Every tile shares the atlas, so the
    // clear happens once and each tile is a scissor rather than a
    // render pass of its own -- which on a tiled GPU is the whole
    // difference between this being affordable and not.
    RenderingInfo ri;
    ri.has_depth = true;
    ri.depth.texture = shadow_atlas_;
    ri.depth.depth_load = LoadOp::Clear;
    ri.depth.clear_depth = 0.0f;  // reverse-Z
    ri.depth.stencil_load = LoadOp::DontCare;
    ri.width = size;
    ri.height = size;
    ri.name = "punctual shadows";
    cmd->begin_rendering(ri);

    for (const ShadowTile &st : shadow_tiles_) {
        const uint32_t tx = st.tile % atlas_tiles_per_row_;
        const uint32_t ty = st.tile / atlas_tiles_per_row_;
        Viewport vp;
        vp.x = float(tx * tile);
        vp.y = float(ty * tile);
        vp.width = float(tile);
        vp.height = float(tile);
        cmd->set_viewport(vp);
        cmd->set_scissor({int(tx * tile), int(ty * tile), tile, tile});

        View lv;
        lv.camera = st.camera;
        lv.projection = st.projection;
        lv.clustered = false;
        const uint32_t offset = upload_view(lv);

        Plane frustum[6];
        st.projection.frustum_planes(st.camera, frustum);

        cmd->bind_pipeline(pipe_.punctual);
        cmd->bind_group(0, frame_group_no_shadow_);
        cmd->bind_group(1, view_group_, &offset, 1);

        PipelineH last = pipe_.punctual;
        BindGroupH last_group;
        for (const Renderable &r : renderables_) {
            if (!r.material->cast_shadows) continue;
            if (r.material->pass == MaterialPass::Transparent) continue;
            if (!visible_in(frustum, r.bounds)) continue;
            PipelineH pipeline =
                r.material->double_sided ? pipe_.punctual_ds : pipe_.punctual;
            if (pipeline != last) {
                cmd->bind_pipeline(pipeline);
                last = pipeline;
                last_group = {};
            }
            BindGroupH mg = r.material->bind_group();
            if (mg != last_group) {
                cmd->bind_group(2, mg);
                last_group = mg;
            }
            PushUniforms pu{};
            pu.model = to_projection(r.model);
            pu.tint = Vec4(1, 1, 1, 1);
            cmd->push_constants(&pu, sizeof(pu));
            cmd->bind_vertex_buffer(0, r.mesh->vertex_buffer());
            cmd->bind_index_buffer(r.mesh->index_buffer(), IndexType::U32);
            cmd->draw_indexed(r.sub->index_count, 1, r.sub->first_index);
            stats_.punctual_shadow_draws++;
            stats_.triangles += r.sub->index_count / 3;
        }
    }
    cmd->end_rendering();
    cmd->texture_barrier(shadow_atlas_, TextureUsage::DepthTarget,
                         TextureUsage::Sampled);
}

// ------------------------------------------------------ the environment

bool Renderer::environment_is_stale() const {
    if (!env_baked_) return true;
    // The sky is a function of these five and nothing else, so this
    // is not a heuristic -- it is the exact condition.
    const Vec3 d = sun_dir_used_.normalized();
    if ((d - baked_sun_).length_sq() > 1e-6f) return true;
    if (std::fabs(sun_energy_used_ - baked_sun_energy_) > 1e-4f) return true;
    auto differs = [](const Color &a, const Color &b) {
        return std::fabs(a.r - b.r) + std::fabs(a.g - b.g) +
                   std::fabs(a.b - b.b) >
               1e-4f;
    };
    return differs(sun_colour_used_, baked_sun_colour_) ||
           differs(ambient, baked_ambient_) || differs(fog_colour, baked_fog_);
}

void Renderer::bake_environment(rhi::CommandList *cmd) {
    if (!pipe_.skycube.valid() || !env_cube_.valid()) return;
    WR_GPU_SCOPE(cmd, "environment");

    const uint32_t size = device_->texture_desc(env_cube_).width;

    auto face_pass = [&](TextureH target, uint32_t face, uint32_t mip,
                         uint32_t extent, const char *name) {
        RenderingInfo ri;
        ColourAttachment ca;
        ca.texture = target;
        ca.layer = int32_t(face);
        ca.mip = mip;
        ca.load = LoadOp::DontCare;
        ri.colour.push_back(ca);
        ri.width = extent;
        ri.height = extent;
        ri.name = name;
        cmd->begin_rendering(ri);
        Viewport vp;
        vp.width = float(extent);
        vp.height = float(extent);
        cmd->set_viewport(vp);
        cmd->set_scissor({0, 0, extent, extent});
    };

    // --- 1. the sky, into six faces
    for (uint32_t face = 0; face < 6; face++) {
        face_pass(env_cube_, face, 0, size, "sky face");
        cmd->bind_pipeline(pipe_.skycube);
        cmd->bind_group(0, frame_group_no_shadow_);
        uint32_t zero = 0;
        cmd->bind_group(1, view_group_, &zero, 1);
        PushUniforms pu{};
        pu.model = Projection::identity();
        pu.tint = Vec4(1, 1, 1, 1);
        pu.params = Vec4(float(face), 0, 0, 0);
        cmd->push_constants(&pu, sizeof(pu));
        cmd->draw(3);
        cmd->end_rendering();
    }
    // The prefilter reads blurred mips of this for its rough lobes.
    cmd->texture_barrier(env_cube_, TextureUsage::ColourTarget,
                         TextureUsage::Sampled);
    cmd->generate_mips(env_cube_);

    // --- 2. the diffuse convolution
    if (pipe_.irradiance.valid() && env_irradiance_.valid()) {
        const uint32_t isize = device_->texture_desc(env_irradiance_).width;
        for (uint32_t face = 0; face < 6; face++) {
            face_pass(env_irradiance_, face, 0, isize, "irradiance face");
            cmd->bind_pipeline(pipe_.irradiance);
            cmd->bind_group(0, frame_group_no_shadow_);
            uint32_t zero = 0;
            cmd->bind_group(1, view_group_, &zero, 1);
            cmd->bind_group(2, env_group_);
            PushUniforms pu{};
            pu.model = Projection::identity();
            pu.params = Vec4(float(face), 0, 0, 0);
            cmd->push_constants(&pu, sizeof(pu));
            cmd->draw(3);
            cmd->end_rendering();
        }
        cmd->texture_barrier(env_irradiance_, TextureUsage::ColourTarget,
                             TextureUsage::Sampled);
    }

    // --- 3. the specular chain, one mip per roughness
    if (pipe_.prefilter.valid() && env_specular_.valid()) {
        const uint32_t mips = env_mips_ ? env_mips_ : 1;
        for (uint32_t mip = 0; mip < mips; mip++) {
            const uint32_t extent = std::max(1u, size >> mip);
            const float rough =
                mips > 1 ? float(mip) / float(mips - 1) : 0.0f;
            for (uint32_t face = 0; face < 6; face++) {
                face_pass(env_specular_, face, mip, extent, "prefilter face");
                cmd->bind_pipeline(pipe_.prefilter);
                cmd->bind_group(0, frame_group_no_shadow_);
                uint32_t zero = 0;
                cmd->bind_group(1, view_group_, &zero, 1);
                cmd->bind_group(2, env_group_);
                PushUniforms pu{};
                pu.model = Projection::identity();
                pu.params = Vec4(float(face), rough, float(size), 0);
                cmd->push_constants(&pu, sizeof(pu));
                cmd->draw(3);
                cmd->end_rendering();
            }
        }
        cmd->texture_barrier(env_specular_, TextureUsage::ColourTarget,
                             TextureUsage::Sampled);
    }

    env_baked_ = true;
    env_bakes_++;
    baked_sun_ = sun_dir_used_.normalized();
    baked_sun_colour_ = sun_colour_used_;
    baked_sun_energy_ = sun_energy_used_;
    baked_ambient_ = ambient;
    baked_fog_ = fog_colour;
}

// --------------------------------------------------------- light culling

void Renderer::upload_lights() {
    // Always write something: a zero-length storage buffer is not a
    // thing, and a descriptor pointing at a buffer with stale bytes
    // behind a zero count is worse than one pointing at zeroes.
    if (!lights_.empty())
        device_->write_buffer(light_buffer_, lights_.data(),
                              lights_.size() * sizeof(LightGpu));
    if (!cluster_counts_.empty())
        device_->write_buffer(cluster_buffer_, cluster_counts_.data(),
                              cluster_counts_.size() * sizeof(uint32_t));
    if (!cluster_indices_.empty())
        device_->write_buffer(light_index_buffer_, cluster_indices_.data(),
                              cluster_indices_.size() * sizeof(uint32_t));
}

int Renderer::cluster_view(const View &v, int slot) {
    if (slot < 0 || slot >= settings_.max_clustered_views) return -1;
    const int base = slot * kClusterCount;

    uint32_t *counts = cluster_counts_.data() + base;
    uint32_t *indices = cluster_indices_.data() + size_t(base) * kClusterMaxLights;
    std::memset(counts, 0, size_t(kClusterCount) * sizeof(uint32_t));
    if (lights_.empty()) return base;

    Transform3D cam = v.camera;
    cam.basis = cam.basis.orthonormalized();
    const Transform3D to_view = cam.inverse_orthonormal();

    const float near = std::max(v.projection.get_z_near(), 1e-3f);
    const float far = kClusterFar;

    // The view-space z at each slice boundary. The shader inverts
    // this with a log; the binder reads it off a table instead,
    // because a light behind the eye still lights what is in front of
    // it and log2 of a negative depth is not a number. Both describe
    // the same slicing -- upload_view writes the shader's scale and
    // bias from the same near and far.
    float slice_z[kClusterZ + 1];
    for (int k = 0; k <= kClusterZ; k++)
        slice_z[k] = near * std::pow(far / near, float(k) / float(kClusterZ));

    // And the view-space x/y extents at each of those depths. Read
    // from the projection, so an off-axis or oblique portal view is
    // handled without a special case.
    float ex_l[kClusterZ + 1], ex_r[kClusterZ + 1];
    float ex_b[kClusterZ + 1], ex_t[kClusterZ + 1];
    for (int k = 0; k <= kClusterZ; k++)
        v.projection.get_extents_at(slice_z[k], &ex_l[k], &ex_r[k], &ex_b[k],
                                    &ex_t[k]);

    size_t assigned = 0;
    for (uint32_t li = 0; li < uint32_t(lights_.size()); li++) {
        const LightGpu &L = lights_[li];
        const Vec3 centre = to_view.xform(L.position_range.xyz());
        const float radius = L.position_range.w;
        // View space looks down -Z, so a point in front has z < 0.
        const float dist = -centre.z;
        if (dist - radius > far || dist + radius < 0.0f) continue;

        // WHICH SLICES. Solved from the boundary table rather than
        // from the log, because the log of a negative z is not a
        // number and a light behind the eye still lights what is in
        // front of it.
        int z0 = 0, z1 = kClusterZ - 1;
        while (z0 < kClusterZ && slice_z[z0 + 1] < dist - radius) z0++;
        while (z1 > z0 && slice_z[z1] > dist + radius) z1--;

        for (int z = z0; z <= z1; z++) {
            // The froxel's depth slab, and the widest x/y extents
            // across it -- a slab is a frustum, not a box, so its
            // bounding box is the far face's.
            const float zn = slice_z[z], zf = slice_z[z + 1];
            const float l = std::min(ex_l[z], ex_l[z + 1]);
            const float r = std::max(ex_r[z], ex_r[z + 1]);
            const float b = std::min(ex_b[z], ex_b[z + 1]);
            const float t = std::max(ex_t[z], ex_t[z + 1]);
            const float tw = (r - l) / float(kClusterX);
            const float th = (t - b) / float(kClusterY);
            if (tw <= 0.0f || th <= 0.0f) continue;

            // Tiles the light's bounding box can touch. Tile 0 in y is
            // the TOP of the screen, matching gl_FragCoord, so y runs
            // from `top` downwards.
            const int tx0 = std::max(0, int(std::floor((centre.x - radius - l) / tw)));
            const int tx1 = std::min(kClusterX - 1,
                                     int(std::floor((centre.x + radius - l) / tw)));
            const int ty0 = std::max(0, int(std::floor((t - centre.y - radius) / th)));
            const int ty1 = std::min(kClusterY - 1,
                                     int(std::floor((t - centre.y + radius) / th)));

            for (int ty = ty0; ty <= ty1; ty++) {
                for (int tx = tx0; tx <= tx1; tx++) {
                    // Exact sphere-versus-box, because the bounding
                    // box above is generous at the corners and a
                    // light assigned to a froxel it does not reach is
                    // a froxel slot wasted on nothing.
                    const AABB froxel(
                        Vec3(l + tw * float(tx), t - th * float(ty + 1), -zf),
                        Vec3(l + tw * float(tx + 1), t - th * float(ty), -zn));
                    if (froxel.distance_squared_to(centre) > radius * radius)
                        continue;

                    const int c = (z * kClusterY + ty) * kClusterX + tx;
                    if (counts[c] >= uint32_t(kClusterMaxLights)) continue;
                    indices[size_t(c) * kClusterMaxLights + counts[c]] = li;
                    counts[c]++;
                    assigned++;
                }
            }
        }
    }
    stats_.light_assignments += uint32_t(assigned);
    return base;
}

// ------------------------------------------------------- portal traversal

// THE ONE COPY OF THE RULE.
//
// Whether a portal recurses from a given view, and with what camera,
// projection and scissor, is decided here and nowhere else. Two
// callers need the answer: the draw, and the walk that fits the shadow
// cascades. Two copies of a rule this fiddly would disagree within a
// week, and the symptom would be shadows that are subtly wrong only
// inside portals -- which is exactly the bug nobody finds.
bool Renderer::portal_child(const View &v, size_t pi, View *out,
                            rhi::Rect *scissor_out) const {
    if (pi >= portals_.size()) return false;
    Portal3D *p = portals_[pi];
    Portal3D *q = p->link();
    if (!q || !q->active) return false;

    // A portal is a hole seen from the front. From behind it is the
    // back of a hole, which is nothing.
    if (!p->faces(v.camera.origin)) return false;

    const int limit = settings_.max_portal_depth;
    int own = p->max_recursion > 0 ? p->max_recursion : limit;
    if (v.depth >= own || v.depth >= limit) return false;

    // Never recurse straight back into the portal we came out of: that
    // is the same room again, one level down, and it doubles the cost
    // of every level for nothing.
    if (v.portal_id == int(pi)) return false;

    Transform3D cam_ortho = v.camera;
    cam_ortho.basis = cam_ortho.basis.orthonormalized();
    Rect2 rect;
    if (!p->screen_rect(cam_ortho.inverse_orthonormal(), v.projection, &rect))
        return false;
    if (rect.area() < settings_.portal_min_coverage) return false;

    // Intersect with the parent's scissor: a portal seen through a
    // portal cannot be wider than the hole it is seen through.
    int x0 = int(rect.position.x * float(width_));
    int y0 = int(rect.position.y * float(height_));
    int x1 = int(std::ceil((rect.position.x + rect.size.x) * float(width_)));
    int y1 = int(std::ceil((rect.position.y + rect.size.y) * float(height_)));
    if (v.has_scissor) {
        x0 = std::max(x0, v.scissor.x);
        y0 = std::max(y0, v.scissor.y);
        x1 = std::min(x1, v.scissor.x + int(v.scissor.width));
        y1 = std::min(y1, v.scissor.y + int(v.scissor.height));
    }
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(int(width_), x1);
    y1 = std::min(int(height_), y1);
    if (x1 <= x0 || y1 <= y0) return false;

    rhi::Rect scissor = {x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0)};
    if (scissor_out) *scissor_out = scissor;

    View inner;
    inner.camera = Portal3D::warp(p, q) * v.camera;
    inner.projection = v.projection;
    // If the eye is nearly in the destination plane the oblique clip
    // degenerates, and the wall it would have removed is edge-on and
    // invisible anyway.
    Plane qp = q->plane();
    if (std::fabs(qp.distance_to(inner.camera.origin)) > 1e-3f) {
        inner.has_clip = true;
        inner.clip = qp;
    }
    inner.has_scissor = true;
    inner.scissor = scissor;
    inner.depth = v.depth + 1;
    // Remembered so the inner view does not immediately look back
    // through the portal it came out of.
    inner.portal_id = -1;
    for (size_t k = 0; k < portals_.size(); k++)
        if (portals_[k] == q) inner.portal_id = int(k);
    *out = inner;
    return true;
}

void Renderer::gather_views(const View &v, std::vector<View> *out) const {
    out->push_back(v);
    // A hard cap independent of the depth limit: the recursion is over
    // pairs of portals and a scene with many of them can branch wider
    // than it goes deep.
    if (out->size() >= kViewRing) return;
    if (v.depth >= settings_.max_portal_depth) return;
    for (size_t pi = 0; pi < portals_.size(); pi++) {
        View inner;
        if (portal_child(v, pi, &inner, nullptr)) gather_views(inner, out);
    }
}

// ------------------------------------------------------------- cascades

void Renderer::fit_cascades(const std::vector<View> &views) {
    const int n = std::clamp(settings_.shadow_cascades, 1, 4);
    cascade_count_ = n;
    const float near = 0.1f;
    const float far = std::max(settings_.shadow_distance, near + 1.0f);

    // THE PRACTICAL SPLIT SCHEME. A logarithmic split gives every
    // cascade the same texel density and a uniform one gives them all
    // the same size; neither is right on its own, and the useful
    // answer is a blend of the two with lambda near three quarters.
    float split[5];
    split[0] = near;
    for (int i = 1; i <= n; i++) {
        const float f = float(i) / float(n);
        const float log_split = near * std::pow(far / near, f);
        const float uniform = near + (far - near) * f;
        split[i] = settings_.shadow_split_lambda * log_split +
                   (1.0f - settings_.shadow_split_lambda) * uniform;
    }

    // The light's orientation. Only the rotation matters for an
    // orthographic projection; the position is chosen per cascade.
    const Vec3 dir = sun_dir_used_.normalized();
    Vec3 up = std::fabs(dir.y) > 0.95f ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
    const Basis light_basis = Basis::looking_at(dir, up);
    const Basis to_light = light_basis.transposed();  // orthonormal inverse

    for (int c = 0; c < 4; c++) {
        if (c >= n) {
            cascade_view_proj_[c] = cascade_view_proj_[n - 1];
            cascade_camera_[c] = cascade_camera_[n - 1];
            cascade_projection_[c] = cascade_projection_[n - 1];
            cascade_texel_[c] = cascade_texel_[n - 1];
            continue;
        }

        // EVERY VIEW, NOT JUST THE CAMERA'S. A portal view looks at a
        // part of the world the camera cannot see directly, and the
        // player is looking straight at it.
        Vec3 corners[8];
        std::vector<Vec3> all;
        all.reserve(views.size() * 8);
        for (const View &v : views) {
            Transform3D cam = v.camera;
            cam.basis = cam.basis.orthonormalized();
            v.projection.slice_corners(split[c], split[c + 1], corners);
            for (int k = 0; k < 8; k++) all.push_back(cam.xform(corners[k]));
        }
        if (all.empty()) {
            cascade_view_proj_[c] = Projection::identity();
            continue;
        }

        // A BOUNDING SPHERE, NOT A BOX, and that is what stops the
        // shadows shimmering. A box fitted to the corners changes size
        // as the camera turns, so every texel lands somewhere new each
        // frame and the edges crawl. A sphere is the same size from
        // every angle, so the only thing left to stabilise is where
        // its centre falls -- and that is a snap to the texel grid.
        Vec3 centre(0, 0, 0);
        for (const Vec3 &p : all) centre = centre + p;
        centre = centre * (1.0f / float(all.size()));
        float radius = 0.0f;
        for (const Vec3 &p : all)
            radius = std::max(radius, (p - centre).length());
        radius = std::max(radius, 0.5f);
        // Rounded up, so a radius that wobbles by a fraction of a
        // texel does not change the projection at all.
        radius = std::ceil(radius * 16.0f) / 16.0f;

        const float texels = float(shadow_size_ ? shadow_size_ : 1);
        const float world_per_texel = 2.0f * radius / texels;

        Vec3 centre_l = to_light.xform(centre);
        centre_l.x = std::floor(centre_l.x / world_per_texel) * world_per_texel;
        centre_l.y = std::floor(centre_l.y / world_per_texel) * world_per_texel;
        centre = light_basis.xform(centre_l);

        // The light sits behind the sphere by the extrusion distance,
        // so geometry outside the view still casts into it.
        const float back = radius + std::max(settings_.shadow_caster_extrusion, 0.0f);
        Transform3D light_xf(light_basis, centre - dir * back);
        Projection proj = Projection::orthographic(-radius, radius, -radius,
                                                   radius, 0.0f, back + radius);

        cascade_camera_[c] = light_xf;
        cascade_projection_[c] = proj;
        cascade_texel_[c] = world_per_texel;
        cascade_view_proj_[c] = proj * to_projection(light_xf.inverse_orthonormal());
    }

    cascade_splits_ = Vec4(split[std::min(1, n)], split[std::min(2, n)],
                           split[std::min(3, n)], split[n]);
    stats_.cascades = uint32_t(n);
}

void Renderer::shadow_pass(rhi::CommandList *cmd) {
    if (!pipe_.shadow.valid() || !shadow_map_.valid()) return;
    WR_GPU_SCOPE(cmd, "shadows");

    for (int c = 0; c < cascade_count_; c++) {
        RenderingInfo ri;
        ri.has_depth = true;
        ri.depth.texture = shadow_map_;
        ri.depth.layer = c;
        ri.depth.depth_load = LoadOp::Clear;
        ri.depth.clear_depth = 0.0f;  // reverse-Z: the far plane
        ri.depth.stencil_load = LoadOp::DontCare;
        ri.width = shadow_size_;
        ri.height = shadow_size_;
        ri.name = "shadow cascade";
        cmd->begin_rendering(ri);

        Viewport vp;
        vp.width = float(shadow_size_);
        vp.height = float(shadow_size_);
        cmd->set_viewport(vp);
        cmd->set_scissor({0, 0, shadow_size_, shadow_size_});

        // The cascade is uploaded as an ordinary view, which is why
        // the shadow shader needs no block of its own.
        View lv;
        lv.camera = cascade_camera_[c];
        lv.projection = cascade_projection_[c];
        lv.clustered = false;
        uint32_t offset = upload_view(lv);

        Plane frustum[6];
        cascade_projection_[c].frustum_planes(cascade_camera_[c], frustum);

        // THE PIPELINE FIRST. Vulkan invalidates the bound descriptor
        // sets when a pipeline with a different layout comes in, so
        // binding the groups before the first pipeline binds them to
        // nothing. Both shadow pipelines share a layout, so the
        // switch to the two-sided one inside the loop is free.
        cmd->bind_pipeline(pipe_.shadow);
        cmd->bind_group(0, frame_group_no_shadow_);
        cmd->bind_group(1, view_group_, &offset, 1);

        PipelineH last = pipe_.shadow;
        BindGroupH last_group;
        for (const Renderable &r : renderables_) {
            if (!r.material->cast_shadows) continue;
            if (r.material->pass == MaterialPass::Transparent) continue;
            if (!visible_in(frustum, r.bounds)) continue;

            PipelineH pipeline =
                r.material->double_sided ? pipe_.shadow_ds : pipe_.shadow;
            if (pipeline != last) {
                cmd->bind_pipeline(pipeline);
                last = pipeline;
                last_group = {};
            }
            BindGroupH mg = r.material->bind_group();
            if (mg != last_group) {
                cmd->bind_group(2, mg);
                last_group = mg;
            }
            PushUniforms pu{};
            pu.model = to_projection(r.model);
            pu.tint = Vec4(1, 1, 1, 1);
            cmd->push_constants(&pu, sizeof(pu));
            cmd->bind_vertex_buffer(0, r.mesh->vertex_buffer());
            cmd->bind_index_buffer(r.mesh->index_buffer(), IndexType::U32);
            cmd->draw_indexed(r.sub->index_count, 1, r.sub->first_index);
            stats_.shadow_draws++;
            stats_.triangles += r.sub->index_count / 3;
        }
        cmd->end_rendering();
    }

    cmd->texture_barrier(shadow_map_, TextureUsage::DepthTarget,
                         TextureUsage::Sampled);
}

uint32_t Renderer::read_shadow_atlas(std::vector<float> *out) const {
    if (!device_ || !shadow_atlas_.valid() || !out) return 0;
    device_->wait_idle();
    const uint32_t n = device_->texture_desc(shadow_atlas_).width;
    out->assign(size_t(n) * n, 0.0f);
    if (device_->read_texture(shadow_atlas_, out->data(),
                              out->size() * sizeof(float)) !=
        out->size() * sizeof(float))
        return 0;
    return n;
}

bool Renderer::dump_shadow_map(const std::string &path) const {
    if (!device_ || !shadow_map_.valid() || cascade_count_ <= 0) return false;
    device_->wait_idle();
    const uint32_t n = uint32_t(cascade_count_);
    const size_t texels = size_t(shadow_size_) * shadow_size_;
    std::vector<float> depth(texels);
    std::vector<uint8_t> out(texels * n);
    for (uint32_t c = 0; c < n; c++) {
        const size_t got = device_->read_texture(
            shadow_map_, depth.data(), depth.size() * sizeof(float), 0, c);
        if (got != depth.size() * sizeof(float)) {
            WR_ERROR("shadow dump: could not read cascade %u", c);
            return false;
        }
        for (uint32_t y = 0; y < shadow_size_; y++)
            for (uint32_t x = 0; x < shadow_size_; x++)
                out[size_t(y) * shadow_size_ * n + c * shadow_size_ + x] =
                    uint8_t(std::clamp(depth[size_t(y) * shadow_size_ + x], 0.0f,
                                       1.0f) * 255.0f);
    }
    if (!stbi_write_png(path.c_str(), int(shadow_size_ * n), int(shadow_size_), 1,
                        out.data(), int(shadow_size_ * n))) {
        WR_ERROR("shadow dump: could not write '%s'", path.c_str());
        return false;
    }
    WR_INFO("shadow dump: %s (%u cascades at %u)", path.c_str(), n, shadow_size_);

    // And the punctual atlas beside it, under the same stem.
    if (shadow_atlas_.valid()) {
        const uint32_t as = device_->texture_desc(shadow_atlas_).width;
        std::vector<float> ad(size_t(as) * as);
        if (device_->read_texture(shadow_atlas_, ad.data(),
                                  ad.size() * sizeof(float)) ==
            ad.size() * sizeof(float)) {
            std::vector<uint8_t> ao(ad.size());
            for (size_t i = 0; i < ad.size(); i++)
                ao[i] = uint8_t(std::clamp(ad[i], 0.0f, 1.0f) * 255.0f);
            std::string ap = path;
            const size_t dot = ap.rfind('.');
            ap.insert(dot == std::string::npos ? ap.size() : dot, "_atlas");
            if (stbi_write_png(ap.c_str(), int(as), int(as), 1, ao.data(),
                               int(as)))
                WR_INFO("shadow dump: %s (atlas %ux%u, %zu tiles used)",
                        ap.c_str(), as, as, shadow_tiles_.size());
        }
    }
    return true;
}

// ------------------------------------------------------------- the frame

void Renderer::render(rhi::CommandList *cmd, SceneTree *tree, Camera3D *camera,
                      rhi::TextureH target) {
    if (!device_ || !cmd || !camera) return;
    double t0 = Clock::now();
    const uint64_t bakes = env_bakes_;
    stats_ = RenderStats();
    stats_.environment_bakes = bakes;
    view_cursor_ = 0;
    portal_cursor_ = 0;
    clustered_views_ = 0;
    camera_ = camera;

    TextureDesc td = device_->texture_desc(target);
    if (td.width != width_ || td.height != height_)
        if (!create_targets(td.width, td.height)) return;

    collect(tree, camera->cull_mask());

    float aspect = height_ ? float(width_) / float(height_) : 1.0f;
    View root;
    root.camera = camera->global_transform();
    root.projection = camera->projection(aspect);
    root.depth = 0;

    // THE CASCADES ARE FITTED BEFORE ANYTHING IS DRAWN, and to every
    // view -- the camera's and every portal view the frame will
    // recurse into. Walking the recursion twice costs a few hundred
    // microseconds of frustum arithmetic and no draws; not doing it
    // leaves the world beyond a portal lit by a cascade fitted to
    // somewhere else, which is most of what the player is looking at.
    cascade_count_ = 0;
    if (settings_.shadows && pipe_.shadow.valid()) {
        std::vector<View> views;
        views.reserve(16);
        gather_views(root, &views);
        fit_cascades(views);
    }
    // THE ENVIRONMENT IS BAKED BEFORE THE FRAME UNIFORMS GO UP, and
    // from the frame uniforms of the PREVIOUS upload -- so the bake
    // needs its own, written first. It only happens when the sky has
    // actually changed.
    if (settings_.image_based_lighting && environment_is_stale()) {
        upload_frame();
        bake_environment(cmd);
    }
    upload_frame();
    if (cascade_count_ > 0) shadow_pass(cmd);
    // THE TILES ARE HANDED OUT BEFORE ANY VIEW IS UPLOADED, because
    // a view upload is what fills in a froxel grid and the grid
    // indexes lights whose shadow fields have to be final by then.
    allocate_punctual_shadows(root.camera.origin);
    punctual_shadow_pass(cmd);

    // --- the scene, into the HDR target
    {
        WR_GPU_SCOPE(cmd, "scene");
        RenderingInfo ri;
        ColourAttachment ca;
        ca.texture = colour_hdr_;
        ca.load = LoadOp::Clear;
        ca.clear = settings_.clear_colour;
        if (samples_ > 1) ca.resolve = colour_resolve_;
        ri.colour.push_back(ca);
        ri.has_depth = true;
        ri.depth.texture = depth_stencil_;
        // REVERSE-Z CLEARS TO ZERO, and the stencil to zero because
        // zero is the outermost recursion level.
        ri.depth.clear_depth = 0.0f;
        ri.depth.clear_stencil = 0;
        ri.width = width_;
        ri.height = height_;
        ri.name = "scene";
        cmd->begin_rendering(ri);
        Viewport vp;
        vp.width = float(width_);
        vp.height = float(height_);
        cmd->set_viewport(vp);
        render_view(cmd, root, 0);
        cmd->end_rendering();
    }

    // --- tonemap to the swapchain
    {
        WR_GPU_SCOPE(cmd, "tonemap");
        TextureH src = samples_ > 1 ? colour_resolve_ : colour_hdr_;
        cmd->texture_barrier(src, TextureUsage::ColourTarget, TextureUsage::Sampled);
        RenderingInfo ri;
        ColourAttachment ca;
        ca.texture = target;
        ca.load = LoadOp::DontCare;
        ri.colour.push_back(ca);
        ri.width = td.width;
        ri.height = td.height;
        ri.name = "tonemap";
        cmd->begin_rendering(ri);
        cmd->set_scissor({0, 0, td.width, td.height});
        cmd->bind_pipeline(pipe_.tonemap);
        cmd->bind_group(0, frame_group_);
        uint32_t zero = 0;
        cmd->bind_group(1, view_group_, &zero, 1);
        cmd->bind_group(2, tonemap_group_);
        PushUniforms pu{};
        pu.model = Projection::identity();
        pu.tint = Vec4(1, 1, 1, 1);
        pu.params = Vec4(settings_.exposure, 0, 0, 0);
        cmd->push_constants(&pu, sizeof(pu));
        cmd->draw(3);
        cmd->end_rendering();
    }

    // THE LIGHT BUFFERS GO UP LAST, and that is safe rather than
    // lucky. Nothing recorded this frame has executed yet -- the
    // command buffer is submitted by end_frame, after this returns --
    // and these are host-visible buffers written straight through
    // their mapping, exactly like the per-view uniform ring. Doing it
    // here is what lets each view's froxel grid be built as the
    // recursion reaches it, instead of having to predict the
    // traversal twice.
    stats_.clustered_views = uint32_t(clustered_views_);
    stats_.environment_bakes = env_bakes_;
    if (settings_.punctual_lights) upload_lights();

    stats_.cpu_ms = (Clock::now() - t0) * 1000.0;
}

}  // namespace wr
