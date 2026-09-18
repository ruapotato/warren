// Warren -- bones, clips, and a vertex that ends up somewhere else.
//
// Skinning is a chain of five things that each look right on their
// own and produce a body inside out when one of them is subtly wrong:
// the bone hierarchy, the inverse bind matrices, the clip's sampling,
// the pose resolve, and the blend of four matrices per vertex. A
// screenshot cannot tell you which of the five it was.
//
// So this checks each link against an answer worked out by hand, and
// then the whole chain against a vertex whose skinned position can be
// written down: a two-bone arm, bent ninety degrees, with a point at
// the end of it. There is exactly one place that point can be.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "anim/clip.h"
#include "anim/skeleton.h"
#include "core/log.h"
#include "resource/packed_scene.h"
#include "resource/resource.h"
#include "scene/animated.h"
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
void check_near(float got, float want, float tol, const char *what) {
    g_checks++;
    if (!(std::fabs(got - want) <= tol)) {
        std::printf("  FAIL  %s: got %.5f, wanted %.5f +- %.4f\n", what,
                    double(got), double(want), double(tol));
        g_fail++;
    }
}
void check_vec(const Vec3 &got, const Vec3 &want, float tol, const char *what) {
    g_checks++;
    if ((got - want).length() > tol) {
        std::printf("  FAIL  %s: got (%.4f %.4f %.4f), wanted (%.4f %.4f %.4f)\n",
                    what, double(got.x), double(got.y), double(got.z),
                    double(want.x), double(want.y), double(want.z));
        g_fail++;
    }
}

// A TWO-BONE ARM ALONG +Y. Shoulder at the origin, elbow a metre up,
// and the hand a metre above that. Small enough to reason about and
// the same shape as every limb in the game.
Ref<Skeleton> arm() {
    Ref<Skeleton> s(new Skeleton());
    s->add("shoulder", -1, Transform3D(Basis(), Vec3(0, 0, 0)));
    s->add("elbow", 0, Transform3D(Basis(), Vec3(0, 1, 0)));
    s->add("hand", 1, Transform3D(Basis(), Vec3(0, 1, 0)));
    s->compute_inverse_binds();
    return s;
}

void write_file(const std::string &path, const void *data, size_t n) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(data, 1, n, f);
    std::fclose(f);
}

// A SKINNED GLB, BUILT BY HAND.
//
// Two bones and two vertices, each bound rigidly to one of them, plus
// a clip that turns the second bone. Everything the importer has to
// get right is in here and nothing else is: joints as unsigned bytes,
// weights as floats, an explicit inverse-bind accessor, and an
// animation channel naming a node rather than a bone.
std::vector<uint8_t> build_skinned_glb() {
    const float positions[6] = {0, 0, 0,   0, 1, 0};
    const float normals[6] = {0, 0, 1,   0, 0, 1};
    const uint8_t joints[8] = {0, 0, 0, 0,   1, 0, 0, 0};
    const float weights[8] = {1, 0, 0, 0,   1, 0, 0, 0};
    const uint16_t indices[3] = {0, 1, 0};
    // Inverse binds: bone 0 rests at the origin, bone 1 a metre up.
    // Column-major, so the translation is elements 12..14.
    const float ibm[32] = {
        1,0,0,0,  0,1,0,0,  0,0,1,0,   0, 0,0,1,
        1,0,0,0,  0,1,0,0,  0,0,1,0,   0,-1,0,1,
    };
    // The clip: bone 1 turns a quarter turn about +Z over one second.
    const float times[2] = {0.0f, 1.0f};
    const float rots[8] = {0, 0, 0, 1,
                           0, 0, 0.70710678f, 0.70710678f};

    std::vector<uint8_t> bin;
    auto append = [&](const void *p, size_t n) {
        const uint8_t *b = (const uint8_t *)p;
        bin.insert(bin.end(), b, b + n);
        while (bin.size() % 4) bin.push_back(0);
    };
    const size_t pos_off = 0;               append(positions, sizeof(positions));
    const size_t nrm_off = bin.size();      append(normals, sizeof(normals));
    const size_t joint_off = bin.size();    append(joints, sizeof(joints));
    const size_t weight_off = bin.size();   append(weights, sizeof(weights));
    const size_t idx_off = bin.size();      append(indices, sizeof(indices));
    const size_t ibm_off = bin.size();      append(ibm, sizeof(ibm));
    const size_t time_off = bin.size();     append(times, sizeof(times));
    const size_t rot_off = bin.size();      append(rots, sizeof(rots));

    char json[4096];
    std::snprintf(json, sizeof(json), R"({
"asset":{"version":"2.0","generator":"warren test"},
"scene":0,
"scenes":[{"nodes":[0,1]}],
"nodes":[
 {"name":"Body","mesh":0,"skin":0},
 {"name":"Root","children":[2]},
 {"name":"Upper","children":[3]},
 {"name":"Lower","translation":[0,1,0]}],
"skins":[{"name":"Arm","joints":[2,3],"inverseBindMatrices":5}],
"meshes":[{"name":"Skin","primitives":[
  {"attributes":{"POSITION":0,"NORMAL":1,"JOINTS_0":2,"WEIGHTS_0":3},
   "indices":4}]}],
"animations":[{"name":"Bend","channels":[
   {"sampler":0,"target":{"node":3,"path":"rotation"}}],
  "samplers":[{"input":6,"output":7,"interpolation":"LINEAR"}]}],
"accessors":[
 {"bufferView":0,"componentType":5126,"count":2,"type":"VEC3","min":[0,0,0],"max":[0,1,0]},
 {"bufferView":1,"componentType":5126,"count":2,"type":"VEC3"},
 {"bufferView":2,"componentType":5121,"count":2,"type":"VEC4"},
 {"bufferView":3,"componentType":5126,"count":2,"type":"VEC4"},
 {"bufferView":4,"componentType":5123,"count":3,"type":"SCALAR"},
 {"bufferView":5,"componentType":5126,"count":2,"type":"MAT4"},
 {"bufferView":6,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},
 {"bufferView":7,"componentType":5126,"count":2,"type":"VEC4"}],
"bufferViews":[
 {"buffer":0,"byteOffset":%zu,"byteLength":24},
 {"buffer":0,"byteOffset":%zu,"byteLength":24},
 {"buffer":0,"byteOffset":%zu,"byteLength":8},
 {"buffer":0,"byteOffset":%zu,"byteLength":32},
 {"buffer":0,"byteOffset":%zu,"byteLength":6},
 {"buffer":0,"byteOffset":%zu,"byteLength":128},
 {"buffer":0,"byteOffset":%zu,"byteLength":8},
 {"buffer":0,"byteOffset":%zu,"byteLength":32}],
"buffers":[{"byteLength":%zu}]
})", pos_off, nrm_off, joint_off, weight_off, idx_off, ibm_off, time_off,
     rot_off, bin.size());

    std::string text(json);
    while (text.size() % 4) text.push_back(' ');
    std::vector<uint8_t> glb;
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; i++) glb.push_back(uint8_t(v >> (8 * i)));
    };
    glb.insert(glb.end(), {'g', 'l', 'T', 'F'});
    u32(2);
    u32(uint32_t(12 + 8 + text.size() + 8 + bin.size()));
    u32(uint32_t(text.size()));
    u32(0x4E4F534Au);
    glb.insert(glb.end(), text.begin(), text.end());
    u32(uint32_t(bin.size()));
    u32(0x004E4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

// The skinned position of a vertex, the way the vertex shader does
// it: sum the bone matrices by weight, then transform once.
Vec3 skin_point(const Pose &pose, const SkinVertex &sv, const Vec3 &p) {
    Vec3 out;
    float total = 0.0f;
    for (int k = 0; k < 4; k++) {
        const float w = float(sv.weights[k]) / 255.0f;
        if (w <= 0.0f) continue;
        const int b = sv.joints[k];
        if (b >= pose.count()) continue;
        out += pose.skin[size_t(b)].xform(p) * w;
        total += w;
    }
    return total > 0.0f ? out / total : p;
}

}  // namespace

int main() {
    // Unbuffered: a test that dies half way through must still have
    // printed what it managed, or the failure it reports is "nothing".
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    log_set_level(LogLevel::Error);
    ClassDB::register_all();
    std::printf("anim\n");

    // ------------------------------------------------ the hierarchy
    {
        Ref<Skeleton> s = arm();
        check(s->count() == 3, "three bones go in");
        check(s->find("elbow") == 1, "and can be found by name");
        check(s->find("nothing") == -1, "a name that is not there is -1");
        check(s->ordered(), "parents come before children");

        // The rest pose in model space: the chain adds up.
        check_vec(s->global_rest(2).origin, Vec3(0, 2, 0), 1e-5f,
                  "the hand rests two metres up");
        // And the inverse bind undoes exactly that.
        check_vec(s->bones[2].inverse_bind.xform(Vec3(0, 2, 0)), Vec3(0, 0, 0),
                  1e-5f, "its inverse bind takes it back to the origin");

        // AN OUT-OF-ORDER RIG IS SORTED, NOT REFUSED. Exporters do
        // emit them, and the pose resolver's single forward pass is
        // only correct on a sorted one.
        Skeleton bad;
        bad.bones.resize(2);
        bad.bones[0].name = "child";
        bad.bones[0].parent = 1;
        bad.bones[0].rest = Transform3D(Basis(), Vec3(0, 1, 0));
        bad.bones[1].name = "root";
        bad.bones[1].parent = -1;
        check(!bad.ordered(), "a child before its parent is noticed");
        const std::vector<int> remap = bad.sort_hierarchically();
        check(bad.ordered(), "and put right");
        check(bad.bones[remap[0]].name == "child"
                      && bad.bones[remap[1]].name == "root",
              "with a mapping that says where everything went");
        check(bad.bones[remap[0]].parent == remap[1],
              "and parents renumbered to match");
    }

    // ------------------------------------------------ resolving a pose
    {
        Ref<Skeleton> s = arm();
        Pose pose;
        pose.bind(s.get());
        pose.resolve();
        check_vec(pose.global[2].origin, Vec3(0, 2, 0), 1e-5f,
                  "an unposed body is at its rest");
        // Every skin matrix is the identity at rest: that is what
        // "bound in this pose" means, and a rig where it is not is a
        // rig whose mesh explodes on the first frame.
        float worst = 0.0f;
        for (int i = 0; i < 3; i++)
            worst = std::max(worst,
                             (pose.skin[size_t(i)].xform(Vec3(1, 2, 3))
                              - Vec3(1, 2, 3)).length());
        check_near(worst, 0.0f, 1e-5f, "and every skin matrix is the identity");

        // BEND THE ELBOW NINETY DEGREES about +Z. The forearm points
        // along +Y from a point one metre up, so afterwards the hand
        // is one along and one up: (-1, 1, 0) for a positive turn
        // about Z, which takes +Y to -X.
        pose.local[1].basis = Basis(Quat::from_axis_angle(Vec3(0, 0, 1), deg2rad(90.0f)));
        pose.resolve();
        check_vec(pose.global[2].origin, Vec3(-1, 1, 0), 1e-4f,
                  "bending the elbow puts the hand where trigonometry says");
        // And the hand's own skin matrix moves a point bound at the
        // hand by the same amount.
        check_vec(pose.skin[2].xform(Vec3(0, 2, 0)), Vec3(-1, 1, 0), 1e-4f,
                  "and its skin matrix carries the vertex with it");
    }

    // ------------------------------------------------ sampling a clip
    {
        Ref<Skeleton> s = arm();
        Ref<AnimationClip> c(new AnimationClip());
        c->name = "bend";
        AnimationClip::BoneTrack tr;
        tr.bone_name = "elbow";
        tr.rotation.times = {0.0f, 2.0f};
        tr.rotation.values = {Quat(),
                              Quat::from_axis_angle(Vec3(0, 0, 1), deg2rad(90.0f))};
        c->tracks.push_back(tr);
        c->compute_duration();
        check_near(c->duration, 2.0f, 1e-6f, "a clip is as long as its last key");
        check(c->retarget(*s) == 1, "and finds the bone it names");

        Pose pose;
        pose.bind(s.get());
        c->sample(pose, 0.0f);
        pose.resolve();
        check_vec(pose.global[2].origin, Vec3(0, 2, 0), 1e-4f,
                  "at the first key the pose is the first key");
        // AT THE LAST KEY OF A LOOPING CLIP YOU ARE BACK AT THE
        // FIRST, which is what looping means and is why a walk cycle
        // is authored with its last frame equal to its first. Asked
        // just short of the end, the pose is the end.
        c->sample(pose, 2.0f - 1e-4f);
        pose.resolve();
        check_vec(pose.global[2].origin, Vec3(-1, 1, 0), 1e-3f,
                  "and just before the end, the end");
        c->sample(pose, 2.0f);
        pose.resolve();
        check_vec(pose.global[2].origin, Vec3(0, 2, 0), 1e-4f,
                  "while the last instant of a loop is its first");

        // HALFWAY IS FORTY-FIVE DEGREES, WHICH IS SLERP AND NOT LERP.
        //
        // Four floats lerped between those two quaternions and
        // renormalised gives 45 degrees here too -- the two agree at
        // the midpoint of a 90 degree arc, which is exactly why a
        // midpoint test does not catch a lerp. A quarter of the way
        // along is where they differ: slerp gives 22.5 degrees and
        // lerp gives about 20.7.
        c->sample(pose, 0.5f);
        pose.resolve();
        const Vec3 quarter = pose.global[2].origin;
        const float ang = deg2rad(22.5f);
        check_vec(quarter, Vec3(0, 1, 0) + Vec3(-std::sin(ang), std::cos(ang), 0),
                  2e-3f, "a quarter of the way along is a slerp, not a lerp");

        // A clip that loops comes back round; one that does not, holds.
        c->loops = true;
        check_near(c->wrap(2.5f), 0.5f, 1e-5f, "a looping clip wraps");
        c->loops = false;
        check_near(c->wrap(2.5f), 2.0f, 1e-5f, "and one that does not, holds");
        check_near(c->wrap(-1.0f), 0.0f, 1e-5f, "at both ends");

        // A BONE THE CLIP DOES NOT TOUCH GOES BACK TO ITS REST, and
        // does not keep whatever the last clip did to it. This is the
        // difference between a reload animation ending and the arm
        // staying up for the rest of the round.
        pose.local[2].basis = Basis(Quat::from_axis_angle(Vec3(1, 0, 0), 1.0f));
        pose.reset();
        c->sample(pose, 0.0f);
        pose.resolve();
        check_vec(pose.global[2].origin, Vec3(0, 2, 0), 1e-4f,
                  "and a bone no track names is left at its rest");
    }

    // ------------------------------------------- masks and one-shots
    {
        Ref<Skeleton> s = arm();
        Ref<AnimationClip> c(new AnimationClip());
        c->name = "both";
        for (const char *n : {"shoulder", "elbow"}) {
            AnimationClip::BoneTrack tr;
            tr.bone_name = n;
            tr.rotation.times = {0.0f};
            tr.rotation.values = {
                Quat::from_axis_angle(Vec3(0, 0, 1), deg2rad(90.0f))};
            c->tracks.push_back(tr);
        }
        c->compute_duration();
        c->retarget(*s);

        Pose pose;
        pose.bind(s.get());
        // Only the elbow may move. The shoulder's track must be
        // ignored entirely rather than applied at zero weight.
        std::vector<bool> mask = {false, true, true};
        c->sample_masked(pose, 0.0f, 1.0f, mask);
        pose.resolve();
        check_vec(pose.global[1].origin, Vec3(0, 1, 0), 1e-4f,
                  "a masked-out bone does not move");
        check_vec(pose.global[2].origin, Vec3(-1, 1, 0), 1e-4f,
                  "and one inside the mask does");
    }

    // --------------------------------------- the whole chain, imported
    {
        std::error_code ec;
        const std::filesystem::path dir =
                std::filesystem::temp_directory_path() / "warren_anim_test";
        std::filesystem::create_directories(dir, ec);
        ResourceLoader::set_base_directory(dir.string());
        const std::vector<uint8_t> glb = build_skinned_glb();
        write_file((dir / "arm.glb").string(), glb.data(), glb.size());

        Ref<Resource> res = ResourceLoader::load("arm.glb");
        PackedScene *ps = res ? res->cast_to<PackedScene>() : nullptr;
        check(ps != nullptr, "a skinned glb loads");
        Node *root = ps ? ps->instantiate() : nullptr;
        check(root != nullptr, "and instantiates");
        if (!root) {
            std::printf("  %d checks\n%s\n", g_checks, "FAILED");
            return 1;
        }

        Node *found = root->find_by_class("Skinned3D");
        Skinned3D *body = found ? found->cast_to<Skinned3D>() : nullptr;
        check(body != nullptr, "a mesh with a skin becomes a Skinned3D");
        check(root->find_by_class("AnimationPlayer") != nullptr,
              "with a player carrying the file's clips");

        if (body) {
            const bool two = body->skeleton() && body->skeleton()->count() == 2;
            check(two, "the skin's two joints become two bones");
            if (two) {
                check(body->skeleton()->bones[1].parent == 0,
                      "related by the node tree, which is where glTF keeps it");
                check(body->skeleton()->bones[1].name == "Lower",
                      "and named after the nodes");
                // THE INVERSE BINDS CAME FROM THE FILE, not from the
                // rests. Both agree here, which is the point: if the
                // accessor were being ignored the test would still
                // pass, so the value is checked rather than where it
                // came from.
                check_vec(
                        body->skeleton()->bones[1].inverse_bind.xform(Vec3(0, 1, 0)),
                        Vec3(0, 0, 0), 1e-5f,
                        "the inverse bind matrices are read from the accessor");
            }

            Mesh *m = body->mesh.get();
            check(m && m->skinned(), "the mesh has a skin stream as long as it is");
            if (m && m->skinned()) {
                check(m->skin[0].joints[0] == 0 && m->skin[1].joints[0] == 1,
                      "each vertex is bound to the joint the file says");
                check(m->skin[0].weights[0] == 255,
                      "with its weight normalised to full");
            }

            // AND NOW THE WHOLE THING. Bone 1 turned ninety degrees
            // about +Z; the vertex bound to it sits at (0,1,0), which
            // is the bone's own origin, so it must not move at all --
            // and the one bound to bone 0 must not move either.
            // Turning the bone and finding the tip somewhere else is
            // the test that every link was right.
            Pose &pose = body->pose();
            pose.reset();
            pose.local[1].basis =
                    Basis(Quat::from_axis_angle(Vec3(0, 0, 1), deg2rad(90.0f)));
            pose.resolve();
            if (m && m->skinned()) {
                check_vec(skin_point(pose, m->skin[1], Vec3(0, 1, 0)),
                          Vec3(0, 1, 0), 1e-4f,
                          "a vertex at its own bone's pivot stays put when it turns");
                // A point a metre beyond the pivot, bound to the same
                // bone, swings a quarter turn: +Y becomes -X.
                check_vec(skin_point(pose, m->skin[1], Vec3(0, 2, 0)),
                          Vec3(-1, 1, 0), 1e-4f,
                          "and one beyond it swings the quarter turn with it");
                check_vec(skin_point(pose, m->skin[0], Vec3(0, 0, 0)),
                          Vec3(0, 0, 0), 1e-4f,
                          "while a vertex on the other bone does not move");
            }

            // The clip came through with its keys.
            Node *pn = root->find_by_class("AnimationPlayer");
            AnimationPlayer *player = pn ? pn->cast_to<AnimationPlayer>() : nullptr;
            if (player) {
                check(player->has("Bend"), "the animation is there by name");
                Ref<AnimationClip> c = player->clip("Bend");
                check(c && std::fabs(c->duration - 1.0f) < 1e-4f,
                      "one second long, as the sampler's times say");
                check(c && c->tracks.size() == 1,
                      "with one track, for the one channel");
            }
        }
        root->queue_free();
        delete root;
        ResourceLoader::forget_all();
        std::filesystem::remove_all(dir, ec);
        ResourceLoader::set_base_directory(".");
    }

    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
