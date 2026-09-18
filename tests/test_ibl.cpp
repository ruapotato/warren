// Manifold -- image-based lighting, and whether the sky is baked the
// right way up.
//
// A cubemap has six faces, each with a uv orientation, and the
// mapping from a texel to the direction it looks in has twelve signs
// in it. Get one wrong and the world is lit by a sky reflected in an
// axis -- which looks entirely plausible. The light comes from
// somewhere, surfaces face towards and away from it, and nothing in
// the picture says the somewhere is wrong.
//
// So the test does not look at the picture. It renders a white sphere
// lit only by the environment, works out the surface normal at each
// pixel from its position on the sphere, and requires the brightest
// normal to be the one pointing AT THE SUN -- where the sun's
// direction comes from the C++ side and owes nothing to the shader's
// idea of which way is up.
//
// Sampling is done by the hardware through the engine's own shader,
// so a bake and a lookup that are consistently wrong together still
// fail: the sun would end up somewhere the renderer never put it.
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

using namespace mf;
using namespace mf::rhi;

namespace {

constexpr uint32_t kSize = 256;
// Off every axis, so a swapped pair of faces cannot pass by symmetry.
const Vec3 kSunTravel = Vec3(-0.62f, -0.55f, -0.56f).normalized();
constexpr float kSphereRadius = 1.0f;
constexpr float kCameraZ = 3.2f;

struct Shot {
    std::vector<uint8_t> pixels;
    bool ok = false;
    std::string device;
    // The sphere's footprint, in pixels.
    float cx = 0, cy = 0, r = 0;
};

Shot run(Backend backend, bool ibl, float roughness, bool validation) {
    Shot out;
    WindowConfig wc;
    wc.backend = backend;
    wc.width = int(kSize);
    wc.height = int(kSize);
    wc.resizable = false;
    wc.debug = validation;
    wc.title = "ibl";
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
    rs.draw_sky = false;             // a flat clear behind the sphere
    rs.shadows = false;
    rs.punctual_lights = false;
    rs.image_based_lighting = ibl;
    rs.clear_colour = Color(0, 0, 0, 1);

    Renderer renderer;
    if (!renderer.init(dev, rs)) {
        destroy_device(dev);
        return out;
    }
    // NO DIRECT SUN. The sky still has a sun in it -- that is what
    // makes the environment directional -- but nothing reaches the
    // sphere except through the baked cubemaps, so every difference
    // in the picture is image-based lighting and nothing else.
    renderer.sun_direction = kSunTravel;
    renderer.sun_energy = 0.0f;
    renderer.ambient_energy = 0.0f;
    renderer.fog_density = 0.0f;
    renderer.settings().env_intensity = 1.0f;

    auto tree = std::make_unique<SceneTree>();
    Node3D *scene = new Node3D();
    scene->set_name("Scene");
    tree->set_scene(scene);

    Ref<Material> mat = Material::make(Color(1, 1, 1, 1), roughness);
    MeshInstance3D *mi = new MeshInstance3D();
    mi->set_name("ball");
    mi->mesh = Mesh::sphere(kSphereRadius, 48, 96);
    mi->set_material(0, mat.get());
    scene->add_child(mi);

    Camera3D *cam = new Camera3D();
    cam->set_name("Camera");
    cam->set_fov_degrees(45.0f);
    cam->set_near(0.05f);
    cam->set_far(100.0f);
    // Straight down -Z with no rotation, so a view-space normal IS a
    // world-space normal and the test needs no basis of its own.
    cam->set_position({0, 0, kCameraZ});
    scene->add_child(cam);
    cam->make_current();

    tree->process(1.0f / 60.0f);

    CommandList *cmd = dev->begin_frame();
    if (!cmd) {
        tree.reset();
        mat.reset();
        renderer.shutdown();
        destroy_device(dev);
        return out;
    }
    // Twice: the first frame bakes the environment, and the bake is
    // what the second frame reads.
    renderer.render(cmd, tree.get(), cam, target);
    dev->end_frame();
    cmd = dev->begin_frame();
    if (cmd) {
        renderer.render(cmd, tree.get(), cam, target);
        dev->end_frame();
    }
    dev->wait_idle();

    // Where the sphere landed, from the camera rather than by
    // guessing: the centre projects to the middle, and a point on the
    // limb gives the radius.
    const Vec3 centre = cam->project_point(Vec3(0, 0, 0), 1.0f);
    const Vec3 edge = cam->project_point(Vec3(kSphereRadius, 0, 0), 1.0f);
    out.cx = centre.x * float(kSize);
    out.cy = centre.y * float(kSize);
    out.r = std::fabs(edge.x - centre.x) * float(kSize);

    out.pixels.resize(size_t(kSize) * kSize * 4);
    out.ok = dev->read_texture(target, out.pixels.data(), out.pixels.size()) ==
             out.pixels.size();

    tree.reset();
    mat.reset();
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

float luma(const Shot &s, int x, int y) {
    size_t i = (size_t(y) * kSize + size_t(x)) * 4;
    return (0.2126f * float(s.pixels[i]) + 0.7152f * float(s.pixels[i + 1]) +
            0.0722f * float(s.pixels[i + 2])) /
           255.0f;
}

// The outward normal of the sphere at a pixel, or false off the limb.
// The camera looks down -Z unrotated, so this is a world normal.
bool normal_at(const Shot &s, int x, int y, Vec3 *out) {
    // Sampled a little inside the limb: right at the edge the sphere
    // is one pixel of surface stretched over a whole texel and the
    // antialiasing has nothing to work with.
    const float u = (float(x) + 0.5f - s.cx) / (s.r * 0.94f);
    const float v = -(float(y) + 0.5f - s.cy) / (s.r * 0.94f);
    const float d = u * u + v * v;
    if (d > 0.82f) return false;
    *out = Vec3(u, v, std::sqrt(1.0f - d)).normalized();
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    bool validation = false;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--validation")) validation = true;
    log_set_level(LogLevel::Warn);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("ibl\n  no video device: %s\n", SDL_GetError());
        return 77;
    }
    std::printf("image-based lighting\n");

    const Vec3 to_sun = -kSunTravel;
    const Backend backends[2] = {Backend::Vulkan, Backend::OpenGL};
    const char *names[2] = {"Vulkan", "OpenGL"};
    Shot diffuse[2], flat[2], mirror[2];

    for (int i = 0; i < 2; i++) {
        diffuse[i] = run(backends[i], true, 0.95f, validation);
        flat[i] = run(backends[i], false, 0.95f, validation);
        mirror[i] = run(backends[i], true, 0.05f, validation);
    }
    if (!diffuse[0].ok && !diffuse[1].ok) {
        std::printf("  neither backend would start; skipping\n");
        SDL_Quit();
        return 77;
    }

    for (int i = 0; i < 2; i++) {
        if (!diffuse[i].ok) {
            std::printf("  %s: not available\n", names[i]);
            continue;
        }
        std::printf("  %-7s %s\n", names[i], diffuse[i].device.c_str());
        char what[240];

        // --- the environment has to do something at all
        double on = 0, off = 0;
        int n = 0;
        Vec3 nrm;
        for (int y = 0; y < int(kSize); y++)
            for (int x = 0; x < int(kSize); x++) {
                if (!normal_at(diffuse[i], x, y, &nrm)) continue;
                on += luma(diffuse[i], x, y);
                off += luma(flat[i], x, y);
                n++;
            }
        on /= std::max(n, 1);
        off /= std::max(n, 1);
        std::snprintf(what, sizeof(what),
                      "[%s] the environment lights the sphere (%.3f against "
                      "%.3f with it off)",
                      names[i], on, off);
        check(n > 1000 && on > off * 1.15 + 0.01, what);

        // --- AND IT POINTS THE RIGHT WAY
        //
        // Brightness against the cosine to the sun, in buckets. A
        // correctly oriented environment is monotonic in it: the
        // normal facing the sun is brightest, the one facing away is
        // darkest. A flipped or rotated cube breaks the ordering
        // without changing the average at all, which is why the
        // average above is not enough.
        constexpr int kBuckets = 8;
        double sum[kBuckets] = {};
        int count[kBuckets] = {};
        for (int y = 0; y < int(kSize); y++)
            for (int x = 0; x < int(kSize); x++) {
                if (!normal_at(diffuse[i], x, y, &nrm)) continue;
                const float c = dot(nrm, to_sun);          // -1 .. 1
                int b = int((c * 0.5f + 0.5f) * kBuckets);
                b = std::clamp(b, 0, kBuckets - 1);
                sum[b] += luma(diffuse[i], x, y);
                count[b]++;
            }
        double avg[kBuckets];
        int populated = 0;
        for (int b = 0; b < kBuckets; b++) {
            avg[b] = count[b] ? sum[b] / count[b] : -1.0;
            if (count[b] > 20) populated++;
        }
        std::printf("       brightness by cos(n, sun):");
        for (int b = 0; b < kBuckets; b++)
            if (avg[b] >= 0) std::printf(" %.3f", avg[b]);
        std::printf("\n");

        std::snprintf(what, sizeof(what),
                      "[%s] the sphere covers a range of normals (%d buckets)",
                      names[i], populated);
        check(populated >= 5, what);

        // MONOTONIC IN THE COSINE, which is the actual property and
        // a stronger one than "the extremes are in the right place".
        // A sphere seen head-on only shows the normals facing the
        // camera, so which buckets are populated depends on where the
        // sun is; what cannot depend on anything is that brighter
        // means more towards the sun, every step of the way. Rotate
        // or flip the cube and the ordering breaks while the average
        // over the sphere does not move at all.
        int first = -1, last = -1, inversions = 0;
        double worst_drop = 0.0;
        for (int b = 0; b < kBuckets; b++) {
            if (count[b] <= 20) continue;
            if (first < 0) first = b;
            if (last >= 0 && avg[b] < avg[last] - 1e-4) {
                inversions++;
                worst_drop = std::max(worst_drop, avg[last] - avg[b]);
            }
            last = b;
        }
        std::snprintf(what, sizeof(what),
                      "[%s] brightness rises with the cosine to the sun, "
                      "every step (%d inversions, worst %.4f)",
                      names[i], inversions, worst_drop);
        check(inversions == 0, what);

        std::snprintf(what, sizeof(what),
                      "[%s] and by a margin worth measuring (%.3f facing the "
                      "sun against %.3f facing away)",
                      names[i], first >= 0 ? avg[last] : 0.0,
                      first >= 0 ? avg[first] : 0.0);
        check(first >= 0 && avg[last] > avg[first] * 1.08, what);

        // --- the specular half
        //
        // A near-mirror sphere reflects the sky, so it must be
        // brighter than a rough one somewhere and must have a much
        // wider range: a mirror shows the sun's disc as a small very
        // bright spot, a rough surface spreads it out.
        double mirror_max = 0, diffuse_max = 0;
        for (int y = 0; y < int(kSize); y++)
            for (int x = 0; x < int(kSize); x++) {
                if (!normal_at(mirror[i], x, y, &nrm)) continue;
                mirror_max = std::max(mirror_max, double(luma(mirror[i], x, y)));
                diffuse_max =
                    std::max(diffuse_max, double(luma(diffuse[i], x, y)));
            }
        std::snprintf(what, sizeof(what),
                      "[%s] a smooth sphere reflects a brighter sun than a "
                      "rough one (%.3f against %.3f)",
                      names[i], mirror_max, diffuse_max);
        check(mirror_max > diffuse_max + 0.02, what);
    }

    // --- and the two backends agree
    //
    // Not to the bit. Mip generation on a cubemap is a driver's own
    // box filter on OpenGL and a chain of blits on Vulkan, and the
    // prefilter reads those mips, so the environment differs in its
    // last few bits and the difference is largest on the shiniest
    // surface in the picture. It must stay a rounding difference:
    // smooth, small and everywhere, never a shape.
    if (diffuse[0].ok && diffuse[1].ok) {
        double total = 0;
        int worst = 0;
        for (size_t i = 0; i < diffuse[0].pixels.size(); i++) {
            const int d = std::abs(int(diffuse[0].pixels[i]) -
                                   int(diffuse[1].pixels[i]));
            total += d;
            worst = std::max(worst, d);
        }
        const double mean = total / double(diffuse[0].pixels.size());
        std::printf("  difference: mean %.4f/255, worst %d\n", mean, worst);
        check(mean < 0.5 && worst < 24,
              "the backends agree about the environment");
    }

    SDL_Quit();
    std::printf("%s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
