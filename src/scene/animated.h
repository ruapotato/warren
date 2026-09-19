// Warren -- a mesh with bones in it, and something to drive them.
//
// TWO NODES, NOT ONE.
//
// `Skinned3D` is a MeshInstance3D that also owns a Pose and a
// Skeleton: it is the thing the renderer skins. `AnimationPlayer` is a
// child that decides what that pose should be this frame. They are
// separate because half the bodies in a game are driven by a clip and
// the other half by code -- a ragdoll, an IK chain, a procedural
// shamble -- and the ones driven by code should not have to carry a
// player they never use.
#pragma once

#include <string>
#include <vector>

#include "anim/clip.h"
#include "anim/skeleton.h"
#include "scene/nodes.h"

namespace wr {

class Skinned3D : public MeshInstance3D {
    WR_CLASS(Skinned3D, MeshInstance3D)

public:
    Skinned3D();

    void set_skeleton(const Ref<Skeleton> &s);
    const Ref<Skeleton> &skeleton() const { return skeleton_; }
    // Raw-pointer accessors for the reflection, which speaks Object*
    // and not Ref<T> -- same as MeshInstance3D::get_mesh.
    void set_skeleton_ptr(Skeleton *s) { set_skeleton(Ref<Skeleton>(s)); }
    Skeleton *get_skeleton() const { return skeleton_.get(); }
    Pose &pose() { return pose_; }
    const Pose &pose() const { return pose_; }

    // Where a bone is in the world this instant. What a muzzle, a
    // lantern, a headshot test and a rope anchor all need.
    Transform3D bone_global(int bone) const;
    Transform3D bone_global(const std::string &name) const;
    int find_bone(const std::string &name) const;

    // Turn a bone about a world axis, on top of whatever posed it.
    // The one operation every hand-written pose in a game is made of;
    // see ROTGRAVE's undead arms.
    void bend(int bone, const Basis &turn, float amount = 1.0f);

    // Resolved once per frame, after everything has had its say.
    void on_process(float dt) override;
    // Whether the bones moved since the last resolve. The renderer
    // uploads only when they did.
    bool pose_dirty() const { return dirty_; }
    void mark_dirty() { dirty_ = true; }
    void clear_dirty() { dirty_ = false; }

private:
    Ref<Skeleton> skeleton_;
    Pose pose_;
    bool dirty_ = true;
};

// SOMETHING HUNG OFF A BONE.
//
// A rig's bones are Node3Ds in the tree, and the obvious thing to do
// with a prop is to parent it to one. That does not work and the
// reason is not obvious: the AnimationPlayer poses the SKELETON --
// the palette the skinning shader reads -- and never touches those
// nodes, so a mesh parented to `hand_r` sits wherever the rest pose
// put it and never moves again. The body animates, the thing in its
// hand does not, and nothing says so.
//
// Which is a hole in the engine rather than a detail of one game: a
// gun, a torch, a hat, a camera on a head, a socket for anything at
// all -- every one of them is this. So it is a node, and it does
// what the name says: each frame it puts itself where the named bone
// is, times whatever offset it was given.
//
// THE OFFSET IS IN THE BONE'S OWN FRAME, which is the frame a rig
// author thinks in ("a hand's width along the palm") and not the
// frame the world thinks in. `discover` fills it in from where the
// node currently IS, which is how a game tunes one: put the prop
// where it looks right, call discover once, and the number is the
// answer rather than a guess.
class BoneAttachment3D : public Node3D {
    WR_CLASS(BoneAttachment3D, Node3D)

public:
    // Which bone, by name. Empty follows nothing.
    std::string bone;
    // Where, in the bone's frame.
    Transform3D offset = Transform3D::identity();
    // The body to follow. Found by searching upward and then the
    // whole tree from the parent, so the ordinary case -- a child of
    // the node the glTF produced -- needs no wiring at all.
    void set_target(Skinned3D *s) { target_ = s; }
    Skinned3D *target() const { return target_; }

    // TAKE THE OFFSET FROM WHERE IT IS NOW. Place the node where it
    // should sit, call this, and the offset is whatever makes that
    // true. Returns false if there is no body or no such bone.
    bool discover();

    void on_ready() override;
    void on_process(float dt) override;

private:
    Skinned3D *find_body();
    Skinned3D *target_ = nullptr;
    int index_ = -1;
};

// WHAT IS PLAYING, AND WHAT IT IS FADING INTO.
//
// Two slots, not a graph. A blend tree is the right answer for a game
// with fifty states and the wrong one for a game with a walk, a run
// and a one-shot over the top: this plays a clip, cross-fades to
// another, and lays a masked one-shot on top, which is every
// locomotion problem ROTGRAVE has and nothing it does not.
class AnimationPlayer : public Node {
    WR_CLASS(AnimationPlayer, Node)

public:
    void add_clip(const Ref<AnimationClip> &clip);
    // The clip list as an Array, so a saved scene keeps it.
    Array get_clips() const;
    void set_clips(const Array &a);
    Ref<AnimationClip> clip(const std::string &name) const;
    std::vector<std::string> clip_names() const;
    bool has(const std::string &name) const { return clip(name) != nullptr; }

    // Start a clip, cross-fading out of whatever is playing.
    void play(const std::string &name, float fade = 0.15f, float speed = 1.0f);
    // Blend two clips by hand -- a walk into a run, by ground speed.
    void blend(const std::string &a, const std::string &b, float mix,
               float speed = 1.0f);
    // A one-shot over the top of the locomotion, on some of the bones.
    // `mask` is bone names; empty means the whole body.
    void one_shot(const std::string &name, const std::vector<std::string> &mask,
                  float speed = 1.0f, float fade = 0.12f);
    // The same, named by ONE bone that brings its descendants -- the
    // shape a caller actually wants ("from the spine up", so a reload
    // plays over a walk). Empty `from` is the whole body. This is the
    // overload scripts get: a subtree root is a string, and a list of
    // them is not something the binding layer carries.
    void one_shot_from(const std::string &name, const std::string &from,
                       float speed = 1.0f, float fade = 0.12f);
    bool acting() const { return !shot_.empty(); }
    void stop_one_shot() { shot_.clear(); }

    const std::string &playing() const { return a_; }
    float phase() const { return phase_a_; }
    void set_phase(float p) { phase_a_ = p; }
    // WHAT IS COMING IN, while a cross-fade runs. Empty when
    // none is. A game driving a blend by hand needs to know, and
    // so does anything checking that asking twice for the same
    // clip does not restart it.
    const std::string &fading_to() const { return b_; }
    float fade_phase() const { return phase_b_; }
    // How much of the fade is left, in seconds.
    float fade_left() const { return fade_left_; }
    // FOR A TEST, and named so nobody mistakes it for anything
    // else: moves the incoming clip on without needing a
    // skeleton, a pose and a frame to do it through.
    void advance_fade_for_test(float dt) { phase_b_ += dt; }
    void set_speed(float s) { speed_ = s; }

    void on_ready() override;
    void on_process(float dt) override;

private:
    Skinned3D *target() const;
    std::vector<bool> mask_for(const std::vector<std::string> &names) const;

    std::vector<Ref<AnimationClip>> clips_;
    // The two locomotion slots and how far between them.
    std::string a_, b_;
    float phase_a_ = 0.0f, phase_b_ = 0.0f, mix_ = 0.0f;
    float speed_ = 1.0f;
    // A cross-fade `play` set up, which drives mix_ towards 1 and then
    // promotes b into a.
    float fade_left_ = 0.0f, fade_len_ = 0.0f;
    // The one-shot.
    std::string shot_;
    float shot_t_ = 0.0f, shot_speed_ = 1.0f, shot_fade_ = 0.12f;
    std::vector<bool> shot_mask_;
    mutable Skinned3D *cached_ = nullptr;
};

}  // namespace wr
