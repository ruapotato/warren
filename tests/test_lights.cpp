// Manifold -- clustered punctual lights, and the froxel grid a portal
// view needs of its own.
//
// Three questions, in increasing order of how particular they are to
// this engine:
//
//   1. Does an omni light actually light things, and fall off?
//   2. Does a spot light stay inside its cone?
//   3. DOES A LIGHT IN THE FAR ROOM LIGHT WHAT IS SEEN THROUGH THE
//      PORTAL?
//
// The third is the one worth having. A portal view is a different
// camera looking at different geometry through the same pixels, so it
// needs its own froxel grid; cluster only the main camera's view and
// the far room goes dark through the hole while everything around the
// hole stays correct -- which looks like a portal bug and is a
// lighting one. The test renders the same frame with the grid budget
// set to every view and to one, and requires the picture inside the
// aperture to differ.
#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
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
#include "scene/portal.h"
#include "scene/scene_tree.h"

using namespace mf;
using namespace mf::rhi;

namespace {

constexpr uint32_t kSize = 256;

struct Shot {
    std::vector<uint8_t> pixels;
    bool ok = false;
    std::string device;
    uint32_t lights = 0;
    uint32_t clustered_views = 0;
    uint32_t assignments = 0;
    // The portal's footprint on screen, so the test can read inside it
    // without knowing where the camera ended up.
    float portal_x0 = 0, portal_y0 = 0, portal_x1 = 0, portal_y1 = 0;
};

enum Scene { SCENE_OMNI, SCENE_SPOT, SCENE_PORTAL };

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

Shot run(Backend backend, Scene which, int clustered_views, bool validation) {
    Shot out;
    WindowConfig wc;
    wc.backend = backend;
    wc.width = int(kSize);
    wc.height = int(kSize);
    wc.resizable = false;
    wc.debug = validation;
    wc.title = "lights";
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
    rs.msaa = 1;
    rs.draw_sky = false;
    // NO SUN AND NO AMBIENT. Whatever is not black in the result was
    // put there by a punctual light, which is the only way to measure
    // one without subtracting two pictures.
    rs.shadows = false;
    rs.clear_colour = Color(0, 0, 0, 1);
    rs.max_clustered_views = clustered_views;
    rs.max_portal_depth = 1;

    Renderer renderer;
    if (!renderer.init(dev, rs)) {
        destroy_device(dev);
        return out;
    }
    renderer.sun_energy = 0.0f;
    renderer.ambient_energy = 0.0f;
    renderer.fog_density = 0.0f;

    auto tree = std::make_unique<SceneTree>();
    Node3D *scene = new Node3D();
    scene->set_name("Scene");
    tree->set_scene(scene);

    Ref<Material> white = Material::make(Color(0.9f, 0.9f, 0.9f, 1), 0.9f);
    Camera3D *cam = new Camera3D();
    cam->set_name("Camera");
    cam->set_fov_degrees(60.0f);
    cam->set_near(0.1f);
    cam->set_far(300.0f);
    scene->add_child(cam);
    cam->make_current();

    Portal3D *near_portal = nullptr;

    if (which == SCENE_OMNI || which == SCENE_SPOT) {
        // A wall to catch the light, seen straight on.
        add_box(scene, "wall", {20, 20, 0.5f}, {0, 0, -6}, white);
        cam->set_position({0, 0, 2});
        cam->look_at({0, 0, -6});

        if (which == SCENE_OMNI) {
            OmniLight3D *o = new OmniLight3D();
            o->set_name("omni");
            o->set_position({0, 0, -3.5f});
            o->colour = Color(1, 1, 1, 1);
            o->energy = 30.0f;
            o->range = 9.0f;
            scene->add_child(o);
        } else {
            SpotLight3D *sp = new SpotLight3D();
            sp->set_name("spot");
            sp->set_position({0, 0, -1.0f});
            sp->look_at({0, 0, -6});
            sp->colour = Color(1, 1, 1, 1);
            sp->energy = 60.0f;
            sp->range = 20.0f;
            sp->angle = deg2rad(14.0f);
            sp->angle_softness = 0.15f;
            scene->add_child(sp);
        }
    } else {
        // TWO ROOMS. The near one has no light at all; the far one has
        // a lamp and a wall for it to fall on. The only way the far
        // room is ever anything but black is through the portal.
        add_box(scene, "near_floor", {14, 0.5f, 14}, {0, -1.5f, 0}, white);
        add_box(scene, "far_wall", {20, 12, 0.5f}, {80, 2, -7}, white);
        add_box(scene, "far_floor", {20, 0.5f, 16}, {80, -1.5f, 0}, white);

        OmniLight3D *o = new OmniLight3D();
        o->set_name("far_lamp");
        o->set_position({80, 1.5f, -3.5f});
        o->colour = Color(1, 1, 1, 1);
        o->energy = 120.0f;
        o->range = 22.0f;
        scene->add_child(o);

        near_portal = new Portal3D();
        near_portal->set_name("A");
        near_portal->width = 3.0f;
        near_portal->height = 3.0f;
        near_portal->set_position({0, 0.2f, -5.0f});
        scene->add_child(near_portal);

        Portal3D *b = new Portal3D();
        b->set_name("B");
        b->width = 3.0f;
        b->height = 3.0f;
        b->set_position({80, 0.2f, 1.5f});
        b->set_rotation(Quat::from_axis_angle(Vec3::up(), PI));
        scene->add_child(b);
        near_portal->link_to(b);

        cam->set_position({0, 0.6f, 0.5f});
        cam->look_at({0, 0.4f, -5.0f});
    }

    tree->process(1.0f / 60.0f);

    if (near_portal) {
        Transform3D c = cam->global_transform();
        c.basis = c.basis.orthonormalized();
        Rect2 r;
        if (near_portal->screen_rect(c.inverse_orthonormal(),
                                     cam->projection(1.0f), &r)) {
            out.portal_x0 = r.position.x * float(kSize);
            out.portal_y0 = r.position.y * float(kSize);
            out.portal_x1 = (r.position.x + r.size.x) * float(kSize);
            out.portal_y1 = (r.position.y + r.size.y) * float(kSize);
        }
    }

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

    out.lights = renderer.stats().lights;
    out.clustered_views = renderer.stats().clustered_views;
    out.assignments = renderer.stats().light_assignments;
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

float mean_in(const Shot &s, int x0, int y0, int x1, int y1) {
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(int(kSize), x1);
    y1 = std::min(int(kSize), y1);
    if (x1 <= x0 || y1 <= y0) return 0.0f;
    double t = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) t += luma(s, uint32_t(x), uint32_t(y));
    return float(t / (double((x1 - x0)) * (y1 - y0)));
}

void compare(const Shot &a, const Shot &b, const char *what) {
    if (!a.ok || !b.ok) return;
    size_t differ = 0;
    double total = 0;
    for (size_t i = 0; i < a.pixels.size(); i++) {
        const int d = std::abs(int(a.pixels[i]) - int(b.pixels[i]));
        total += d;
        if (d > 4) differ++;
    }
    std::printf("  %s: mean %.3f/255, %zu of %zu samples differ\n", what,
                total / double(a.pixels.size()), differ, a.pixels.size());
    check(differ < a.pixels.size() / 200, what);
}

}  // namespace

int main(int argc, char **argv) {
    bool validation = false;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--validation")) validation = true;
    log_set_level(LogLevel::Warn);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("lights\n  no video device: %s\n", SDL_GetError());
        return 77;
    }
    std::printf("clustered lights\n");

    const Backend backends[2] = {Backend::Vulkan, Backend::OpenGL};
    const char *names[2] = {"Vulkan", "OpenGL"};
    Shot omni[2], spot[2], portal_all[2], portal_one[2];

    for (int i = 0; i < 2; i++) {
        omni[i] = run(backends[i], SCENE_OMNI, 16, validation);
        spot[i] = run(backends[i], SCENE_SPOT, 16, validation);
        portal_all[i] = run(backends[i], SCENE_PORTAL, 16, validation);
        portal_one[i] = run(backends[i], SCENE_PORTAL, 1, validation);
    }
    if (!omni[0].ok && !omni[1].ok) {
        std::printf("  neither backend would start; skipping\n");
        SDL_Quit();
        return 77;
    }

    for (int i = 0; i < 2; i++) {
        if (!omni[i].ok) {
            std::printf("  %s: not available\n", names[i]);
            continue;
        }
        std::printf("  %-7s %s\n", names[i], omni[i].device.c_str());
        char what[200];

        // --- an omni light lights, and falls off
        std::snprintf(what, sizeof(what), "[%s] the omni light was collected",
                      names[i]);
        check(omni[i].lights == 1, what);
        std::snprintf(what, sizeof(what),
                      "[%s] and binned into froxels (%u assignments)", names[i],
                      omni[i].assignments);
        check(omni[i].assignments > 0, what);

        const float centre = mean_in(omni[i], 112, 112, 144, 144);
        const float corner = mean_in(omni[i], 4, 4, 36, 36);
        std::snprintf(what, sizeof(what),
                      "[%s] the wall is lit under the lamp (%.3f)", names[i],
                      double(centre));
        check(centre > 0.15f, what);
        std::snprintf(what, sizeof(what),
                      "[%s] and falls off towards the corners (%.3f < %.3f)",
                      names[i], double(corner), double(centre));
        check(corner < centre * 0.5f, what);

        // --- a spot stays in its cone
        const float in_cone = mean_in(spot[i], 118, 118, 138, 138);
        const float out_cone = mean_in(spot[i], 4, 118, 32, 138);
        std::snprintf(what, sizeof(what),
                      "[%s] the spot lights inside its cone (%.3f)", names[i],
                      double(in_cone));
        check(in_cone > 0.15f, what);
        std::snprintf(what, sizeof(what),
                      "[%s] and nothing outside it (%.4f)", names[i],
                      double(out_cone));
        check(out_cone < 0.02f, what);

        // --- THE PORTAL VIEW HAS ITS OWN GRID
        if (!portal_all[i].ok || !portal_one[i].ok) continue;
        const int x0 = int(portal_all[i].portal_x0) + 6;
        const int y0 = int(portal_all[i].portal_y0) + 6;
        const int x1 = int(portal_all[i].portal_x1) - 6;
        const int y1 = int(portal_all[i].portal_y1) - 6;
        std::snprintf(what, sizeof(what),
                      "[%s] the portal has a footprint on screen (%d,%d)-(%d,%d)",
                      names[i], x0, y0, x1, y1);
        check(x1 > x0 + 4 && y1 > y0 + 4, what);

        std::snprintf(what, sizeof(what),
                      "[%s] two views were clustered, not one (%u)", names[i],
                      portal_all[i].clustered_views);
        check(portal_all[i].clustered_views >= 2, what);

        const float through = mean_in(portal_all[i], x0, y0, x1, y1);
        const float through_unclustered = mean_in(portal_one[i], x0, y0, x1, y1);
        std::snprintf(what, sizeof(what),
                      "[%s] the far room's lamp reaches the portal view "
                      "(%.3f inside the aperture)",
                      names[i], double(through));
        check(through > 0.05f, what);
        std::snprintf(what, sizeof(what),
                      "[%s] and only because that view got its own froxel grid "
                      "(%.3f with a grid, %.3f without)",
                      names[i], double(through), double(through_unclustered));
        check(through > through_unclustered * 2.0f + 0.02f, what);

        // The near room is unlit either way: nothing outside the
        // aperture may change when the portal view's grid appears.
        std::printf("       through the aperture: %.3f with a grid for that "
                    "view, %.3f without; %u views clustered, %u assignments\n",
                    double(through), double(through_unclustered),
                    portal_all[i].clustered_views, portal_all[i].assignments);

        const float near_room = mean_in(portal_all[i], 4, 200, 60, 250);
        std::snprintf(what, sizeof(what),
                      "[%s] and the near room stays dark -- no light leaked "
                      "out of the hole (%.4f)",
                      names[i], double(near_room));
        check(near_room < 0.02f, what);
    }

    if (omni[0].ok && omni[1].ok) {
        compare(omni[0], omni[1], "omni agrees across backends");
        compare(spot[0], spot[1], "spot agrees across backends");
        compare(portal_all[0], portal_all[1], "through a portal agrees too");
    }

    SDL_Quit();
    std::printf("%s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
