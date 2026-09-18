#include "anim/skeleton.h"

#include <algorithm>

#include "core/bind.h"
#include "core/log.h"

namespace wr {

int Skeleton::find(const std::string &name) const {
    for (size_t i = 0; i < bones.size(); i++)
        if (bones[i].name == name) return int(i);
    return -1;
}

int Skeleton::add(const std::string &name, int parent, const Transform3D &rest) {
    if (parent >= int(bones.size())) {
        WR_ERROR("skeleton: bone '%s' names parent %d, which does not exist yet; "
                 "parents must be added first",
                 name.c_str(), parent);
        return -1;
    }
    Bone b;
    b.name = name;
    b.parent = parent;
    b.rest = rest;
    bones.push_back(b);
    return int(bones.size()) - 1;
}

Transform3D Skeleton::global_rest(int bone) const {
    if (bone < 0 || bone >= int(bones.size())) return Transform3D();
    Transform3D t = bones[size_t(bone)].rest;
    for (int p = bones[size_t(bone)].parent; p >= 0; p = bones[size_t(p)].parent)
        t = bones[size_t(p)].rest * t;
    return t;
}

void Skeleton::compute_inverse_binds() {
    for (size_t i = 0; i < bones.size(); i++)
        bones[i].inverse_bind = global_rest(int(i)).inverse();
}

bool Skeleton::ordered() const {
    for (size_t i = 0; i < bones.size(); i++)
        if (bones[i].parent >= int(i)) return false;
    return true;
}

std::vector<int> Skeleton::sort_hierarchically() {
    const size_t n = bones.size();
    std::vector<int> remap(n, -1);
    if (ordered()) {
        for (size_t i = 0; i < n; i++) remap[i] = int(i);
        return remap;
    }
    // Roots first, then anything whose parent is already placed. A
    // cycle -- which a malformed file can contain -- would loop here
    // for ever, so a pass that places nothing gives up and takes the
    // rest in their original order rather than hanging.
    std::vector<Bone> out;
    out.reserve(n);
    std::vector<bool> placed(n, false);
    bool progress = true;
    while (out.size() < n && progress) {
        progress = false;
        for (size_t i = 0; i < n; i++) {
            if (placed[i]) continue;
            const int p = bones[i].parent;
            if (p >= 0 && !placed[size_t(p)]) continue;
            remap[i] = int(out.size());
            out.push_back(bones[i]);
            placed[i] = true;
            progress = true;
        }
    }
    if (out.size() < n) {
        WR_WARN("skeleton: %zu bones form a cycle; taking them as they came",
                n - out.size());
        for (size_t i = 0; i < n; i++) {
            if (placed[i]) continue;
            remap[i] = int(out.size());
            out.push_back(bones[i]);
        }
    }
    for (Bone &b : out)
        if (b.parent >= 0) b.parent = remap[size_t(b.parent)];
    bones.swap(out);
    return remap;
}

// ------------------------------------------------------------------ Pose

void Pose::bind(const Skeleton *s) {
    skeleton = s;
    const size_t n = s ? s->bones.size() : 0;
    local.assign(n, Transform3D());
    global.assign(n, Transform3D());
    skin.assign(n, Transform3D());
    reset();
}

void Pose::reset() {
    if (!skeleton) return;
    for (size_t i = 0; i < local.size(); i++) local[i] = skeleton->bones[i].rest;
}

void Pose::resolve() {
    if (!skeleton) return;
    const size_t n = std::min(local.size(), skeleton->bones.size());
    for (size_t i = 0; i < n; i++) {
        const int p = skeleton->bones[i].parent;
        // ONE FORWARD PASS, and it is only correct because a parent's
        // index is always lower than its child's. See the note in the
        // header; `ordered()` is what keeps that true.
        global[i] = p >= 0 ? global[size_t(p)] * local[i] : local[i];
        skin[i] = global[i] * skeleton->bones[i].inverse_bind;
    }
}

static void register_skeleton_class() {
    ClassBuilder<Skeleton>()
        .method("count", &Skeleton::count)
        .method("find", &Skeleton::find).args("name")
        .method("compute_inverse_binds", &Skeleton::compute_inverse_binds)
        .method("global_rest", &Skeleton::global_rest).args("bone");
}
WR_REGISTER(register_skeleton_class)

}  // namespace wr
