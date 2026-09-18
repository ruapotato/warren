// Warren -- recorded motion, and how a body is put at one moment of it.
//
// A CLIP IS TRACKS, NOT FRAMES.
//
// A frame-per-tick format is simple and is the wrong shape: most bones
// in most clips do nothing, a walk cycle's hips hold four keys and its
// feet hold forty, and a fixed frame rate means resampling every clip
// that was not authored at it. A track per bone per channel, each with
// its own times, stores what was actually recorded and samples at any
// rate.
//
// SAMPLING WRITES INTO A POSE AND RETURNS. It does not own a pose, a
// time, or a playback state -- those belong to whoever is playing it,
// and there are twenty of those per clip. See AnimationPlayer.
#pragma once

#include <string>
#include <vector>

#include "core/math/transform.h"
#include "core/object.h"
#include "resource/resource.h"

namespace wr {

struct Pose;
class Skeleton;

class AnimationClip : public Resource {
    WR_CLASS(AnimationClip, Resource)

public:
    // Three channels, kept apart because they interpolate differently:
    // positions and scales are linear, rotations are spherical, and a
    // quaternion lerped as four floats is a rotation that speeds up in
    // the middle and comes out short.
    struct Vec3Track {
        std::vector<float> times;
        std::vector<Vec3> values;
    };
    struct QuatTrack {
        std::vector<float> times;
        std::vector<Quat> values;
    };

    struct BoneTrack {
        // Resolved against a skeleton on load; -1 for a bone the target
        // skeleton does not have, which is normal when a clip recorded
        // on a fuller rig is played on a simpler one.
        int bone = -1;
        std::string bone_name;
        Vec3Track position;
        QuatTrack rotation;
        Vec3Track scale;
    };

    std::string name;
    std::vector<BoneTrack> tracks;
    float duration = 0.0f;
    bool loops = true;

    // Point every track at a bone of `skel` by name. Returns how many
    // found one; a clip that resolves nothing is a clip recorded on a
    // different rig, and the caller should say so rather than play
    // silence.
    int retarget(const Skeleton &skel);

    // WRITE ONE MOMENT INTO A POSE.
    //
    // `weight` below 1 blends over whatever is already there, which is
    // what makes a walk turn into a run rather than snapping. Bones the
    // clip does not touch are left alone -- that is what lets a reload
    // play on the arms over a run playing on the legs.
    void sample(Pose &pose, float time, float weight = 1.0f) const;

    // The same, restricted to a set of bones. `mask` is indexed by bone
    // and may be shorter than the skeleton; missing entries count as
    // allowed.
    void sample_masked(Pose &pose, float time, float weight,
                       const std::vector<bool> &mask) const;

    // Seconds, wrapped or clamped according to `loops`.
    float wrap(float time) const;

    void compute_duration();
};

}  // namespace wr
