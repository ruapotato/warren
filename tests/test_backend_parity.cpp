// Warren -- do the two backends agree?
//
// "Vulkan and OpenGL are both first class" is a claim, and this is what
// makes it checkable. The same scene is built through the same RHI
// calls on both, rendered offscreen, read back, and compared pixel for
// pixel. Anything the two disagree about -- the direction of +Y, the
// winding of a front face, the sense of the depth test, what the
// stencil buffer does -- shows up here as a number instead of as a
// bug report six months later saying "it looks wrong on my laptop".
//
// It runs headless on a hidden window, so it belongs in CI. On a
// machine with no GPU at all, llvmpipe answers for both.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/log.h"
#include <SDL2/SDL.h>

#include "platform/window.h"
#include "render/shaders/generated/shaders.h"
#include "rhi/rhi.h"

using namespace wr;
using namespace wr::rhi;

namespace {

constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 256;

struct Vertex {
    Vec3 position;
    Color colour;
};

// A scene chosen so that every convention leaves a mark.
//
//  * two overlapping triangles at different depths, so reverse-Z is
//    visible: get the depth test backwards and the wrong one wins;
//  * one wound counter-clockwise and one clockwise, so back-face
//    culling is visible: get the winding wrong and one vanishes;
//  * everything off-centre and asymmetric in y, so a flipped Y axis
//    cannot hide;
//  * a stencil-masked quad over the top, which is the mechanism the
//    whole portal renderer is built on.
struct Scene {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

// ORTHOGRAPHIC, deliberately: with the camera at the origin and an
// ortho box of [-1, 1], a world coordinate IS its NDC coordinate. That
// removes every question of what got clipped or landed off-screen, and
// leaves the test measuring only the four conventions it is about.
Scene build_scene() {
    Scene s;
    auto tri = [&](Vec3 a, Vec3 b, Vec3 c, Color col) {
        uint32_t base = uint32_t(s.vertices.size());
        s.vertices.push_back({a, col});
        s.vertices.push_back({b, col});
        s.vertices.push_back({c, col});
        s.indices.push_back(base);
        s.indices.push_back(base + 1);
        s.indices.push_back(base + 2);
    };
    // 1. RED, counter-clockwise in NDC (positive signed area), so
    //    front-facing, and NEAR.
    tri({-0.8f, -0.6f, -2.0f}, {0.2f, -0.6f, -2.0f}, {-0.3f, 0.4f, -2.0f},
        Color(0.9f, 0.2f, 0.15f, 1));
    // 2. BLUE, also front-facing, FURTHER AWAY and overlapping the red
    //    one. Under reverse-Z with GREATER_EQUAL, red must win where
    //    they cross. Get the compare backwards and blue does.
    tri({-0.6f, -0.75f, -6.0f}, {0.5f, -0.75f, -6.0f}, {0.0f, 0.25f, -6.0f},
        Color(0.15f, 0.35f, 0.9f, 1));
    // 3. GREEN, CLOCKWISE: signed area (v1-v0) x (v2-v1) is negative,
    //    so it is back-facing and must be culled entirely.
    tri({-0.9f, 0.9f, -2.0f}, {-0.3f, 0.9f, -2.0f}, {-0.9f, 0.35f, -2.0f},
        Color(0.1f, 0.9f, 0.2f, 1));
    // 4. YELLOW, front-facing, in the UPPER RIGHT and nowhere near the
    //    centre, so a flipped Y axis cannot be mistaken for anything
    //    else.
    tri({0.45f, 0.5f, -2.0f}, {0.9f, 0.5f, -2.0f}, {0.675f, 0.9f, -2.0f},
        Color(0.95f, 0.85f, 0.1f, 1));
    return s;
}

// A full-screen quad, used as the stencil mask and as the thing drawn
// through it.
Scene build_quad() {
    Scene s;
    // Lower-left quadrant, wound counter-clockwise.
    s.vertices = {{{-0.85f, -0.85f, -1.0f}, Color(1, 1, 1, 1)},
                  {{-0.05f, -0.85f, -1.0f}, Color(1, 1, 1, 1)},
                  {{-0.05f, -0.15f, -1.0f}, Color(1, 1, 1, 1)},
                  {{-0.85f, -0.15f, -1.0f}, Color(1, 1, 1, 1)}};
    s.indices = {0, 1, 2, 0, 2, 3};
    return s;
}

// EVERY NDC CORNER, so a scissor can be shown to keep exactly one of
// them. Drawn last, over the top of everything.
Scene build_full_quad() {
    Scene s;
    s.vertices = {{{-1.0f, -1.0f, -1.0f}, Color(1, 1, 1, 1)},
                  {{1.0f, -1.0f, -1.0f}, Color(1, 1, 1, 1)},
                  {{1.0f, 1.0f, -1.0f}, Color(1, 1, 1, 1)},
                  {{-1.0f, 1.0f, -1.0f}, Color(1, 1, 1, 1)}};
    s.indices = {0, 1, 2, 0, 2, 3};
    return s;
}

struct Rendered {
    std::vector<uint8_t> pixels;  // RGBA8, kWidth * kHeight * 4
    bool ok = false;
    std::string device;
};

VertexLayout parity_layout() {
    VertexLayout vl;
    vl.bindings.push_back({0, uint32_t(sizeof(Vertex)), false});
    vl.attributes.push_back({0, 0, Format::RGB32F, 0});
    vl.attributes.push_back({1, 0, Format::RGBA32F, uint32_t(offsetof(Vertex, colour))});
    return vl;
}

// std140 layout, matching ViewData in common.glsl.
struct ViewUniforms {
    Projection view;
    Projection proj;
    Projection view_proj;
    Projection inv_view;
    Projection inv_proj;
    Vec4 eye;
    Vec4 near_far;
    int32_t portal[4];
};

// Must match the Push block in common.glsl: 96 bytes.
struct PushUniforms {
    Projection model;
    Vec4 tint;
    Vec4 params;
};
static_assert(sizeof(PushUniforms) == 96, "push constants must fit the budget");

Rendered render_with(Backend backend, bool validation) {
    Rendered out;
    WindowConfig wc;
    wc.backend = backend;
    wc.title = "warren parity";
    wc.width = int(kWidth);
    wc.height = int(kHeight);
    wc.resizable = false;
    wc.debug = validation;

    Window window;
    if (!window.open(wc)) {
        std::printf("  %-7s window could not be opened; skipping\n",
                    backend_name(backend));
        return out;
    }
    SDL_HideWindow(window.sdl_window());

    DeviceDesc dd;
    dd.backend = backend;
    dd.window = window.sdl_window();
    dd.validation = validation;
    dd.vsync = false;
    Device *dev = create_device(dd);
    if (!dev) {
        std::printf("  %-7s device could not be created; skipping\n",
                    backend_name(backend));
        return out;
    }
    out.device = dev->caps().device_name;

    // --- targets. Offscreen, so this works with no display.
    TextureDesc cd;
    cd.width = kWidth;
    cd.height = kHeight;
    // UNORM, not SRGB: the comparison should be of the values the
    // shaders wrote, not of a colour-space conversion that the two
    // backends might round differently in the last bit.
    cd.format = Format::RGBA8;
    cd.usage = TextureUsage::ColourTarget | TextureUsage::TransferSrc |
               TextureUsage::Sampled;
    cd.name = "parity colour";
    TextureH colour = dev->create_texture(cd);

    TextureDesc dd2;
    dd2.width = kWidth;
    dd2.height = kHeight;
    dd2.format = Format::D32F_S8;
    dd2.usage = TextureUsage::DepthTarget;
    dd2.name = "parity depth";
    TextureH depth = dev->create_texture(dd2);

    // --- the view
    BindGroupLayoutDesc fl;
    fl.entries.push_back({0, BindingType::UniformBuffer, true, true, false, 1});
    fl.name = "frame layout";
    BindGroupLayoutH frame_layout = dev->create_bind_group_layout(fl);

    BindGroupLayoutDesc vl_desc;
    vl_desc.entries.push_back({8, BindingType::UniformBuffer, true, true, false, 1});
    vl_desc.name = "view layout";
    BindGroupLayoutH view_layout = dev->create_bind_group_layout(vl_desc);

    BufferDesc ubd;
    ubd.size = 1024;
    ubd.usage = BufferUsage::Uniform;
    ubd.access = MemoryAccess::CpuToGpu;
    ubd.name = "frame ubo";
    BufferH frame_ubo = dev->create_buffer(ubd);
    ubd.name = "view ubo";
    BufferH view_ubo = dev->create_buffer(ubd);

    BindGroupDesc fg;
    fg.layout = frame_layout;
    fg.entries.push_back({0, frame_ubo, 0, 0, {}, {}, 0, -1});
    BindGroupH frame_group = dev->create_bind_group(fg);

    BindGroupDesc vg;
    vg.layout = view_layout;
    vg.entries.push_back({8, view_ubo, 0, 0, {}, {}, 0, -1});
    BindGroupH view_group = dev->create_bind_group(vg);

    ViewUniforms vu{};
    Transform3D camera = Transform3D::identity();
    vu.view = to_projection(camera.inverse_orthonormal());
    vu.proj = Projection::orthographic(-1, 1, -1, 1, 0.1f, 100.0f);
    vu.view_proj = vu.proj * vu.view;
    vu.inv_view = to_projection(camera);
    vu.inv_proj = vu.proj.inverse();
    vu.eye = Vec4(camera.origin, 1.0f);
    vu.near_far = Vec4(0.1f, 100.0f, 10.0f, 0.0f);
    dev->write_buffer(view_ubo, &vu, sizeof(vu));
    std::vector<uint8_t> frame_zero(1024, 0);
    dev->write_buffer(frame_ubo, frame_zero.data(), frame_zero.size());

    // --- shaders and pipelines
    const shaders::Blob *vsb = shaders::find("parity", ShaderStage::Vertex);
    const shaders::Blob *fsb = shaders::find("parity", ShaderStage::Fragment);
    if (!vsb || !fsb) {
        WR_ERROR("parity shader missing from the build");
        destroy_device(dev);
        return out;
    }
    ShaderH vs = dev->create_shader(shaders::desc(*vsb));
    ShaderH fs = dev->create_shader(shaders::desc(*fsb));

    PipelineDesc pd;
    pd.vertex = vs;
    pd.fragment = fs;
    pd.vertex_layout = parity_layout();
    pd.colour_formats = {Format::RGBA8};
    pd.depth_format = Format::D32F_S8;
    pd.blend = {BlendState::opaque()};
    pd.bind_group_layouts = {frame_layout, view_layout};
    pd.push_constant_size = sizeof(PushUniforms);
    pd.name = "parity opaque";
    PipelineH opaque = dev->create_pipeline(pd);

    // Writes stencil 1 where the quad is, touching neither colour nor
    // depth. This is exactly the first step of drawing a portal.
    PipelineDesc sd = pd;
    sd.blend = {BlendState::no_colour()};
    sd.depth_stencil.depth_write = false;
    sd.depth_stencil.stencil_test = true;
    sd.depth_stencil.front.compare = CompareOp::Always;
    sd.depth_stencil.front.pass = StencilOp::Replace;
    sd.depth_stencil.back = sd.depth_stencil.front;
    sd.name = "parity stencil write";
    PipelineH stencil_write = dev->create_pipeline(sd);

    // Draws only where the stencil says 1, ignoring depth.
    PipelineDesc md = pd;
    md.depth_stencil.depth_test = false;
    md.depth_stencil.depth_write = false;
    md.depth_stencil.stencil_test = true;
    md.depth_stencil.front.compare = CompareOp::Equal;
    md.depth_stencil.front.pass = StencilOp::Keep;
    md.depth_stencil.back = md.depth_stencil.front;
    md.blend = {BlendState::alpha()};
    md.name = "parity stencil test";
    PipelineH stencil_read = dev->create_pipeline(md);

    // No depth, no stencil, no blend: whatever it covers, it owns.
    PipelineDesc fd = pd;
    fd.depth_stencil.depth_test = false;
    fd.depth_stencil.depth_write = false;
    fd.depth_stencil.stencil_test = false;
    fd.blend = {BlendState::opaque()};
    fd.name = "parity scissor probe";
    PipelineH scissor_probe = dev->create_pipeline(fd);

    // --- geometry
    Scene scene = build_scene();
    Scene quad = build_quad();
    Scene full = build_full_quad();
    BufferDesc vbd;
    vbd.size = scene.vertices.size() * sizeof(Vertex);
    vbd.usage = BufferUsage::Vertex;
    vbd.name = "scene vertices";
    BufferH scene_vb = dev->create_buffer(vbd, scene.vertices.data());
    BufferDesc ibd;
    ibd.size = scene.indices.size() * sizeof(uint32_t);
    ibd.usage = BufferUsage::Index;
    ibd.name = "scene indices";
    BufferH scene_ib = dev->create_buffer(ibd, scene.indices.data());

    vbd.size = quad.vertices.size() * sizeof(Vertex);
    vbd.name = "quad vertices";
    BufferH quad_vb = dev->create_buffer(vbd, quad.vertices.data());
    ibd.size = quad.indices.size() * sizeof(uint32_t);
    ibd.name = "quad indices";
    BufferH quad_ib = dev->create_buffer(ibd, quad.indices.data());

    vbd.size = full.vertices.size() * sizeof(Vertex);
    vbd.name = "full quad vertices";
    BufferH full_vb = dev->create_buffer(vbd, full.vertices.data());
    ibd.size = full.indices.size() * sizeof(uint32_t);
    ibd.name = "full quad indices";
    BufferH full_ib = dev->create_buffer(ibd, full.indices.data());

    // --- the frame
    CommandList *cmd = dev->begin_frame();
    if (!cmd) {
        destroy_device(dev);
        return out;
    }

    RenderingInfo ri;
    ColourAttachment ca;
    ca.texture = colour;
    ca.load = LoadOp::Clear;
    ca.clear = Color(0.06f, 0.07f, 0.09f, 1.0f);
    ri.colour.push_back(ca);
    ri.has_depth = true;
    ri.depth.texture = depth;
    // Reverse-Z clears depth to ZERO.
    ri.depth.clear_depth = 0.0f;
    ri.depth.clear_stencil = 0;
    ri.width = kWidth;
    ri.height = kHeight;
    ri.name = "parity";
    cmd->begin_rendering(ri);
    cmd->push_debug_group("parity scene");

    PushUniforms pu{};
    pu.model = Projection::identity();
    pu.tint = Vec4(1, 1, 1, 1);

    cmd->bind_pipeline(opaque);
    cmd->bind_group(0, frame_group);
    cmd->bind_group(1, view_group);
    cmd->push_constants(&pu, sizeof(pu));
    cmd->bind_vertex_buffer(0, scene_vb);
    cmd->bind_index_buffer(scene_ib, IndexType::U32);
    cmd->draw_indexed(uint32_t(scene.indices.size()));

    // The stencil half.
    cmd->bind_pipeline(stencil_write);
    cmd->set_stencil_reference(1);
    cmd->bind_group(0, frame_group);
    cmd->bind_group(1, view_group);
    cmd->push_constants(&pu, sizeof(pu));
    cmd->bind_vertex_buffer(0, quad_vb);
    cmd->bind_index_buffer(quad_ib, IndexType::U32);
    cmd->draw_indexed(uint32_t(quad.indices.size()));

    pu.tint = Vec4(0.2f, 0.9f, 0.8f, 0.5f);
    cmd->bind_pipeline(stencil_read);
    cmd->set_stencil_reference(1);
    cmd->bind_group(0, frame_group);
    cmd->bind_group(1, view_group);
    cmd->push_constants(&pu, sizeof(pu));
    cmd->bind_vertex_buffer(0, quad_vb);
    cmd->bind_index_buffer(quad_ib, IndexType::U32);
    cmd->draw_indexed(uint32_t(quad.indices.size()));

    // --- THE SCISSOR, which is a convention all of its own.
    //
    // A full-screen quad in magenta, scissored to a rectangle that is
    // asymmetric in BOTH axes: x 16..80 and y 32..96 measured from the
    // TOP-LEFT, which is the one convention the RHI states. A backend
    // that measures y from the bottom, or that measures it from the
    // wrong height, puts the magenta somewhere else -- and since the
    // full-screen viewport is symmetric in y, nothing else in this
    // test would ever catch it. The portal renderer scissors every
    // recursion level, so getting this wrong shows up as a portal
    // whose contents are sliced off.
    cmd->push_debug_group("scissor");
    cmd->set_scissor({16, 32, 64, 64});
    pu.tint = Vec4(1, 0, 1, 1);
    cmd->bind_pipeline(scissor_probe);
    cmd->bind_group(0, frame_group);
    cmd->bind_group(1, view_group);
    cmd->push_constants(&pu, sizeof(pu));
    cmd->bind_vertex_buffer(0, full_vb);
    cmd->bind_index_buffer(full_ib, IndexType::U32);
    cmd->draw_indexed(uint32_t(full.indices.size()));
    cmd->set_scissor({0, 0, kWidth, kHeight});
    cmd->pop_debug_group();

    // --- THE VIEWPORT, which has exactly the same trap.
    //
    // Every viewport the renderer sets today covers the whole target,
    // and a full-target viewport is its own reflection -- so a
    // backend that measures viewport y from the wrong end looks
    // perfect right up until something renders to part of a target.
    // Same shape of check: an off-centre rectangle, and the whole
    // NDC square squeezed into it.
    cmd->push_debug_group("viewport");
    Viewport sub;
    sub.x = 160;
    sub.y = 16;
    sub.width = 48;
    sub.height = 32;
    cmd->set_viewport(sub);
    pu.tint = Vec4(0, 0, 1, 1);
    cmd->bind_pipeline(scissor_probe);
    cmd->bind_group(0, frame_group);
    cmd->bind_group(1, view_group);
    cmd->push_constants(&pu, sizeof(pu));
    cmd->bind_vertex_buffer(0, full_vb);
    cmd->bind_index_buffer(full_ib, IndexType::U32);
    cmd->draw_indexed(uint32_t(full.indices.size()));
    Viewport whole;
    whole.width = float(kWidth);
    whole.height = float(kHeight);
    cmd->set_viewport(whole);
    cmd->pop_debug_group();

    cmd->pop_debug_group();
    cmd->end_rendering();
    dev->end_frame();
    dev->wait_idle();

    out.pixels.resize(size_t(kWidth) * kHeight * 4);
    size_t got = dev->read_texture(colour, out.pixels.data(), out.pixels.size());
    out.ok = got == out.pixels.size();

    destroy_device(dev);
    return out;
}

// A PPM, so a failure can be looked at rather than argued about.
void write_ppm(const char *path, const std::vector<uint8_t> &rgba, uint32_t w,
               uint32_t h) {
    FILE *f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t y = 0; y < h; y++) {
        // Already top-down, which is what a PPM wants.
        const uint8_t *row = &rgba[size_t(y) * w * 4];
        for (uint32_t x = 0; x < w; x++) std::fwrite(row + x * 4, 1, 3, f);
    }
    std::fclose(f);
}

struct Diff {
    double mean = 0.0;
    int max = 0;
    size_t differing = 0;
};

Diff compare(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
    Diff d;
    if (a.size() != b.size()) {
        d.max = 255;
        return d;
    }
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); i += 4) {
        int worst = 0;
        for (int c = 0; c < 3; c++) {
            int delta = std::abs(int(a[i + c]) - int(b[i + c]));
            if (delta > worst) worst = delta;
        }
        sum += worst;
        if (worst > d.max) d.max = worst;
        if (worst > 2) d.differing++;
    }
    d.mean = sum / double(a.size() / 4);
    return d;
}

// Sanity checks that hold for EITHER backend on its own, so a run with
// only one available still proves the conventions.
int check_conventions(const Rendered &r, const char *who) {
    int failures = 0;
    // read_texture hands back top-down, so row 0 is the top of the
    // picture and `up` is a small y.
    auto at = [&](uint32_t x, uint32_t y) {
        size_t i = (size_t(y) * kWidth + x) * 4;
        return Color(float(r.pixels[i]) / 255.0f, float(r.pixels[i + 1]) / 255.0f,
                     float(r.pixels[i + 2]) / 255.0f, 1.0f);
    };
    auto fail = [&](const char *what) {
        std::printf("  FAIL  [%s] %s\n", who, what);
        failures++;
    };

    // The yellow marker sits at NDC (0.675, 0.65)-ish: right of centre
    // and high. In a top-down image that is x = 84%, y = 22%.
    Color top_right = at(uint32_t(kWidth * 0.84f), uint32_t(kHeight * 0.22f));
    if (!(top_right.r > 0.5f && top_right.g > 0.4f && top_right.b < 0.3f))
        fail("+Y is not up: no yellow marker in the upper right");

    // The green triangle is wound clockwise and must have been culled.
    bool any_green = false;
    for (uint32_t y = 0; y < kHeight; y++)
        for (uint32_t x = 0; x < kWidth; x++) {
            Color c = at(x, y);
            if (c.g > 0.6f && c.r < 0.3f && c.b < 0.3f) any_green = true;
        }
    if (any_green) fail("back-face culling is not working: the CW triangle drew");

    // The red and blue triangles overlap at NDC (0.05, -0.45), which
    // is x = 52.5%, y = 72.5% top-down -- chosen to sit clear of the
    // teal stencil quad, which covers the lower LEFT and would tint
    // the sample blue and make a working depth test look broken.
    Color overlap = at(uint32_t(kWidth * 0.525f), uint32_t(kHeight * 0.725f));
    if (!(overlap.r > 0.5f && overlap.b < 0.35f))
        fail("reverse-Z depth test is inverted: the far triangle won the overlap");

    // The stencil-masked teal tint must appear somewhere, and only
    // inside the quad.
    bool any_teal = false;
    for (uint32_t y = 0; y < kHeight; y++)
        for (uint32_t x = 0; x < kWidth; x++) {
            Color c = at(x, y);
            if (c.g > 0.35f && c.b > 0.3f && c.r < 0.45f) any_teal = true;
        }
    if (!any_teal) fail("stencil masking drew nothing -- portals cannot work");

    // The magenta probe must be exactly the scissor rectangle: x
    // 16..79, y 32..95, top-left origin.
    uint32_t mx0 = kWidth, my0 = kHeight, mx1 = 0, my1 = 0;
    size_t magenta = 0;
    for (uint32_t y = 0; y < kHeight; y++)
        for (uint32_t x = 0; x < kWidth; x++) {
            Color c = at(x, y);
            if (c.r > 0.85f && c.b > 0.85f && c.g < 0.15f) {
                magenta++;
                mx0 = std::min(mx0, x);
                my0 = std::min(my0, y);
                mx1 = std::max(mx1, x);
                my1 = std::max(my1, y);
            }
        }
    if (magenta != 64 * 64) {
        char b[160];
        std::snprintf(b, sizeof(b),
                      "the scissor kept %zu pixels, not %d", magenta, 64 * 64);
        fail(b);
    }
    if (magenta && (mx0 != 16 || my0 != 32 || mx1 != 79 || my1 != 95)) {
        char b[200];
        std::snprintf(b, sizeof(b),
                      "the scissor landed at x %u..%u y %u..%u, not x 16..79 "
                      "y 32..95 -- y is measured from the TOP",
                      mx0, mx1, my0, my1);
        fail(b);
    }

    // And the blue probe must be exactly the viewport rectangle.
    uint32_t bx0 = kWidth, by0 = kHeight, bx1 = 0, by1 = 0;
    size_t blue = 0;
    for (uint32_t y = 0; y < kHeight; y++)
        for (uint32_t x = 0; x < kWidth; x++) {
            Color c = at(x, y);
            if (c.b > 0.85f && c.r < 0.15f && c.g < 0.15f) {
                blue++;
                bx0 = std::min(bx0, x);
                by0 = std::min(by0, y);
                bx1 = std::max(bx1, x);
                by1 = std::max(by1, y);
            }
        }
    if (blue != 48 * 32) {
        char b[160];
        std::snprintf(b, sizeof(b), "the viewport covered %zu pixels, not %d",
                      blue, 48 * 32);
        fail(b);
    }
    if (blue && (bx0 != 160 || by0 != 16 || bx1 != 207 || by1 != 47)) {
        char b[200];
        std::snprintf(b, sizeof(b),
                      "the viewport landed at x %u..%u y %u..%u, not x 160..207 "
                      "y 16..47 -- y is measured from the TOP",
                      bx0, bx1, by0, by1);
        fail(b);
    }

    return failures;
}

}  // namespace

int main(int argc, char **argv) {
    bool validation = true;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--no-validation")) validation = false;
    log_set_level(LogLevel::Warn);

    std::printf("backend parity\n");
    Rendered gl, vk;
#if WARREN_OPENGL
    gl = render_with(Backend::OpenGL, validation);
#endif
#if WARREN_VULKAN
    vk = render_with(Backend::Vulkan, validation);
#endif

    int failures = 0;
    int ran = 0;
    if (gl.ok) {
        std::printf("  OpenGL  %s\n", gl.device.c_str());
        write_ppm("parity_opengl.ppm", gl.pixels, kWidth, kHeight);
        failures += check_conventions(gl, "OpenGL");
        ran++;
    }
    if (vk.ok) {
        std::printf("  Vulkan  %s\n", vk.device.c_str());
        write_ppm("parity_vulkan.ppm", vk.pixels, kWidth, kHeight);
        failures += check_conventions(vk, "Vulkan");
        ran++;
    }

    if (gl.ok && vk.ok) {
        Diff d = compare(gl.pixels, vk.pixels);
        std::printf("  difference: mean %.3f/255, max %d/255, %zu of %u pixels differ\n",
                    d.mean, d.max, d.differing, kWidth * kHeight);
        // Rasterisation edges legitimately differ by a pixel between
        // drivers, so a handful of edge pixels is fine. A convention
        // that disagrees is not: that shows up as thousands.
        size_t budget = size_t(kWidth) * kHeight / 100;  // one percent
        if (d.differing > budget) {
            std::printf("  FAIL  the backends disagree about more than 1%% of the "
                        "image (%zu > %zu)\n", d.differing, budget);
            failures++;
        }
    } else if (ran == 0) {
        std::printf("  no backend could be started; nothing was checked\n");
        return 77;  // ctest's "skipped"
    } else {
        std::printf("  only one backend available; parity not compared\n");
    }

    std::printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
