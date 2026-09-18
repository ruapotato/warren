#include "anim/clip.h"

#include <algorithm>
#include <cmath>

#include "anim/skeleton.h"
#include "core/bind.h"

namespace wr {
namespace {

// WHERE `t` FALLS IN A SORTED LIST OF TIMES.
//
// Binary search rather than a remembered cursor. A cursor is faster
// for playback that only ever moves forward and is wrong the moment
// anything seeks, scrubs, blends two clips at different phases or
// plays the same clip at two times for two bodies -- all of which
// this engine does. Twenty bodies times nineteen bones times three
// channels is a thousand searches a frame over lists of tens; it does
// not register.
int span_of(const std::vector<float> &times, float t, float *frac) {
    const size_t n = times.size();
    if (n == 0) { *frac = 0.0f; return -1; }
    if (n == 1 || t <= times[0]) { *frac = 0.0f; return 0; }
    if (t >= times[n - 1]) { *frac = 0.0f; return int(n) - 1; }
    const auto it = std::upper_bound(times.begin(), times.end(), t);
    const int hi = int(it - times.begin());
    const int lo = hi - 1;
    const float span = times[size_t(hi)] - times[size_t(lo)];
    *frac = span > 1e-9f ? (t - times[size_t(lo)]) / span : 0.0f;
    return lo;
}

Vec3 sample_vec(const AnimationClip::Vec3Track &tr, float t, bool *got) {
    float f = 0.0f;
    const int i = span_of(tr.times, t, &f);
    if (i < 0) { *got = false; return Vec3(); }
    *got = true;
    if (f <= 0.0f || i + 1 >= int(tr.values.size())) return tr.values[size_t(i)];
    return lerp(tr.values[size_t(i)], tr.values[size_t(i) + 1], f);
}

Quat sample_quat(const AnimationClip::QuatTrack &tr, float t, bool *got) {
    float f = 0.0f;
    const int i = span_of(tr.times, t, &f);
    if (i < 0) { *got = false; return Quat(); }
    *got = true;
    if (f <= 0.0f || i + 1 >= int(tr.values.size())) return tr.values[size_t(i)];
    return slerp(tr.values[size_t(i)], tr.values[size_t(i) + 1], f);
}

}  // namespace

float AnimationClip::wrap(float time) const {
    if (duration <= 0.0f) return 0.0f;
    if (!loops) return std::clamp(time, 0.0f, duration);
    float t = std::fmod(time, duration);
    if (t < 0.0f) t += duration;
    return t;
}

void AnimationClip::compute_duration() {
    duration = 0.0f;
    for (const BoneTrack &tr : tracks) {
        if (!tr.position.times.empty())
            duration = std::max(duration, tr.position.times.back());
        if (!tr.rotation.times.empty())
            duration = std::max(duration, tr.rotation.times.back());
        if (!tr.scale.times.empty())
            duration = std::max(duration, tr.scale.times.back());
    }
}

int AnimationClip::retarget(const Skeleton &skel) {
    int found = 0;
    for (BoneTrack &tr : tracks) {
        tr.bone = skel.find(tr.bone_name);
        if (tr.bone >= 0) found++;
    }
    return found;
}

void AnimationClip::sample(Pose &pose, float time, float weight) const {
    sample_masked(pose, time, weight, {});
}

void AnimationClip::sample_masked(Pose &pose, float time, float weight,
                                  const std::vector<bool> &mask) const {
    if (!pose.valid() || weight <= 0.0f) return;
    const float t = wrap(time);
    const float w = std::min(weight, 1.0f);
    for (const BoneTrack &tr : tracks) {
        const int b = tr.bone;
        if (b < 0 || b >= pose.count()) continue;
        if (b < int(mask.size()) && !mask[size_t(b)]) continue;

        Transform3D &out = pose.local[size_t(b)];
        // THE REST IS THE DEFAULT FOR A CHANNEL THE CLIP LEFT OUT.
        //
        // Not whatever happens to be in the pose. A clip that animates
        // rotation and not position, sampled onto a pose another clip
        // has already moved, would otherwise inherit that other clip's
        // translation and the two would fight over the hips.
        const Transform3D &rest = pose.skeleton->bones[size_t(b)].rest;
        bool got = false;
        Vec3 p = sample_vec(tr.position, t, &got);
        if (!got) p = rest.origin;
        Quat q = sample_quat(tr.rotation, t, &got);
        if (!got) q = rest.basis.to_quat();
        Vec3 s = sample_vec(tr.scale, t, &got);
        if (!got) s = rest.basis.scale();

        Basis basis = Basis(q) * Basis::scaled(s);
        if (w >= 0.999f) {
            out.basis = basis;
            out.origin = p;
        } else {
            // Blended as rotation and position rather than as sixteen
            // floats: a matrix lerped entry by entry shears on the way
            // between two rotations.
            out.origin = lerp(out.origin, p, w);
            out.basis = Basis(slerp(out.basis.to_quat(), q, w))
                        * Basis::scaled(lerp(out.basis.scale(), s, w));
        }
    }
}

static void register_clip_class() {
    ClassBuilder<AnimationClip>()
        .field("duration", &AnimationClip::duration)
        .field("loops", &AnimationClip::loops)
        .method("wrap", &AnimationClip::wrap).args("time");
}
WR_REGISTER(register_clip_class)

}  // namespace wr
