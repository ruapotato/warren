// Warren -- shadows from point and spot lights, and all six faces
// of the cube pointing the way they should.
//
// An omni light's shadow is six frusta rendered into six tiles of an
// atlas, and the shader has to find the one a fragment belongs to and
// read it the same way round it was written. Get a face mirrored and
// the shadow on that face alone lands on the wrong side of its
// caster -- while the other five are perfect, the atlas looks full of
// sensible depth, and the symptom reads as a bias problem.
//
// So the test does not ask "is there a shadow". It works out, in
// world space, where the light would throw the caster's shadow --
// straight line from the light through the caster to the wall -- and
// requires the dark patch to be within a few pixels of there. Once
// per face, with the caster deliberately off the face's axis so that
// a mirrored face has somewhere else to put it.
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
#include "scene/scene_tree.h"

using namespace wr;
using namespace wr::rhi;

namespace {

constexpr uint32_t kSize = 256;
constexpr float kWall = 8.0f;      // the wall is this far from the light
constexpr float kCaster = 2.5f;    // and the caster this far
constexpr float kOffset = 0.9f;    // off the axis, so mirroring shows

// The six directions an omni's faces look in, in the order the
// renderer allocates them.
const Vec3 kAxis[6] = {{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                       {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
// A perpendicular for each, chosen so no two faces are offset the
// same way -- a test where every answer is "up" cannot tell up from
// a face that happens to agree about up.
const Vec3 kPerp[6] = {{0, 1, 0},  {0, 0, 1}, {1, 0, 0},
                       {0, 0, -1}, {0, 1, 0}, {1, 0, 0}};

struct Shot {
    std::vector<uint8_t> pixels;
    bool ok = false;
    std::string device;
    float expect_x = 0, expect_y = 0;   // where the shadow should be
    float caster_x = 0, caster_y = 0;   // and where the caster is
    std::vector<float> atlas;           // the raw depth the pass wrote
    uint32_t atlas_size = 0;
};

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

// `face` < 0 means a spot light pointed along +Z instead of an omni.
Shot run(Backend backend, int face, bool shadows, bool validation) {
    Shot out;
    WindowConfig wc;
    wc.backend = backend;
    wc.width = int(kSize);
    wc.height = int(kSize);
    wc.resizable = false;
    wc.debug = validation;
    wc.title = "punctual shadows";
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
    rs.shadows = false;                 // no sun, no cascades
    rs.image_based_lighting = false;    // and no sky light
    rs.punctual_shadows = shadows;
    rs.clear_colour = Color(0, 0, 0, 1);

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

    const int f = face < 0 ? 4 : face;   // a spot points along +Z
    const Vec3 axis = kAxis[f];
    const Vec3 perp = kPerp[f];
    const Vec3 light_pos(0, 0, 0);
    const Vec3 caster_pos = axis * kCaster + perp * kOffset;

    Ref<Material> white = Material::make(Color(0.9f, 0.9f, 0.9f, 1), 0.95f);

    // A wall square on to the light, and the caster in front of it.
    Vec3 wall_size = Vec3(14, 14, 14) - Vec3(std::fabs(axis.x) * 13.5f,
                                             std::fabs(axis.y) * 13.5f,
                                             std::fabs(axis.z) * 13.5f);
    add_box(scene, "wall", wall_size, axis * kWall, white);
    add_box(scene, "caster", {0.7f, 0.7f, 0.7f}, caster_pos, white);

    if (face < 0) {
        SpotLight3D *sp = new SpotLight3D();
        sp->set_name("spot");
        sp->set_position(light_pos);
        sp->look_at(axis * kWall);
        sp->colour = Color(1, 1, 1, 1);
        sp->energy = 120.0f;
        sp->range = 30.0f;
        sp->angle = deg2rad(40.0f);
        sp->angle_softness = 0.1f;
        scene->add_child(sp);
    } else {
        OmniLight3D *o = new OmniLight3D();
        o->set_name("omni");
        o->set_position(light_pos);
        o->colour = Color(1, 1, 1, 1);
        o->energy = 120.0f;
        o->range = 30.0f;
        scene->add_child(o);
    }

    // The camera sits behind and to one side of the light, looking at
    // the wall, so the caster does not stand in front of its own
    // shadow.
    Vec3 side = cross(axis, perp).normalized();
    Camera3D *cam = new Camera3D();
    cam->set_name("Camera");
    cam->set_fov_degrees(70.0f);
    cam->set_near(0.05f);
    cam->set_far(100.0f);
    cam->set_position(light_pos - axis * 3.0f + side * 2.2f + perp * 0.6f);
    cam->look_at(axis * kWall + perp * kOffset * (kWall / kCaster) * 0.6f);
    scene->add_child(cam);
    cam->make_current();

    tree->process(1.0f / 60.0f);

    // WHERE THE SHADOW HAS TO BE, from geometry alone: the light, the
    // caster's centre and the wall are collinear, so the shadow lands
    // where that line meets the wall.
    const float t = kWall / kCaster;
    const Vec3 expect = light_pos + (caster_pos - light_pos) * t;
    const Vec3 e = cam->project_point(expect, 1.0f);
    out.expect_x = e.x * float(kSize);
    out.expect_y = e.y * float(kSize);
    const Vec3 c = cam->project_point(caster_pos, 1.0f);
    out.caster_x = c.x * float(kSize);
    out.caster_y = c.y * float(kSize);

    CommandList *cmd = dev->begin_frame();
    if (cmd) {
        renderer.render(cmd, tree.get(), cam, target);
        dev->end_frame();
        dev->wait_idle();
        out.pixels.resize(size_t(kSize) * kSize * 4);
        out.ok = dev->read_texture(target, out.pixels.data(),
                                   out.pixels.size()) == out.pixels.size();
        out.atlas_size = renderer.read_shadow_atlas(&out.atlas);
    }

    tree.reset();
    white.reset();
    renderer.shutdown();
    destroy_device(dev);
    return out;
}

int g_fail = 0, g_checks = 0;
void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        g_fail++;
    }
}

float luma(const Shot &s, int x, int y) {
    size_t i = (size_t(y) * kSize + size_t(x)) * 4;
    return (0.2126f * float(s.pixels[i]) + 0.7152f * float(s.pixels[i + 1]) +
            0.0722f * float(s.pixels[i + 2])) /
           255.0f;
}

}  // namespace

int main(int argc, char **argv) {
    bool validation = false;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--validation")) validation = true;
    log_set_level(LogLevel::Warn);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("punctual shadows\n  no video device: %s\n", SDL_GetError());
        return 77;
    }
    std::printf("punctual shadows\n");

    const Backend backends[2] = {Backend::Vulkan, Backend::OpenGL};
    const char *names[2] = {"Vulkan", "OpenGL"};
    const char *face_name[7] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z", "spot"};

    bool any = false;
    for (int b = 0; b < 2; b++) {
        Shot probe = run(backends[b], 0, true, validation);
        if (!probe.ok) {
            std::printf("  %s: not available\n", names[b]);
            continue;
        }
        any = true;
        std::printf("  %-7s %s\n", names[b], probe.device.c_str());

        for (int case_i = 0; case_i < 7; case_i++) {
            const int face = case_i < 6 ? case_i : -1;
            const Shot on = (case_i == 0) ? probe
                                          : run(backends[b], face, true,
                                                validation);
            const Shot off = run(backends[b], face, false, validation);
            if (!on.ok || !off.ok) continue;

            // The shadow is what turning the pass on took away.
            double sx = 0, sy = 0;
            int n = 0;
            for (int y = 0; y < int(kSize); y++)
                for (int x = 0; x < int(kSize); x++) {
                    const float a = luma(off, x, y), c = luma(on, x, y);
                    if (a < 0.02f) continue;          // unlit background
                    if (c > a - 0.05f) continue;      // not darkened
                    sx += x;
                    sy += y;
                    n++;
                }
            char what[220];
            std::snprintf(what, sizeof(what),
                          "[%s %s] the light casts a shadow (%d pixels)",
                          names[b], face_name[case_i], n);
            check(n > 30, what);
            if (n <= 30) continue;
            sx /= n;
            sy /= n;

            const double dx = sx - on.expect_x, dy = sy - on.expect_y;
            const double err = std::sqrt(dx * dx + dy * dy);
            std::printf("       %-4s shadow at (%5.1f,%5.1f), geometry says "
                        "(%5.1f,%5.1f), off by %4.1f px\n",
                        face_name[case_i], sx, sy, double(on.expect_x),
                        double(on.expect_y), err);

            // WHERE THE GEOMETRY SAYS, not merely somewhere dark. A
            // mirrored face puts the shadow the same distance on the
            // other side of the axis, which is tens of pixels away
            // and nothing else is.
            std::snprintf(what, sizeof(what),
                          "[%s %s] and it lands where the light, the caster "
                          "and the wall put it (%.1f px out)",
                          names[b], face_name[case_i], err);
            check(err < 18.0, what);
        }
    }

    // --- AND THE ATLAS ITSELF, not only what it does to the picture.
    //
    // A depth image is where a shadow bug lives; the picture is only
    // where it is eventually noticed, and a face whose light barely
    // reaches the camera can be badly wrong while the frame looks
    // fine. Comparing the raw depth is the direct question.
    {
        const Shot v = run(Backend::Vulkan, 0, true, validation);
        const Shot g = run(Backend::OpenGL, 0, true, validation);
        if (v.atlas_size && v.atlas_size == g.atlas_size) {
            size_t differ = 0;
            double total = 0;
            float worst = 0;
            for (size_t i = 0; i < v.atlas.size(); i++) {
                const float d = std::fabs(v.atlas[i] - g.atlas[i]);
                total += d;
                worst = std::max(worst, d);
                if (d > 0.002f) differ++;
            }
            std::printf("  atlas %ux%u: mean |dz| %.6f, worst %.4f, %.3f%% of "
                        "texels differ by more than 0.002\n",
                        v.atlas_size, v.atlas_size, total / double(v.atlas.size()),
                        double(worst), 100.0 * double(differ) /
                                           double(v.atlas.size()));
            check(differ < v.atlas.size() / 200,
                  "the two backends write the same depth into the atlas");
        }
    }

    if (!any) {
        std::printf("  neither backend would start; skipping\n");
        SDL_Quit();
        return 77;
    }
    SDL_Quit();
    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
