// Warren -- the frame.
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

#include <string>
#include <unordered_map>
#include <vector>

#include "render/material.h"
#include "render/mesh.h"
#include "rhi/rhi.h"
#include "scene/nodes.h"
#include "scene/portal.h"

namespace wr {

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
    // AND ASK FOR MORE OF THE SCREEN AT EVERY LEVEL DOWN.
    //
    // A recursion level costs a whole scene draw whatever depth
    // it is at, and is worth less the deeper it goes: the second
    // reflection in a pair of portals is a small patch inside a
    // small patch, and nobody looking at the room can tell it
    // from the wall. With one threshold for every level the
    // deepest views were free to cost the same as the first, and
    // the frames that drew them were the frames that missed
    // vsync -- 27.7 ms at the 95th percentile against 18.2 for
    // the frames that did not, on a scene whose CPU cost is five
    // milliseconds. That is the "frame hiccup going between
    // portals": not the crossing, the extra view you are looking
    // at while you cross.
    //
    // Multiplying the threshold by this at each level keeps the
    // first recursion anywhere it is worth having and drops the
    // rest. It is a function of screen area alone, so it changes
    // smoothly as you move rather than flickering the way a
    // per-frame budget would.
    float portal_depth_falloff = 6.0f;

    // SHADOWS.
    //
    // Cascades are fitted to every view the frame will draw, portal
    // views included -- see Renderer::fit_cascades. An engine that
    // fits them to the main camera alone leaves everything seen
    // through a portal either unshadowed or shadowed by a cascade
    // meant for somewhere else, and in this engine that is most of
    // the interesting geometry.
    bool shadows = true;
    int shadow_cascades = 4;            // 1..4; the shader indexes four
    uint32_t shadow_map_size = 2048;    // per cascade, square
    float shadow_distance = 120.0f;     // beyond this, nothing is shadowed
    // 0 is an even split by distance, 1 is logarithmic. Logarithmic is
    // right for the texel density and wrong for the far cascades'
    // size, so the practical answer is between the two.
    float shadow_split_lambda = 0.75f;
    // Slope-scaled, because acne is worst where the surface is nearly
    // edge-on to the light and a constant bias there is either useless
    // or large enough to detach the shadow.
    float shadow_bias_constant = 1.25f;
    float shadow_bias_slope = 2.75f;
    // How far behind the cascade's slice the light starts drawing, so
    // a caster outside the view still casts into it.
    float shadow_caster_extrusion = 60.0f;

    // PUNCTUAL LIGHTS.
    //
    // Clustered, and clustered per view -- a portal view is a
    // different camera looking at different geometry through the same
    // pixels, so it needs its own froxel grid. Views past this many
    // (the deepest recursion levels, which occupy a handful of pixels)
    // fall back to the sun and the ambient alone.
    // IMAGE-BASED LIGHTING.
    //
    // The sky is baked into two cubemaps -- cosine-convolved for
    // diffuse, GGX-prefiltered per mip for specular -- and rebaked
    // when the sun moves. Nothing to author and nothing to ship: the
    // environment is the same function the backdrop is drawn from.
    bool image_based_lighting = true;
    uint32_t env_size = 128;        // the specular cube's top mip
    uint32_t env_irradiance_size = 32;
    float env_intensity = 1.0f;

    bool punctual_lights = true;
    int max_clustered_views = 16;
    int max_lights = 256;
    // SHADOWS FOR PUNCTUAL LIGHTS, in one atlas of square tiles. A
    // spot takes one tile and an omni six, so the default 4x4 grid
    // is two omnis and four spots, or one omni and ten spots. Lights
    // are given tiles nearest-first; the rest still light, they just
    // do not cast.
    bool punctual_shadows = true;
    uint32_t shadow_atlas_size = 2048;
    // 512, AND THE TILE COUNT IS A BUDGET RATHER THAN A LIMIT.
    //
    // An omni wants six tiles, so a 2048 atlas of these holds
    // sixteen: two shadow-casting point lights, whatever the
    // scene. That looks like a ceiling worth raising and it is
    // not. Dropping the tile to 256 makes it sixty-four tiles
    // and ten lights, and measured on a thirteen-light room it
    // took the median frame from 16.3 ms to 22.1 -- because the
    // cost is the NUMBER of shadowed lights, in tile draws and
    // again in per-pixel lookups, and not the resolution of any
    // one of them. The small number is doing useful work.
    //
    // What does need fixing is the CHURN: see
    // allocate_punctual_shadows, which now keeps a light's tiles
    // unless something is decisively closer.
    uint32_t shadow_tile_size = 512;
    // Nearer than this to the light's own centre, nothing casts. Too
    // small and the depth range is wasted; too large and a lamp
    // inside a fitting shadows itself away.
    float punctual_shadow_near = 0.12f;

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
    uint32_t shadow_draws = 0;
    uint32_t cascades = 0;
    uint32_t lights = 0;            // punctual, after culling
    uint32_t skinned_bones = 0;     // bone matrices uploaded this frame
    uint32_t shadow_casting_lights = 0;
    uint32_t punctual_shadow_draws = 0;
    bool punctual_shadows_reused = false;
    uint32_t light_assignments = 0; // light-froxel pairs binned
    // FROXELS THAT WANTED MORE LIGHTS THAN THEY CAN HOLD. Not an
    // error -- the replacement rule keeps the ones that matter --
    // but it is the thing to look at when the picture has
    // tile-shaped steps in it, because that is what a silent
    // overflow looks like.
    uint32_t cluster_overflows = 0;
    uint32_t clustered_views = 0;
    // Counts up over the session, not per frame: an environment that
    // rebakes every frame is a bug that costs milliseconds and is
    // otherwise invisible.
    uint64_t environment_bakes = 0;
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

    // The cascade maps, side by side, as 8-bit grey: white is close
    // to the light and black is the far plane (reverse-Z), so an
    // empty cascade is black. Worth having permanently -- a shadow
    // bug is otherwise diagnosed by staring at the lit result and
    // guessing which of the fit, the pass and the lookup is wrong.
    bool dump_shadow_map(const std::string &path) const;
    // The punctual atlas as raw depth, for a test that wants to
    // compare it between backends rather than look at a picture of
    // it. Returns the side length, or 0.
    uint32_t read_shadow_atlas(std::vector<float> *out) const;

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
        // THE DEPTH RANGE THIS VIEW IS REALLY A SLICE OF.
        //
        // An oblique near plane rewrites the projection's third
        // row, which is exactly the two entries get_z_near and
        // get_z_far read back. So a portal view's projection LIES
        // about its depth range: ask it and you get a number that
        // depends on where the oblique cut happened to fall
        // rather than on the camera it was warped from, and when
        // the cut is behind the eye you get a negative one.
        //
        // Clustered lighting slices the depth range with that
        // number. A bad near put every fragment of the portal
        // view into one froxel slice, which held the lights for
        // some other depth or none -- so the view through a
        // portal came out darker than the room it was showing,
        // and how much darker depended on the angle.
        //
        // Recorded before the cut, and zero means "ask the
        // projection", which is right for every view that has
        // not been cut.
        float z_near = 0.0f;
        float z_far = 0.0f;
        float near_of() const {
            return z_near > 0.0f ? z_near : projection.get_z_near();
        }
        float far_of() const {
            return z_far != 0.0f ? z_far : projection.get_z_far();
        }
        // A shadow cascade is uploaded through this same struct, and
        // it must not take a froxel grid: it is the light's view, not
        // a view lights are gathered for.
        bool clustered = true;
        // What this view is allowed to see. The primary view takes
        // the camera's mask; a portal view takes the portal's, so
        // a game can show through a hole what it hides from the
        // eye.
        uint32_t cull_mask = 0xFFFFFFFFu;
    };

    struct Renderable {
        Mesh *mesh = nullptr;
        const SubMesh *sub = nullptr;
        Material *material = nullptr;
        Transform3D model;
        AABB bounds;
        Color tint;
        uint64_t key = 0;
        // WHERE THIS BODY'S BONES START in the frame's one bone
        // buffer, or -1 for a mesh that has none. Every skinned body
        // in the frame writes its matrices end to end into that
        // buffer and the draw passes its own offset as a push
        // constant, so twenty characters cost one upload and no
        // per-draw binding.
        int bone_base = -1;
        // WHICH VIEWS MAY DRAW IT. Kept per renderable rather than
        // filtered away at collection, because a mesh can be
        // wanted in one view and not another -- a first-person
        // body is hidden from the eye that is inside its head and
        // still has to appear through a portal, where seeing
        // yourself walk up to the other side is the entire point.
        uint32_t layers = 0xFFFFFFFFu;
    };

    // Walks the portal recursion without drawing anything, so the
    // shadow cascades can be fitted to every view the frame is about
    // to render rather than only to the camera's.
    void upload_bones();
    void gather_views(const View &v, std::vector<View> *out) const;
    // The one copy of "does this portal recurse from here, and with
    // what camera" -- used by the gather walk and by the draw.
    void draw_orphan_rim(rhi::CommandList *cmd, const View &v, size_t pi,
                         uint32_t view_offset, uint32_t stencil_ref);
    bool portal_child(const View &v, size_t portal_index, View *out,
                      rhi::Rect *scissor) const;
    void fit_cascades(const std::vector<View> &views);
    // Fills the froxel grid for one view. Returns the base index of
    // its block, or -1 if the frame has run out of room for grids.
    int cluster_view(const View &v, int slot);
    void upload_lights();
    // Six faces of sky, then the two convolutions. Costs a few
    // milliseconds and happens when the sun has moved enough to
    // matter, not every frame.
    void bake_environment(rhi::CommandList *cmd);
    bool environment_is_stale() const;
    void shadow_pass(rhi::CommandList *cmd);
    // Hands out atlas tiles to the nearest shadow-casting lights and
    // fills in each light's shadow fields. Returns how many tiles
    // were used.
    int allocate_punctual_shadows(const Vec3 &eye);
    void punctual_shadow_pass(rhi::CommandList *cmd);
    // What the atlas's contents depend on: every shadow-casting
    // light, and every caster's mesh and place in the world. Equal
    // hashes mean the atlas already holds the right answer.
    uint64_t shadow_hash() const;

    bool create_targets(uint32_t w, uint32_t h);
    void destroy_targets();
    bool create_pipelines();
    rhi::PipelineH tonemap_for(rhi::Format format);
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
    // A one-texel stand-in bound in place of the real map while the
    // real map is the thing being rendered into. Sampling a texture
    // that is currently a depth attachment is a feedback loop; binding
    // nothing at all is a validation error. This is neither.
    rhi::TextureH shadow_dummy_;
    uint32_t shadow_size_ = 0;
    int cascade_count_ = 0;
    Projection cascade_view_proj_[4];
    Transform3D cascade_camera_[4];
    Projection cascade_projection_[4];
    Vec4 cascade_splits_{0, 0, 0, 0};
    Vec4 cascade_texel_{0, 0, 0, 0};

    rhi::BindGroupLayoutH frame_layout_, view_layout_, material_layout_,
        portal_layout_, tonemap_layout_;
    // THE CURRENT SLOT'S GROUPS. Every call site binds these; they
    // are pointed at the right slot once per frame, in render().
    rhi::BindGroupH frame_group_, view_group_, portal_group_, tonemap_group_;
    rhi::BindGroupH frame_group_no_shadow_;
    // ONE SET PER FRAME IN FLIGHT. See Device::frame_slot: a buffer
    // written every frame cannot be a single buffer, because the CPU
    // writes frame N while the GPU is still reading frame N-1.
    std::vector<rhi::BindGroupH> frame_groups_, frame_groups_no_shadow_;
    std::vector<rhi::BindGroupH> bone_groups_;
    uint32_t ring_ = 1;          // frames in flight
    uint32_t slot_ = 0;          // which one is being recorded
    // Bytes of each per-frame buffer belonging to one slot.
    uint64_t frame_span_ = 0, view_span_ = 0, portal_span_ = 0;
    uint64_t light_span_ = 0, cluster_span_ = 0, light_index_span_ = 0;
    uint64_t bone_span_ = 0;
    rhi::BufferH light_buffer_, cluster_buffer_, light_index_buffer_;
    // Every skinned body in the frame, end to end: three rows per
    // bone, one upload, one bind. See Renderable::bone_base.
    rhi::BufferH bone_buffer_;
    rhi::BindGroupH bone_group_;
    rhi::BindGroupLayoutH bone_layout_;
    std::vector<Vec4> bone_rows_;
    uint32_t bone_capacity_ = 0;
    rhi::TextureH env_cube_, env_irradiance_, env_specular_;
    rhi::TextureH shadow_atlas_, shadow_atlas_dummy_;
    uint32_t atlas_tiles_per_row_ = 0;
    uint64_t atlas_hash_ = 0;
    bool atlas_valid_ = false;
    rhi::BindGroupLayoutH env_layout_;
    rhi::BindGroupH env_group_, env_specular_group_;
    uint32_t env_mips_ = 0;
    uint64_t env_bakes_ = 0;
    bool env_baked_ = false;
    // What the environment was baked for. The sky is a function of
    // these and nothing else, so comparing them is exactly the test
    // for whether the bake is still valid.
    Vec3 baked_sun_{0, 0, 0};
    Color baked_sun_colour_, baked_ambient_, baked_fog_;
    float baked_sun_energy_ = -1.0f;
    rhi::BufferH frame_ubo_, view_ubo_, portal_ubo_;
    uint32_t view_stride_ = 0, portal_stride_ = 0;
    uint32_t view_cursor_ = 0, portal_cursor_ = 0;

    struct Pipelines {
        rhi::PipelineH mesh_opaque, mesh_opaque_ds;
        rhi::PipelineH mesh_skinned, mesh_skinned_ds;
        rhi::PipelineH mesh_cutout, mesh_cutout_ds;
        rhi::PipelineH mesh_blend, mesh_blend_ds;
        rhi::PipelineH sky;
        rhi::PipelineH portal_mark;
        rhi::PipelineH portal_depth_clear;
        rhi::PipelineH portal_restore;
        rhi::PipelineH portal_rim;
        // ONE PER TARGET FORMAT. The tonemap is the only pass that
        // writes to a texture the caller chose, and a graphics
        // pipeline is compiled against the format it writes to. A
        // renderer that only ever drew to the swapchain got away with
        // a single pipeline; rendering to an offscreen target of any
        // other format is invalid, silently on a release driver and
        // loudly under validation.
        std::unordered_map<int, rhi::PipelineH> tonemap;
        rhi::PipelineH shadow, shadow_ds;
        rhi::PipelineH punctual, punctual_ds;
        rhi::PipelineH skycube, irradiance, prefilter;
    } pipe_;

    // Exactly the bytes of the Light struct in common.glsl.
    struct LightGpu {
        Vec4 position_range;
        Vec4 colour_energy;
        Vec4 direction_cone;
        Vec4 params;
        Vec4 shadow;
        // One per cube face; a spot uses only the first.
        Projection shadow_view_proj[6];
    };
    static_assert(sizeof(LightGpu) == 464, "must match Light in common.glsl");

    // One entry per atlas tile that has to be rendered this frame.
    struct ShadowTile {
        uint32_t tile = 0;
        Transform3D camera;
        Projection projection;
    };
    std::vector<LightGpu> lights_;
    std::vector<ShadowTile> shadow_tiles_;
    // Mirrors of the GPU buffers, filled on the CPU and uploaded once.
    std::vector<uint32_t> cluster_counts_;
    std::vector<uint32_t> cluster_indices_;
    int clustered_views_ = 0;
    // WHICH LIGHTS HELD SHADOW TILES LAST FRAME, by position --
    // an index would go stale the moment the light list is
    // rebuilt, which it is every frame. See
    // allocate_punctual_shadows.
    std::vector<Vec3> shadow_holders_;
    // The sun, after the scene has had its say: a DirectionalLight3D
    // in the tree overrides the renderer's own fields.
    Vec3 sun_dir_used_{0, -1, 0};
    Color sun_colour_used_ = Color::white();
    float sun_energy_used_ = 1.0f;

    Ref<Mesh> portal_quad_;
    Ref<Material> default_material_;
    std::vector<Renderable> renderables_;
    std::vector<Portal3D *> portals_;
    Camera3D *camera_ = nullptr;
};

}  // namespace wr
