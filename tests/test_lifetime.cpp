// Warren -- destroying a GPU resource that the GPU is still reading.
//
// A game frees things in its update: a corpse is removed, a level is
// unloaded, a weapon is swapped. That happens BETWEEN frames, which
// is when a script runs, and it is the case the naive deferred-delete
// gets wrong. Queueing the deletion on `frame_index_` looks right and
// is not: end_frame advances that index, so between frames it names
// the slot to be recorded NEXT, whose fence is frames_in_flight old
// and already signalled. The deletion then runs at the very next
// begin_frame while the frame just submitted is still reading the
// resource.
//
// Nothing crashes immediately. The validation layer reports it, the
// driver carries on, and some seconds later the frame loop comes
// apart -- fences in use, command buffers in use, vkQueueSubmit2
// failing. Which is a long way from "a corpse was freed".
//
// So: churn resources the way a game does -- a mesh added to the
// scene, RENDERED, and freed from outside the frame -- with the
// validation layer on, and fail on a single error from it. Rendering
// is not optional in that sentence: a resource the GPU never
// referenced can be destroyed whenever you like, and a first attempt
// at this test created and destroyed buffers without drawing them
// and passed happily against the broken code.
#include <SDL2/SDL.h>

#include <cstdio>
#include <cstring>
#include <string>
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

using namespace wr;
using namespace wr::rhi;

namespace {

int g_errors = 0;
std::string g_first;

void sink(LogLevel level, const char *text) {
    if (level < LogLevel::Error || !text) return;
    g_errors++;
    if (g_first.empty()) g_first = text;
}

int g_fail = 0;
void check(bool ok, const char *what) {
    std::printf("    %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

// Enough frames that every in-flight slot is reused several times,
// and a target big enough that a frame is still running when the next
// one starts. THE SECOND PART IS NOT DECORATION: the validation layer
// asks the driver whether a fence has signalled, so a frame that
// finishes in microseconds is a frame that never catches a
// use-after-free. A first version of this test rendered two boxes
// into a 64-pixel target and passed against the broken code.
constexpr int kFrames = 30;
constexpr uint32_t kSize = 1024;
constexpr int kBodies = 40;

bool run(Backend backend, std::string *device, bool *deferring,
         bool *survived) {
    WindowConfig wc;
    wc.backend = backend;
    wc.width = wc.height = int(kSize);
    wc.resizable = false;
    wc.debug = true;              // the whole point
    wc.title = "lifetime";
    Window window;
    if (!window.open(wc)) return false;
    SDL_HideWindow(window.sdl_window());

    DeviceDesc dd;
    dd.backend = backend;
    dd.window = window.sdl_window();
    dd.validation = true;
    dd.vsync = false;
    Device *dev = create_device(dd);
    if (!dev) return false;
    *device = dev->caps().device_name;

    TextureDesc cd;
    cd.width = cd.height = kSize;
    cd.format = Format::RGBA8;
    cd.usage = TextureUsage::ColourTarget | TextureUsage::TransferSrc |
               TextureUsage::Sampled;
    cd.name = "target";
    TextureH target = dev->create_texture(cd);

    RenderSettings rs;
    rs.msaa = 1;
    rs.draw_sky = false;
    rs.shadows = false;
    rs.image_based_lighting = false;
    Renderer renderer;
    if (!renderer.init(dev, rs)) {
        destroy_device(dev);
        return false;
    }

    auto tree = std::make_unique<SceneTree>();
    Node3D *scene = new Node3D();
    scene->set_name("Scene");
    tree->set_scene(scene);

    Camera3D *cam = new Camera3D();
    cam->set_name("Camera");
    cam->set_fov_degrees(60.0f);
    cam->set_position({0, 6, 14});
    cam->look_at({0, 0, 0});
    scene->add_child(cam);
    cam->make_current();

    // A body arrives, is drawn, and is freed a couple of frames later
    // from OUTSIDE the frame -- exactly what a round of this game
    // does thirty times a minute.
    std::vector<Node3D *> live;
    for (int i = 0; i < kFrames; i++) {
        // A crowd of them, each with a mesh and a material of its
        // own so there is something real to destroy -- shared ones
        // would never be freed -- and so that the frame costs enough
        // to still be running when the next begins.
        Node3D *wave = new Node3D();
        wave->set_name("wave");
        for (int k = 0; k < kBodies; k++) {
            MeshInstance3D *mi = new MeshInstance3D();
            mi->set_name("body");
            mi->mesh = Mesh::sphere(0.5f, 24, 16);
            Ref<Material> mat =
                Material::make(Color(0.8f, 0.3f, 0.2f, 1), 0.7f);
            mi->set_material(0, mat.get());
            mi->set_position({float(k % 12) - 6.0f, 0.0f,
                              float(k / 12) * -1.5f});
            wave->add_child(mi);
        }
        scene->add_child(wave);
        live.push_back(wave);

        tree->process(1.0f / 60.0f);
        if (CommandList *cmd = dev->begin_frame()) {
            renderer.render(cmd, tree.get(), cam, target);
            dev->end_frame();
        }

        // BETWEEN FRAMES. Not inside one. queue_free is what a game
        // calls, and the tree reaps it on the next process.
        if (live.size() > 2) {
            live.front()->queue_free();
            live.erase(live.begin());
        }
    }

    // --- AND THE RULE ITSELF, checked exactly rather than by hoping
    // the GPU is slow.
    //
    // The flood of validation errors above is the symptom, and it
    // only appears when a frame is genuinely still running -- on a
    // fast machine with a cheap scene it does not, which is how this
    // shipped broken. The underlying rule does not depend on speed:
    // a resource destroyed BETWEEN frames must not be freed at the
    // next begin_frame, because the frame just submitted may still be
    // reading it. It must survive until the fence of that frame is
    // waited, which is one further begin_frame away.
    dev->wait_idle();
    if (dev->pending_deletions() == 0) {
        BufferDesc bd;
        bd.size = 1024;
        bd.usage = BufferUsage::Vertex | BufferUsage::TransferDst;
        bd.name = "rule";
        BufferH b = dev->create_buffer(bd);

        if (CommandList *cmd = dev->begin_frame()) {
            renderer.render(cmd, tree.get(), cam, target);
            dev->end_frame();
        }
        // Here: outside a frame, one frame submitted and possibly
        // still running.
        dev->destroy(b);
        const size_t queued = dev->pending_deletions();

        if (CommandList *cmd = dev->begin_frame()) {
            (void)cmd;
            dev->end_frame();
        }
        const size_t after_one = dev->pending_deletions();
        if (CommandList *cmd = dev->begin_frame()) {
            (void)cmd;
            dev->end_frame();
        }
        dev->wait_idle();
        const size_t after_two = dev->pending_deletions();
        *survived = queued > 0 && after_one >= queued && after_two == 0;
        *deferring = queued > 0;
    } else {
        *deferring = false;
        *survived = true;
    }

    dev->wait_idle();
    tree.reset();
    renderer.shutdown();
    destroy_device(dev);
    return true;
}

}  // namespace

int main() {
    log_set_level(LogLevel::Warn);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("lifetime\n  no video device: %s\n", SDL_GetError());
        return 77;
    }
    std::printf("lifetime\n");
    log_add_sink(&sink);

    bool any = false;
    for (int i = 0; i < 2; i++) {
        const Backend backend = i == 0 ? Backend::Vulkan : Backend::OpenGL;
        const char *name = i == 0 ? "Vulkan" : "OpenGL";
        g_errors = 0;
        g_first.clear();
        std::string device;
        bool deferring = false, survived = false;
        if (!run(backend, &device, &deferring, &survived)) {
            std::printf("  %s: not available\n", name);
            continue;
        }
        any = true;
        std::printf("  %-7s %s\n", name, device.c_str());
        char what[400];
        std::snprintf(what, sizeof(what),
                      "[%s] %d frames of %d bodies drawn then freed, with "
                      "validation on, and not one error (%d; first: %s)",
                      name, kFrames, kBodies, g_errors,
                      g_first.empty() ? "-" : g_first.substr(0, 150).c_str());
        check(g_errors == 0, what);
        if (deferring) {
            std::snprintf(what, sizeof(what),
                          "[%s] a resource destroyed between frames outlives "
                          "the next begin_frame", name);
            check(survived, what);
        } else {
            std::printf("    ---- [%s] destroys immediately; nothing to "
                        "defer\n", name);
        }
    }
    log_remove_sink(&sink);

    if (!any) {
        std::printf("  neither backend would start; skipping\n");
        SDL_Quit();
        return 77;
    }
    SDL_Quit();
    std::printf("  %s\n", g_fail ? "FAILED" : "all good");
    return g_fail ? 1 : 0;
}
