// Manifold -- the portal mechanism, in isolation.
//
// Recursive portals are four stencil operations in a row, and if any
// one of them behaves differently on the two backends the picture is
// wrong in a way that is very hard to see the cause of inside a full
// renderer. So the sequence is tested on its own, with nothing else in
// the frame:
//
//   1. draw a background            stencil == 0
//   2. MARK a quad                  stencil == 0, pass -> increment
//   3. fill where the mark landed   stencil == 1   -> green
//   4. RESTORE                      stencil == 1, pass -> decrement
//   5. fill where the mark STILL is stencil == 1   -> blue
//
// Step 3 must cover exactly the quad. Step 5 must cover NOTHING: if
// any blue survives, the restore failed to take the mark back down,
// and a real portal would leave its stencil value behind for
// everything drawn afterwards to trip over.
#include <SDL2/SDL.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "platform/window.h"
#include "render/shaders/generated/shaders.h"
#include "rhi/rhi.h"

using namespace mf;
using namespace mf::rhi;

namespace {

constexpr uint32_t kSize = 128;

struct Vertex {
    Vec3 position;
    Color colour;
};

struct PushUniforms {
    Projection model;
    Vec4 tint;
    Vec4 params;
};

struct ViewUniforms {
    Projection view, proj, view_proj, inv_view, inv_proj;
    Vec4 eye, near_far;
    int32_t portal[4];
};

struct Counts {
    int background = 0;   // step 1 colour
    int inside = 0;       // step 3 colour
    int outside = 0;      // step 5 colour
    bool ok = false;
    std::string device;
};

Counts run(Backend backend, bool validation) {
    Counts out;
    WindowConfig wc;
    wc.backend = backend;
    wc.width = int(kSize);
    wc.height = int(kSize);
    wc.resizable = false;
    wc.debug = validation;
    wc.title = "portal stencil";
    Window window;
    if (!window.open(wc)) return out;
    SDL_HideWindow(window.sdl_window());

    DeviceDesc dd;
    dd.backend = backend;
    dd.window = window.sdl_window();
    dd.validation = validation;
    dd.vsync = false;
    Device *dev = create_device(dd);
    if (!dev) return out;
    out.device = dev->caps().device_name;

    TextureDesc cd;
    cd.width = cd.height = kSize;
    cd.format = Format::RGBA8;
    cd.usage = TextureUsage::ColourTarget | TextureUsage::TransferSrc |
               TextureUsage::Sampled;
    cd.name = "colour";
    TextureH colour = dev->create_texture(cd);

    TextureDesc zd;
    zd.width = zd.height = kSize;
    zd.format = Format::D32F_S8;
    zd.usage = TextureUsage::DepthTarget;
    zd.name = "depth stencil";
    TextureH depth = dev->create_texture(zd);

    BindGroupLayoutDesc fl;
    fl.entries.push_back({0, BindingType::UniformBuffer, true, true, false, 1});
    BindGroupLayoutH frame_layout = dev->create_bind_group_layout(fl);
    BindGroupLayoutDesc vl;
    vl.entries.push_back({8, BindingType::UniformBuffer, true, true, false, 1});
    BindGroupLayoutH view_layout = dev->create_bind_group_layout(vl);

    BufferDesc bd;
    bd.size = 1024;
    bd.usage = BufferUsage::Uniform;
    bd.access = MemoryAccess::CpuToGpu;
    bd.name = "ubo";
    BufferH frame_ubo = dev->create_buffer(bd);
    BufferH view_ubo = dev->create_buffer(bd);
    std::vector<uint8_t> zero(1024, 0);
    dev->write_buffer(frame_ubo, zero.data(), zero.size());

    ViewUniforms vu{};
    vu.view = to_projection(Transform3D::identity());
    vu.proj = Projection::orthographic(-1, 1, -1, 1, 0.1f, 100.0f);
    vu.view_proj = vu.proj;
    vu.inv_view = vu.view;
    vu.inv_proj = vu.proj.inverse();
    vu.near_far = Vec4(0.1f, 100.0f, 10.0f, 0.0f);
    dev->write_buffer(view_ubo, &vu, sizeof(vu));

    BindGroupDesc fg;
    fg.layout = frame_layout;
    fg.entries.push_back({0, frame_ubo, 0, 0, {}, {}, 0, -1});
    BindGroupH frame_group = dev->create_bind_group(fg);
    BindGroupDesc vg;
    vg.layout = view_layout;
    vg.entries.push_back({8, view_ubo, 0, 0, {}, {}, 0, -1});
    BindGroupH view_group = dev->create_bind_group(vg);

    ShaderH vs = dev->create_shader(
        shaders::desc(*shaders::find("parity", ShaderStage::Vertex)));
    ShaderH fs = dev->create_shader(
        shaders::desc(*shaders::find("parity", ShaderStage::Fragment)));
    ShaderH full_vs = dev->create_shader(
        shaders::desc(*shaders::find("fullscreen", ShaderStage::Vertex)));
    ShaderH full_fs = dev->create_shader(
        shaders::desc(*shaders::find("fullscreen", ShaderStage::Fragment)));

    VertexLayout layout;
    layout.bindings.push_back({0, uint32_t(sizeof(Vertex)), false});
    layout.attributes.push_back({0, 0, Format::RGB32F, 0});
    layout.attributes.push_back({1, 0, Format::RGBA32F, uint32_t(offsetof(Vertex, colour))});

    StencilFace equal;
    equal.compare = CompareOp::Equal;
    equal.pass = StencilOp::Keep;
    equal.write_mask = 0;

    PipelineDesc base;
    base.vertex = vs;
    base.fragment = fs;
    base.vertex_layout = layout;
    base.colour_formats = {Format::RGBA8};
    base.depth_format = Format::D32F_S8;
    base.bind_group_layouts = {frame_layout, view_layout};
    base.push_constant_size = sizeof(PushUniforms);
    base.depth_stencil.stencil_test = true;
    base.depth_stencil.front = equal;
    base.depth_stencil.back = equal;
    base.blend = {BlendState::opaque()};
    base.name = "background";
    PipelineH background = dev->create_pipeline(base);

    // 2. MARK.
    PipelineDesc md = base;
    md.depth_stencil.depth_write = false;
    StencilFace mark;
    mark.compare = CompareOp::Equal;
    mark.pass = StencilOp::IncrementClamp;
    md.depth_stencil.front = mark;
    md.depth_stencil.back = mark;
    md.blend = {BlendState::no_colour()};
    md.name = "mark";
    PipelineH mark_pipe = dev->create_pipeline(md);

    // 3 and 5. Full-screen fills, gated on the stencil.
    PipelineDesc fd;
    fd.vertex = full_vs;
    fd.fragment = full_fs;
    fd.colour_formats = {Format::RGBA8};
    fd.depth_format = Format::D32F_S8;
    fd.bind_group_layouts = {frame_layout, view_layout};
    fd.push_constant_size = sizeof(PushUniforms);
    fd.raster.cull = CullMode::None;
    fd.depth_stencil.depth_test = false;
    fd.depth_stencil.depth_write = false;
    fd.depth_stencil.stencil_test = true;
    fd.depth_stencil.front = equal;
    fd.depth_stencil.back = equal;
    fd.blend = {BlendState::opaque()};
    fd.name = "stencil fill";
    PipelineH fill = dev->create_pipeline(fd);

    // 4. RESTORE.
    PipelineDesc rd = md;
    StencilFace restore;
    restore.compare = CompareOp::Equal;
    restore.pass = StencilOp::DecrementClamp;
    rd.depth_stencil.front = restore;
    rd.depth_stencil.back = restore;
    rd.depth_stencil.depth_compare = CompareOp::Always;
    rd.depth_stencil.depth_write = true;
    rd.name = "restore";
    PipelineH restore_pipe = dev->create_pipeline(rd);

    // A quad covering the middle of the screen, wound counter-clockwise.
    std::vector<Vertex> verts = {{{-0.5f, -0.5f, -1.0f}, Color(1, 1, 1, 1)},
                                 {{0.5f, -0.5f, -1.0f}, Color(1, 1, 1, 1)},
                                 {{0.5f, 0.5f, -1.0f}, Color(1, 1, 1, 1)},
                                 {{-0.5f, 0.5f, -1.0f}, Color(1, 1, 1, 1)}};
    std::vector<uint32_t> idx = {0, 1, 2, 0, 2, 3};
    BufferDesc vbd;
    vbd.size = verts.size() * sizeof(Vertex);
    vbd.usage = BufferUsage::Vertex;
    BufferH vb = dev->create_buffer(vbd, verts.data());
    BufferDesc ibd;
    ibd.size = idx.size() * sizeof(uint32_t);
    ibd.usage = BufferUsage::Index;
    BufferH ib = dev->create_buffer(ibd, idx.data());

    CommandList *cmd = dev->begin_frame();
    if (!cmd) {
        destroy_device(dev);
        return out;
    }
    RenderingInfo ri;
    ColourAttachment ca;
    ca.texture = colour;
    ca.load = LoadOp::Clear;
    ca.clear = Color(0, 0, 0, 1);
    ri.colour.push_back(ca);
    ri.has_depth = true;
    ri.depth.texture = depth;
    ri.depth.clear_depth = 0.0f;
    ri.depth.clear_stencil = 0;
    ri.width = ri.height = kSize;
    ri.name = "portal stencil";
    cmd->begin_rendering(ri);

    PushUniforms pu{};
    pu.model = Projection::identity();

    auto quad_draw = [&](PipelineH p, uint32_t ref) {
        cmd->bind_pipeline(p);
        cmd->set_stencil_reference(ref);
        cmd->bind_group(0, frame_group);
        cmd->bind_group(1, view_group);
        cmd->push_constants(&pu, sizeof(pu));
        cmd->bind_vertex_buffer(0, vb);
        cmd->bind_index_buffer(ib, IndexType::U32);
        cmd->draw_indexed(uint32_t(idx.size()));
    };
    auto fill_draw = [&](uint32_t ref, const Color &c) {
        cmd->bind_pipeline(fill);
        cmd->set_stencil_reference(ref);
        cmd->bind_group(0, frame_group);
        cmd->bind_group(1, view_group);
        PushUniforms f{};
        f.model = Projection::identity();
        f.tint = c.rgba();
        f.params = Vec4(0.0f, 0, 0, 0);
        cmd->push_constants(&f, sizeof(f));
        cmd->draw(3);
    };

    // 1. background: dark red everywhere, stencil 0.
    pu.tint = Vec4(0.5f, 0.1f, 0.1f, 1.0f);
    cmd->bind_pipeline(background);
    cmd->set_stencil_reference(0);
    cmd->bind_group(0, frame_group);
    cmd->bind_group(1, view_group);
    {
        PushUniforms bg{};
        bg.model = Projection::identity();
        bg.tint = Vec4(0.5f, 0.1f, 0.1f, 1.0f);
        // A big quad behind the portal quad.
        Transform3D t(Basis::scaled({4, 4, 1}), Vec3(0, 0, -2.0f));
        bg.model = to_projection(t);
        cmd->push_constants(&bg, sizeof(bg));
        cmd->bind_vertex_buffer(0, vb);
        cmd->bind_index_buffer(ib, IndexType::U32);
        cmd->draw_indexed(uint32_t(idx.size()));
    }

    // 2. mark.
    pu.tint = Vec4(1, 1, 1, 1);
    quad_draw(mark_pipe, 0);
    // 3. fill inside: green.
    fill_draw(1, Color(0, 1, 0, 1));
    // 4. restore.
    quad_draw(restore_pipe, 1);
    // 5. fill where the mark still stands: blue. After the restore
    //    there should be none, so this must paint nothing at all.
    fill_draw(1, Color(0, 0, 1, 1));

    cmd->end_rendering();
    dev->end_frame();
    dev->wait_idle();

    std::vector<uint8_t> px(size_t(kSize) * kSize * 4);
    if (dev->read_texture(colour, px.data(), px.size()) == px.size()) {
        for (size_t i = 0; i < px.size(); i += 4) {
            if (px[i] > 100 && px[i + 1] < 80) out.background++;
            if (px[i + 1] > 100 && px[i] < 80 && px[i + 2] < 80) out.inside++;
            if (px[i + 2] > 100 && px[i] < 80 && px[i + 1] < 80) out.outside++;
        }
        out.ok = true;
    }
    destroy_device(dev);
    return out;
}

}  // namespace

int main(int argc, char **argv) {
    bool validation = true;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--no-validation")) validation = false;
    log_set_level(LogLevel::Warn);

    std::printf("portal stencil mechanism\n");
    const uint32_t total = kSize * kSize;
    // The quad is half the screen in each axis, so a quarter of it.
    const int expect_quad = int(total / 4);

    int failures = 0, ran = 0;
    Counts results[2];
    Backend backends[2] = {Backend::OpenGL, Backend::Vulkan};
    for (int i = 0; i < 2; i++) {
#if !MANIFOLD_OPENGL
        if (backends[i] == Backend::OpenGL) continue;
#endif
#if !MANIFOLD_VULKAN
        if (backends[i] == Backend::Vulkan) continue;
#endif
        Counts c = run(backends[i], validation);
        results[i] = c;
        if (!c.ok) {
            std::printf("  %-7s unavailable\n", backend_name(backends[i]));
            continue;
        }
        ran++;
        std::printf("  %-7s %s\n", backend_name(backends[i]), c.device.c_str());
        std::printf("          marked %d px (expected about %d), "
                    "left over after restore %d, background %d\n",
                    c.inside, expect_quad, c.outside, c.background);
        // Step 3 must land on the quad and nowhere else.
        if (std::abs(c.inside - expect_quad) > expect_quad / 10) {
            std::printf("  FAIL  [%s] the stencil MARK did not cover the quad\n",
                        backend_name(backends[i]));
            failures++;
        }
        // Nothing may survive the restore.
        if (c.outside > 0) {
            std::printf("  FAIL  [%s] the stencil RESTORE left %d marked pixels\n",
                        backend_name(backends[i]), c.outside);
            failures++;
        }
        // And the background must still be everything the quad is not.
        if (c.background < int(total) - expect_quad - int(total) / 20) {
            std::printf("  FAIL  [%s] the background was overwritten (%d left)\n",
                        backend_name(backends[i]), c.background);
            failures++;
        }
    }

    if (ran == 0) return 77;
    if (ran == 2 && results[0].ok && results[1].ok) {
        if (std::abs(results[0].inside - results[1].inside) > 16) {
            std::printf("  FAIL  the backends disagree: %d vs %d marked pixels\n",
                        results[0].inside, results[1].inside);
            failures++;
        }
    }
    std::printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
