// Warren -- the voxel plugin, and whether level of detail leaves a
// hole in the world.
//
// Also the first test of the plugin ABI end to end: the shared library
// is loaded the way the engine loads it, its class arrives in the same
// ClassDB the engine uses, and it is driven entirely through
// reflection -- so this fails if the boundary breaks, not only if the
// terrain does.
//
// The claim LOD makes is that the horizon costs the same as your feet.
// The risk it runs is that the shells -- level 0 out to `lod_distance`,
// level 1 to twice that, and so on -- do not quite meet, and the
// player walks to a ring of missing ground. So: stream a world, then
// sample a grid of points and require every one of them to be covered
// by some chunk.
//
// Headless. Meshing is CPU work on the job system and a chunk's
// MeshInstance3D needs no device until something draws it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "core/bind.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/object.h"
#include "plugin/host.h"
#include "scene/nodes.h"
#include "scene/scene_tree.h"

using namespace wr;

namespace {

int g_fail = 0, g_checks = 0;
void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        g_fail++;
    }
}

constexpr float kViewDistance = 224.0f;
constexpr float kLodDistance = 48.0f;

struct Result {
    bool ok = false;
    uint32_t triangles = 0;
    uint32_t live = 0;
    int levels_used = 0;
    int uncovered = 0;
    int too_coarse = 0;
    int worst_coarse = 0;
    int sampled = 0;
    std::string report;
};

// Stream a world to a standstill and measure it.
Result stream(int max_lod) {
    Result r;
    SceneTree tree;
    Node3D *scene = new Node3D();
    scene->set_name("Scene");
    tree.set_scene(scene);

    Object *o = ClassDB::instantiate("VoxelTerrain3D");
    if (!o) return r;
    Node3D *terrain = static_cast<Node3D *>(o);
    scene->add_child(terrain);

    Node3D *viewer = new Node3D();
    viewer->set_name("Viewer");
    viewer->set_position({0, 40, 0});
    scene->add_child(viewer);

    // Everything through reflection, which is how a game or a script
    // would reach a plugin's class.
    terrain->set("view_distance", Variant(double(kViewDistance)));
    terrain->set("max_lod", Variant(int64_t(max_lod)));
    terrain->set("lod_distance", Variant(double(kLodDistance)));
    terrain->set("generate_collision", Variant(false));
    // Unthrottled: this is a test, not a frame.
    terrain->set("queue_per_frame", Variant(int64_t(4096)));
    terrain->set("upload_per_frame", Variant(int64_t(4096)));
    terrain->set("max_in_flight", Variant(int64_t(4096)));
    Array vp{Variant(static_cast<Object *>(viewer))};
    terrain->callv("set_viewer", vp);

    // A handful of passes: each one queues what is missing and takes
    // delivery of what finished, and a coarse chunk can only be
    // decided on once its neighbours exist.
    for (int i = 0; i < 12; i++) {
        tree.process(1.0f / 60.0f);
        terrain->callv("wait_for_chunks", {});
    }
    tree.process(1.0f / 60.0f);

    r.report = terrain->callv("report", {}).to_string();
    r.ok = true;

    // --- coverage, on a grid through the middle of the world
    //
    // Sampled at the ground, because that is where the player is and
    // where a seam between shells would show. The field is analytic,
    // so the height is known without asking any chunk.
    //
    // AND AT WHAT LEVEL, which is the check with teeth. "Is anything
    // loaded here" is nearly free to satisfy: a level-4 chunk is 512
    // metres across, so one of them covers the whole sample area and
    // a scheme that skipped level 0 entirely would still pass. The
    // real guarantee is that ground is drawn at the level its
    // distance calls for -- finer is fine, since shells overlap, but
    // coarser means the player is standing on a sixteen-metre cell.
    const Vec3 eye = viewer->global_position();
    const float step = 16.0f;
    for (float z = -kViewDistance + step; z < kViewDistance; z += step) {
        for (float x = -kViewDistance + step; x < kViewDistance; x += step) {
            if (std::sqrt(x * x + z * z) > kViewDistance * 0.8f) continue;
            Array hp{Variant(double(x)), Variant(double(z))};
            const float h = float(terrain->callv("height_at", hp).to_float());
            const Vec3 p(x, h, z);
            Array pp{Variant(p)};
            r.sampled++;
            const int got = int(terrain->callv("loaded_lod", pp).to_int());
            if (got < 0) {
                r.uncovered++;
                continue;
            }
            Array dp{Variant(double((p - eye).length()))};
            const int want = int(terrain->callv("lod_at", dp).to_int());
            if (got > want) {
                r.too_coarse++;
                r.worst_coarse = std::max(r.worst_coarse, got - want);
            }
        }
    }

    // --- what got built
    for (int l = 0; l < 8; l++) {
        Array pp{Variant(Vec3(0, 0, 0))};
        (void)pp;
    }
    // The report carries the per-level counts; parse the live total
    // and the triangles back out of it rather than widening the API
    // for a test.
    const std::string &s = r.report;
    size_t at = s.find("voxel: ");
    if (at != std::string::npos) r.live = uint32_t(std::atoi(s.c_str() + at + 7));
    at = s.find("(lod ");
    if (at != std::string::npos) {
        const char *p = s.c_str() + at + 5;
        while (*p && *p != ')') {
            if (std::atoi(p) > 0) r.levels_used++;
            while (*p && *p != '/' && *p != ')') p++;
            if (*p == '/') p++;
        }
    }
    at = s.find(", ");
    at = s.find("triangles");
    if (at != std::string::npos) {
        // Walk back over the number in front of the word.
        size_t end = at - 1;
        while (end > 0 && !std::isdigit((unsigned char)s[end])) end--;
        size_t start = end;
        while (start > 0 && std::isdigit((unsigned char)s[start - 1])) start--;
        r.triangles = uint32_t(std::atoi(s.c_str() + start));
    }

    tree.set_scene(nullptr);
    return r;
}

}  // namespace

int main() {
    log_set_level(LogLevel::Warn);
    ClassDB::register_all();
    Jobs::init(0);

    std::printf("voxel level of detail\n");

    PluginHost host;
    PluginContext ctx;
    const std::string dir = PluginHost::resolve_directory("");
    ctx.directory = dir.c_str();
    const int loaded = host.load_directory(dir, ctx);
    ClassDB::register_all();
    if (!loaded || !ClassDB::get("VoxelTerrain3D")) {
        std::printf("  the voxel plugin is not built; skipping\n");
        Jobs::shutdown();
        return 77;
    }
    std::printf("  loaded %d plugin%s from %s\n", loaded, loaded == 1 ? "" : "s",
                dir.c_str());

    const Result lod = stream(4);
    const Result flat = stream(0);
    check(lod.ok && flat.ok, "the terrain can be instantiated by name");
    if (!lod.ok || !flat.ok) {
        Jobs::shutdown();
        std::printf("FAILED\n");
        return 1;
    }
    std::printf("  lod 4: %s\n", lod.report.c_str());
    std::printf("         %d of %d ground samples covered, %d coarser than "
                "their distance calls for\n",
                lod.sampled - lod.uncovered, lod.sampled, lod.too_coarse);
    std::printf("  lod 0: %s\n", flat.report.c_str());
    std::printf("         %d of %d ground samples covered, %d coarser than "
                "their distance calls for\n",
                flat.sampled - flat.uncovered, flat.sampled, flat.too_coarse);

    char what[220];

    // --- LOD must actually be cheaper, and by a lot
    std::snprintf(what, sizeof(what),
                  "level of detail cuts the triangles (%u against %u)",
                  lod.triangles, flat.triangles);
    check(lod.triangles > 0 && flat.triangles > 0 &&
              lod.triangles * 2 < flat.triangles,
          what);

    std::snprintf(what, sizeof(what), "and the chunk count with it (%u against %u)",
                  lod.live, flat.live);
    check(lod.live > 0 && lod.live < flat.live, what);

    std::snprintf(what, sizeof(what), "using more than one level (%d in use)",
                  lod.levels_used);
    check(lod.levels_used >= 3, what);

    // --- and it must cover the same ground
    //
    // This is the check the shell algorithm exists to pass. A gap
    // between two levels is a ring of missing world that only appears
    // at one distance from the player, which is exactly the kind of
    // bug that ships.
    std::snprintf(what, sizeof(what),
                  "no gap between the shells: %d of %d ground samples "
                  "uncovered",
                  lod.uncovered, lod.sampled);
    check(lod.sampled > 100 && lod.uncovered == 0, what);

    std::snprintf(what, sizeof(what),
                  "and the flat world covers the same %d samples (%d uncovered)",
                  flat.sampled, flat.uncovered);
    check(flat.uncovered == 0, what);

    // THE CHECK WITH TEETH. Ground must be drawn at the level its
    // distance calls for. Finer is allowed -- the shells overlap on
    // purpose -- but coarser means the shell boundaries have let a
    // ring of the world fall through to a level that should not own
    // it, and the player walks onto sixteen-metre cells.
    std::snprintf(what, sizeof(what),
                  "every sample is at the level its distance calls for "
                  "(%d too coarse, worst by %d levels)",
                  lod.too_coarse, lod.worst_coarse);
    check(lod.too_coarse == 0, what);

    Jobs::shutdown();
    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
