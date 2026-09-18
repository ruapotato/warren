// Warren -- does the exposure actually expose, and does the frame
// survive it?
//
// The tonemap is one full-screen triangle at the end of everything,
// and for a while any exposure above 1.0 rendered a completely empty
// frame. The cause was not in the tonemap's arithmetic: the pass had
// been built with the SHARED full-screen vertex shader, which writes
// `push.params.x` into `gl_Position.z` because the portal renderer
// needs a triangle at a chosen depth to clear depth inside a stencil.
// The tonemap's fragment stage reads that same push slot as the
// exposure. So the exposure was also the clip-space Z, and above 1.0
// the triangle fell out of the clip volume and was discarded whole --
// which is why the threshold sat exactly on 1.0.
//
// Two passes sharing a vertex shader while disagreeing about what a
// push constant means is the defect, and it is invisible from inside
// either shader. So this test does the only thing that catches it:
// renders the same scene at a spread of exposures and reads the
// pixels back.
//
//   * every exposure produces a frame at all;
//   * brighter exposure is brighter picture, in order;
//   * and it is the same story on both backends.
#include <SDL2/SDL.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "platform/window.h"
#include "render/material.h"
#include "render/mesh.h"
#include "render/renderer.h"
#include "rhi/rhi.h"
#include "scene/nodes.h"
#include "scene/scene_tree.h"

using namespace wr;
using namespace wr::rhi;

namespace {

constexpr uint32_t kSize = 128;

// A spread either side of 1.0, close enough on both sides of it to
// catch a threshold sitting exactly there.
const float kExposures[] = {0.5f, 0.99f, 1.0f, 1.001f, 1.5f, 3.0f};
constexpr int kCount = int(sizeof(kExposures) / sizeof(kExposures[0]));

struct Shot {
    bool ok = false;
    double mean = 0.0;     // average luma, 0..255
    double covered = 0.0;  // fraction of pixels that are not pure black
};

struct Run {
    bool available = false;
    std::string device;
    Shot shots[kCount];
};

Run run(Backend backend, bool validation) {
    Run out;
    WindowConfig wc;
    wc.backend = backend;
    wc.width = int(kSize);
    wc.height = int(kSize);
    wc.resizable = false;
    wc.debug = validation;
    wc.title = "tonemap";
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
    out.available = true;

    TextureDesc cd;
    cd.width = cd.height = kSize;
    cd.format = Format::RGBA8;
    cd.usage = TextureUsage::ColourTarget | TextureUsage::TransferSrc |
               TextureUsage::Sampled;
    cd.name = "target";
    TextureH target = dev->create_texture(cd);

    // THE SCENE IS DELIBERATELY DIM. A filmic curve shoulders off, so
    // a scene already at the top of the range would read the same at
    // every exposure and the test would pass on a broken engine.
    auto tree = std::make_unique<SceneTree>();
    Node3D *scene = new Node3D();
    scene->set_name("Scene");
    tree->set_scene(scene);

    Ref<Material> grey = Material::make(Color(0.5f, 0.5f, 0.5f, 1), 0.9f);
    MeshInstance3D *floor = new MeshInstance3D();
    floor->set_name("floor");
    floor->mesh = Mesh::box({40, 1, 40});
    floor->set_material(0, grey.get());
    floor->set_position({0, -0.5f, 0});
    scene->add_child(floor);

    Camera3D *cam = new Camera3D();
    cam->set_name("Camera");
    cam->set_fov_degrees(60.0f);
    cam->set_near(0.1f);
    cam->set_far(200.0f);
    cam->set_position({0, 6, 10});
    cam->look_at({0, 0, 0});
    scene->add_child(cam);
    cam->make_current();
    tree->process(1.0f / 60.0f);

    std::vector<uint8_t> pixels(size_t(kSize) * kSize * 4);
    for (int i = 0; i < kCount; i++) {
        RenderSettings rs;
        rs.msaa = 1;
        rs.draw_sky = false;
        rs.shadows = false;
        rs.image_based_lighting = false;
        rs.clear_colour = Color(0, 0, 0, 1);
        rs.exposure = kExposures[i];

        Renderer renderer;
        if (!renderer.init(dev, rs)) break;
        renderer.sun_direction = {-0.55f, -0.78f, -0.30f};
        renderer.sun_energy = 0.7f;
        renderer.ambient_energy = 0.05f;
        renderer.fog_density = 0.0f;

        CommandList *cmd = dev->begin_frame();
        if (!cmd) {
            renderer.shutdown();
            break;
        }
        renderer.render(cmd, tree.get(), cam, target);
        dev->end_frame();
        dev->wait_idle();

        Shot &s = out.shots[i];
        if (dev->read_texture(target, pixels.data(), pixels.size()) ==
            pixels.size()) {
            double sum = 0.0;
            size_t lit = 0;
            for (size_t p = 0; p < pixels.size(); p += 4) {
                const double l = 0.2126 * pixels[p] + 0.7152 * pixels[p + 1] +
                                 0.0722 * pixels[p + 2];
                sum += l;
                if (pixels[p] || pixels[p + 1] || pixels[p + 2]) lit++;
            }
            s.mean = sum / double(pixels.size() / 4);
            s.covered = double(lit) / double(pixels.size() / 4);
            s.ok = true;
        }
        renderer.shutdown();
    }

    tree.reset();
    grey.reset();
    destroy_device(dev);
    return out;
}

int g_fail = 0;

void check(bool ok, const char *what) {
    std::printf("    %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

}  // namespace

int main(int argc, char **argv) {
    bool validation = false;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--validation")) validation = true;
    log_set_level(LogLevel::Warn);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("tonemap\n  no video device: %s\n", SDL_GetError());
        return 77;
    }
    std::printf("tonemap\n");

    Run runs[2] = {run(Backend::Vulkan, validation),
                   run(Backend::OpenGL, validation)};
    const char *names[2] = {"Vulkan", "OpenGL"};

    if (!runs[0].available && !runs[1].available) {
        std::printf("  neither backend would start; skipping\n");
        SDL_Quit();
        return 77;
    }

    for (int r = 0; r < 2; r++) {
        if (!runs[r].available) {
            std::printf("  %s: not available\n", names[r]);
            continue;
        }
        std::printf("  %-7s %s\n", names[r], runs[r].device.c_str());
        char what[200];

        for (int i = 0; i < kCount; i++) {
            const Shot &s = runs[r].shots[i];
            std::snprintf(what, sizeof(what),
                          "[%s] exposure %.3f drew a frame "
                          "(%.0f%% of pixels, mean luma %.1f)",
                          names[r], kExposures[i], s.covered * 100.0, s.mean);
            // The frame must exist and must not be black. An empty
            // frame is the exact failure this test was written for.
            // Half the pixels is the bar rather than all of them:
            // the sky is not drawn, so the top fifth of the picture
            // is the clear colour and is meant to be.
            check(s.ok && s.covered > 0.5 && s.mean > 1.0, what);
        }

        // AND IT HAS TO BE MONOTONIC. A pass that survives every
        // exposure but ignores the value is just as wrong, and would
        // sail through the check above.
        for (int i = 1; i < kCount; i++) {
            const Shot &a = runs[r].shots[i - 1], &b = runs[r].shots[i];
            if (!a.ok || !b.ok) continue;
            std::snprintf(what, sizeof(what),
                          "[%s] %.3f is not darker than %.3f (%.1f vs %.1f)",
                          names[r], kExposures[i], kExposures[i - 1], b.mean,
                          a.mean);
            check(b.mean >= a.mean - 0.5, what);
        }

        // The ends have to be far apart, or "monotonic" is satisfied
        // by a constant.
        if (runs[r].shots[0].ok && runs[r].shots[kCount - 1].ok) {
            std::snprintf(what, sizeof(what),
                          "[%s] exposure changes the picture (%.1f -> %.1f)",
                          names[r], runs[r].shots[0].mean,
                          runs[r].shots[kCount - 1].mean);
            check(runs[r].shots[kCount - 1].mean >
                      runs[r].shots[0].mean * 1.25, what);
        }
    }

    SDL_Quit();
    std::printf("  %s\n", g_fail ? "FAILED" : "all good");
    return g_fail ? 1 : 0;
}
