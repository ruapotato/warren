// Manifold -- the frame.
//
// PORTALS ARE NOT A POST-PROCESS HERE. They are not a texture rendered
// from a second camera and pasted onto a quad, which is how an engine
// without a writable projection matrix has to do it and which brings
// with it a resolution to choose, a seam to hide, anti-aliasing that
// does not match, and a wall behind the far portal that no amount of
// bias quite removes.
//
// Instead, the inner view is drawn INTO THE SAME FRAMEBUFFER, masked
// by the stencil buffer and clipped by an oblique near plane lying
// exactly in the destination aperture. That is the technique Portal
// used in 2007, it is exact, and it needs two things the engine was
// built to provide: eight bits of stencil, and a camera that is a
// matrix rather than a field of view.
//
// The cost is that each recursion level redraws the scene. The
// scissor rectangle around each portal's screen bounds is what makes
// that affordable -- a portal covering a twentieth of the screen costs
// a twentieth of a frame, not a frame.
#pragma once

#include <unordered_map>
#include <vector>

#include "render/material.h"
#include "render/mesh.h"
#include "rhi/rhi.h"
#include "scene/nodes.h"
#include "scene/portal.h"

namespace mf {

class SceneTree;

struct RenderSettings {
    // How many portals deep to go. Each level costs a scissored redraw
    // of what is visible through the one above, so the useful range is
    // 1 to 6; beyond that the inner image is a few pixels across.
    int max_portal_depth = 4;
    // Scissor each recursion to the portal's screen bounds. Off only
    // for debugging -- it is the single biggest saving in the system.
    bool portal_scissor = true;
    // Tint each recursion level, so a capture shows how deep it went.
    bool portal_debug_tint = false;
    // Skip a portal covering less than this fraction of the screen.
    float portal_min_coverage = 0.0002f;

    int msaa = 4;
    float exposure = 1.0f;
    Color clear_colour = Color(0.05f, 0.06f, 0.08f, 1.0f);
    bool draw_sky = true;
};

struct RenderStats {
    uint32_t draw_calls = 0;
    uint32_t triangles = 0;
    uint32_t views = 0;             // 1 plus one per portal rendered
    uint32_t portals_considered = 0;
    uint32_t portals_culled = 0;
    uint32_t max_depth_reached = 0;
    uint32_t visible_meshes = 0;
    double cpu_ms = 0.0;
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    bool init(rhi::Device *dev, const RenderSettings &settings = {});
    void shutdown();
    void resize(uint32_t width, uint32_t height);

    // Draws the scene as seen by `camera` into `target`, which is
    // normally the swapchain image.
    void render(rhi::CommandList *cmd, SceneTree *tree, Camera3D *camera,
                rhi::TextureH target);

    RenderSettings &settings() { return settings_; }
    const RenderStats &stats() const { return stats_; }

    // The sun and the ambient light. A scene with no DirectionalLight3D
    // uses these directly.
    Vec3 sun_direction{-0.35f, -0.8f, -0.48f};
    Color sun_colour = Color(1.0f, 0.96f, 0.88f, 1.0f);
    float sun_energy = 3.2f;
    Color ambient = Color(0.30f, 0.38f, 0.5f, 1.0f);
    float ambient_energy = 0.55f;
    Color fog_colour = Color(0.52f, 0.60f, 0.70f, 1.0f);
    float fog_density = 0.004f;
    float fog_height_falloff = 0.0f;

    rhi::Device *device() const { return device_; }
    Material *default_material() const { return default_material_.get(); }

private:
    // One camera's worth of state. A portal makes another of these.
    struct View {
        Transform3D camera;
        Projection projection;
        // The oblique near plane, in view space, when this view is
        // looking out of a portal.
        bool has_clip = false;
        Plane clip;
        // Pixels, not fractions.
        bool has_scissor = false;
        rhi::Rect scissor;
        int depth = 0;
        int portal_id = -1;
    };

    struct Renderable {
        Mesh *mesh = nullptr;
        const SubMesh *sub = nullptr;
        Material *material = nullptr;
        Transform3D model;
        AABB bounds;
        Color tint;
        uint64_t key = 0;
    };

    bool create_targets(uint32_t w, uint32_t h);
    void destroy_targets();
    bool create_pipelines();
    void collect(SceneTree *tree, uint32_t cull_mask);
    void render_view(rhi::CommandList *cmd, const View &view, uint32_t stencil_ref);
    void draw_geometry(rhi::CommandList *cmd, const View &view, uint32_t stencil_ref,
                       MaterialPass pass, const Plane frustum[6]);
    // Writes one view's uniforms into the ring and returns the dynamic
    // offset to bind with.
    uint32_t upload_view(const View &view);
    uint32_t upload_portal(const Portal3D *p);
    void upload_frame();

    rhi::Device *device_ = nullptr;
    RenderSettings settings_;
    RenderStats stats_;
    uint32_t width_ = 0, height_ = 0;
    uint32_t samples_ = 1;

    // HDR, because the sky and an emissive surface both go well past
    // one and clipping them at the point of shading loses the bloom
    // and the tonemap both.
    rhi::TextureH colour_hdr_;
    rhi::TextureH colour_resolve_;
    rhi::TextureH depth_stencil_;
    rhi::TextureH shadow_map_;

    rhi::BindGroupLayoutH frame_layout_, view_layout_, material_layout_,
        portal_layout_, tonemap_layout_;
    rhi::BindGroupH frame_group_, view_group_, portal_group_, tonemap_group_;
    rhi::BufferH frame_ubo_, view_ubo_, portal_ubo_;
    uint32_t view_stride_ = 0, portal_stride_ = 0;
    uint32_t view_cursor_ = 0, portal_cursor_ = 0;

    struct Pipelines {
        rhi::PipelineH mesh_opaque, mesh_opaque_ds;
        rhi::PipelineH mesh_cutout, mesh_cutout_ds;
        rhi::PipelineH mesh_blend, mesh_blend_ds;
        rhi::PipelineH sky;
        rhi::PipelineH portal_mark;
        rhi::PipelineH portal_depth_clear;
        rhi::PipelineH portal_restore;
        rhi::PipelineH portal_rim;
        rhi::PipelineH tonemap;
    } pipe_;

    Ref<Mesh> portal_quad_;
    Ref<Material> default_material_;
    std::vector<Renderable> renderables_;
    std::vector<Portal3D *> portals_;
    Camera3D *camera_ = nullptr;
};

}  // namespace mf
