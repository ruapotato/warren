// Manifold -- do the cascades actually shadow anything, and do both
// backends agree about where?
//
// A shadow map is a render target sampled with a comparison sampler,
// and there are four independent conventions in that sentence, each of
// which can be inverted on its own:
//
//   * reverse-Z, so the map clears to 0 and the compare is GREATER;
//   * the depth slice, so the cascade's orthographic box has to cover
//     the geometry rather than sit behind it;
//   * the v flip, because a render target's row 0 is its TOP while
//     ndc.y = +1 is the top of the picture;
//   * the array layer, so cascade 2 samples cascade 2.
//
// Get any one of them backwards and the picture is still plausible --
// evenly lit, or evenly dark, or shadowed in the wrong half -- which
// is why this renders a known scene and reads back known pixels
// instead of looking at it. A floor with a box standing on it, a sun
// at a stated angle, and three questions: is the floor lit where
// nothing blocks it, is it dark where the box blocks it, and do the
// two backends give the same answer.
#include <SDL2/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "core/log.h"
#include "platform/window.h"
#include "render/material.h"
#include "render/mesh.h"
#include "render/renderer.h"
#include "rhi/rhi.h"
#include "scene/nodes.h"
#include "scene/scene_tree.h"

using namespace mf;
using namespace mf::rhi;

namespace {

constexpr uint32_t kSize = 256;

struct Shot {
    std::vector<uint8_t> pixels;  // RGBA8, kSize * kSize * 4
    bool ok = false;
    std::string device;
    uint32_t cascades = 0;
    // Where the caster meets the floor, in pixels. Projected by the
    // engine rather than worked out by hand, so the test says nothing
    // about the camera that the camera does not say itself.
    float caster_x = 0, caster_y = 0;
};

// Straight down the -Z axis at a floor, from slightly above. The sun
// comes from the +X side so the box's shadow falls to -X, which is
// LEFT in the picture -- a direction the test can name.
constexpr Vec3 kSun{-0.55f, -0.78f, -0.30f};

// The caster's centre, and half its height -- so the test can ask the
// camera where its base lands rather than guessing at pixels.
constexpr float kCasterHeight = 2.0f;
constexpr Vec3 kCaster{2.5f, kCasterHeight, 0.0f};

MeshInstance3D *add_box(Node *parent, const char *name, const Vec3 &size,
                        const Vec3 &at, Ref<Material> mat) {
    MeshInstance3D *mi = new MeshInstance3D();
    mi->set_name(name);
    mi->mesh = Mesh::box(size);
    mi->set_material(0, mat.get());
    mi->set_position(at);
    parent->add_child(mi);
    return mi;
}

Shot run(Backend backend, bool shadows, bool validation) {
    Shot out;
    WindowConfig wc;
    wc.backend = backend;
    wc.width = int(kSize);
    wc.height = int(kSize);
    wc.resizable = false;
    wc.debug = validation;
    wc.title = "shadows";
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
    cd.name = "target";
    TextureH target = dev->create_texture(cd);

    RenderSettings rs;
    rs.msaa = 1;               // one sample, so a pixel is one answer
    rs.draw_sky = false;       // a flat clear, so nothing else varies
    rs.shadows = shadows;
    rs.shadow_map_size = 1024;
    rs.shadow_cascades = 4;
    rs.shadow_distance = 40.0f;
    rs.clear_colour = Color(0, 0, 0, 1);

    Renderer renderer;
    if (!renderer.init(dev, rs)) {
        destroy_device(dev);
        return out;
    }
    renderer.sun_direction = kSun;
    renderer.sun_energy = 3.0f;
    // Low ambient: the difference between lit and shadowed has to be
    // large enough to read off a byte.
    renderer.ambient_energy = 0.10f;
    renderer.fog_density = 0.0f;

    // THE TREE IS HELD BY POINTER so it can be destroyed BEFORE the
    // device. A mesh releases its buffers in its destructor, through
    // the device it was uploaded with, so a scene that outlives the
    // device frees through a dangling one. Engine::shutdown has the
    // same ordering for the same reason.
    auto tree = std::make_unique<SceneTree>();
    Node3D *scene = new Node3D();
    scene->set_name("Scene");
    tree->set_scene(scene);

    Ref<Material> white = Material::make(Color(0.9f, 0.9f, 0.9f, 1), 0.9f);
    add_box(scene, "floor", {40, 1, 40}, {0, -0.5f, 0}, white);
    // One caster, well clear of the edges of the picture.
    add_box(scene, "caster", {2, kCasterHeight * 2, 2}, kCaster, white);

    Camera3D *cam = new Camera3D();
    cam->set_name("Camera");
    cam->set_fov_degrees(60.0f);
    cam->set_near(0.1f);
    cam->set_far(200.0f);
    cam->set_position({0, 9, 14});
    cam->look_at({0, 0, 0});
    scene->add_child(cam);
    cam->make_current();

    tree->process(1.0f / 60.0f);

    const Vec3 base = cam->project_point(kCaster - Vec3(0, kCasterHeight, 0), 1.0f);
    out.caster_x = base.x * float(kSize);
    out.caster_y = base.y * float(kSize);

    CommandList *cmd = dev->begin_frame();
    if (!cmd) {
        tree.reset();
        white.reset();
        renderer.shutdown();
        destroy_device(dev);
        return out;
    }
    renderer.render(cmd, tree.get(), cam, target);
    dev->end_frame();
    dev->wait_idle();

    out.cascades = renderer.stats().cascades;
    out.pixels.resize(size_t(kSize) * kSize * 4);
    out.ok = dev->read_texture(target, out.pixels.data(), out.pixels.size()) ==
             out.pixels.size();

    tree.reset();
    white.reset();
    renderer.shutdown();
    destroy_device(dev);
    return out;
}

int g_fail = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        g_fail++;
    }
}

float luma(const Shot &s, uint32_t x, uint32_t y) {
    size_t i = (size_t(y) * kSize + x) * 4;
    return (0.2126f * float(s.pixels[i]) + 0.7152f * float(s.pixels[i + 1]) +
            0.0722f * float(s.pixels[i + 2])) /
           255.0f;
}

// A failure in a rendering test is much easier to act on when it can
// be looked at. MF_SHADOW_DUMP=prefix writes the frames it compared.
void write_ppm(const char *path, const Shot &s) {
    FILE *f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%u %u\n255\n", kSize, kSize);
    for (size_t i = 0; i < size_t(kSize) * kSize; i++)
        std::fwrite(&s.pixels[i * 4], 1, 3, f);
    std::fclose(f);
}

void maybe_dump(const char *name, const Shot &s) {
    const char *prefix = getenv("MF_SHADOW_DUMP");
    if (!prefix || !s.ok) return;
    char path[256];
    std::snprintf(path, sizeof(path), "%s%s.ppm", prefix, name);
    write_ppm(path, s);
    std::printf("  wrote %s\n", path);
}

}  // namespace

int main(int argc, char **argv) {
    bool validation = false;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--validation")) validation = true;
    log_set_level(LogLevel::Warn);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("shadows\n  no video device: %s\n", SDL_GetError());
        return 77;
    }
    std::printf("shadows\n");

    Shot lit_vk = run(Backend::Vulkan, false, validation);
    Shot shadowed_vk = run(Backend::Vulkan, true, validation);
    Shot lit_gl = run(Backend::OpenGL, false, validation);
    Shot shadowed_gl = run(Backend::OpenGL, true, validation);

    maybe_dump("lit_vk", lit_vk);
    maybe_dump("shadowed_vk", shadowed_vk);
    maybe_dump("lit_gl", lit_gl);
    maybe_dump("shadowed_gl", shadowed_gl);

    if (!shadowed_vk.ok && !shadowed_gl.ok) {
        std::printf("  neither backend would start; skipping\n");
        SDL_Quit();
        return 77;
    }

    struct Case {
        const char *name;
        const Shot &lit;
        const Shot &shadowed;
    } cases[] = {{"Vulkan", lit_vk, shadowed_vk}, {"OpenGL", lit_gl, shadowed_gl}};

    for (const Case &c : cases) {
        if (!c.shadowed.ok) {
            std::printf("  %s: not available\n", c.name);
            continue;
        }
        std::printf("  %-7s %s\n", c.name, c.shadowed.device.c_str());
        char what[160];

        std::snprintf(what, sizeof(what), "[%s] four cascades were fitted",
                      c.name);
        check(c.shadowed.cascades == 4, what);

        // The shadow pass must DARKEN something and BRIGHTEN nothing.
        size_t darker = 0, brighter = 0;
        for (uint32_t y = 0; y < kSize; y++)
            for (uint32_t x = 0; x < kSize; x++) {
                const float a = luma(c.lit, x, y), b = luma(c.shadowed, x, y);
                if (b < a - 0.02f) darker++;
                if (b > a + 0.02f) brighter++;
            }
        std::snprintf(what, sizeof(what),
                      "[%s] the shadow pass darkened %zu pixels", c.name, darker);
        check(darker > kSize * kSize / 100, what);
        std::snprintf(what, sizeof(what),
                      "[%s] and brightened none (it brightened %zu)", c.name,
                      brighter);
        check(brighter == 0, what);

        // WHERE the shadow fell, not just that one did. The sun
        // comes from +X, so the shadow goes towards -X, which is the
        // left of the picture; and it starts at the caster's feet.
        // Invert the v flip in the lookup, or the light's basis, and
        // the darkening still happens -- somewhere else.
        double sx = 0, sy = 0, nearest = 1e9;
        size_t n = 0;
        for (uint32_t y = 0; y < kSize; y++)
            for (uint32_t x = 0; x < kSize; x++) {
                if (luma(c.shadowed, x, y) < luma(c.lit, x, y) - 0.05f) {
                    sx += x;
                    sy += y;
                    n++;
                    const double dx = double(x) - double(c.shadowed.caster_x);
                    const double dy = double(y) - double(c.shadowed.caster_y);
                    nearest = std::min(nearest, std::sqrt(dx * dx + dy * dy));
                }
            }
        if (n) {
            sx /= double(n);
            sy /= double(n);
        }
        std::printf("       shadow centroid (%.1f, %.1f), caster base (%.1f, %.1f)\n",
                    sx, sy, double(c.shadowed.caster_x),
                    double(c.shadowed.caster_y));

        std::snprintf(what, sizeof(what),
                      "[%s] the shadow is left of the caster, away from the "
                      "sun (%.1f < %.1f)",
                      c.name, sx, double(c.shadowed.caster_x));
        check(n > 0 && sx < double(c.shadowed.caster_x) - 4.0, what);

        // A SHADOW IS ATTACHED TO ITS CASTER. This is the check that
        // has teeth: a lookup with the v flip missing, or the wrong
        // cascade, or a light basis built from the wrong axis, still
        // darkens about the right number of pixels and still puts
        // them vaguely downsun -- but somewhere else on the floor.
        // Only the correct one touches the box's feet.
        std::snprintf(what, sizeof(what),
                      "[%s] the shadow touches the caster's feet (nearest "
                      "darkened pixel is %.1f away, allowed 16)",
                      c.name, nearest);
        check(n > 0 && nearest < 16.0, what);

        std::snprintf(what, sizeof(what),
                      "[%s] and does not run off across the floor (centroid "
                      "%.1f from the feet, allowed 60)",
                      c.name,
                      std::sqrt((sx - c.shadowed.caster_x) *
                                    (sx - c.shadowed.caster_x) +
                                (sy - c.shadowed.caster_y) *
                                    (sy - c.shadowed.caster_y)));
        check(n > 0 && (sx - c.shadowed.caster_x) * (sx - c.shadowed.caster_x) +
                               (sy - c.shadowed.caster_y) *
                                   (sy - c.shadowed.caster_y) <
                           60.0 * 60.0,
              what);

        // The darkest shadowed pixel must be much darker than the lit
        // floor beside it, or the map is only grazing the geometry.
        float darkest = 1.0f, floor_lit = 0.0f;
        for (uint32_t y = 0; y < kSize; y++)
            for (uint32_t x = 0; x < kSize; x++) {
                const float a = luma(c.lit, x, y), b = luma(c.shadowed, x, y);
                if (a < 0.01f) continue;  // background
                floor_lit = std::max(floor_lit, a);
                if (b < a - 0.05f) darkest = std::min(darkest, b);
            }
        std::snprintf(what, sizeof(what),
                      "[%s] the shadow is deep (%.3f against a lit %.3f)",
                      c.name, double(darkest), double(floor_lit));
        check(floor_lit - darkest > 0.25f, what);
    }

    // AND THE TWO BACKENDS AGREE. A shadow map is the part of the
    // frame most likely to drift: a different depth format, a
    // different compare, a scissor measured from the other end.
    if (shadowed_vk.ok && shadowed_gl.ok) {
        size_t differ = 0;
        double total = 0.0;
        for (size_t i = 0; i < shadowed_vk.pixels.size(); i++) {
            const int d = std::abs(int(shadowed_vk.pixels[i]) -
                                   int(shadowed_gl.pixels[i]));
            total += d;
            if (d > 8) differ++;
        }
        const double mean = total / double(shadowed_vk.pixels.size());
        std::printf("  difference: mean %.3f/255, %zu of %u samples differ\n",
                    mean, differ, kSize * kSize * 4);
        // Not bit-exact: the two rasterise the shadow map's edges
        // independently and a comparison sampler turns a half-texel
        // into a whole shadowed pixel. A percent of the samples is
        // the silhouette; more than that is a convention.
        check(differ < size_t(kSize) * kSize * 4 / 100,
              "the backends agree about where the shadows are");
    }

    SDL_Quit();
    std::printf("%s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
