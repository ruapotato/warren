#include "scene/animated.h"

#include <algorithm>

#include "core/bind.h"
#include "core/log.h"

namespace wr {

// ------------------------------------------------------------- Skinned3D

Skinned3D::Skinned3D() { set_processing(true); }

void Skinned3D::set_skeleton(const Ref<Skeleton> &s) {
    skeleton_ = s;
    pose_.bind(s.get());
    pose_.resolve();
    dirty_ = true;
}

int Skinned3D::find_bone(const std::string &name) const {
    return skeleton_ ? skeleton_->find(name) : -1;
}

Transform3D Skinned3D::bone_global(int bone) const {
    if (!pose_.valid() || bone < 0 || bone >= pose_.count())
        return global_transform();
    return global_transform() * pose_.global[size_t(bone)];
}

Transform3D Skinned3D::bone_global(const std::string &name) const {
    return bone_global(find_bone(name));
}

void Skinned3D::bend(int bone, const Basis &turn, float amount) {
    if (!pose_.valid() || bone < 0 || bone >= pose_.count()) return;
    // IN THE SKELETON'S FRAME, WHICH IS THE BODY'S.
    //
    // The turn is given in the space the body faces, so "swing the arm
    // forward" means the same thing whichever way the body is pointing
    // -- which is the whole reason a pose is written by hand rather
    // than recorded. Applying it to the bone's LOCAL rotation instead
    // would mean the caller had to know how each bone's rest happens
    // to be oriented, and that is a different answer per bone and per
    // rig.
    //
    // The parent's global is needed to bring the result back into the
    // local frame, so this only reads correctly if the parent has
    // already been resolved -- which it has, because a pose resolves
    // in index order and a parent's index is lower.
    const int parent = skeleton_->bones[size_t(bone)].parent;
    const Basis cur = pose_.global[size_t(bone)].basis;
    Basis want = turn * cur;
    if (parent >= 0) want = pose_.global[size_t(parent)].basis.inverse() * want;
    Quat q = want.to_quat().normalized();
    if (amount < 0.999f)
        q = slerp(pose_.local[size_t(bone)].basis.to_quat(), q,
                  std::clamp(amount, 0.0f, 1.0f));
    pose_.local[size_t(bone)].basis =
            Basis(q) * Basis::scaled(pose_.local[size_t(bone)].basis.scale());
    // The children of this bone are now stale; one resolve at the end
    // of the frame fixes all of them at once.
    pose_.resolve();
    dirty_ = true;
}

void Skinned3D::on_process(float) {
    if (!pose_.valid()) return;
    pose_.resolve();
    dirty_ = true;
}

// ------------------------------------------------------ BoneAttachment3D

namespace {
// The nearest Skinned3D at or under a node, breadth first.
Skinned3D *skinned_under(Node *n) {
    if (!n) return nullptr;
    if (Skinned3D *s = dynamic_cast<Skinned3D *>(n)) return s;
    for (const Ref<Node> &c : n->children())
        if (Skinned3D *s = skinned_under(c.get())) return s;
    return nullptr;
}
}  // namespace

Skinned3D *BoneAttachment3D::find_body() {
    // Upward first: the usual arrangement is a child of the node the
    // importer made, with the skinned mesh a sibling or an uncle.
    for (Node *p = parent(); p; p = p->parent())
        if (Skinned3D *s = skinned_under(p)) return s;
    return nullptr;
}

void BoneAttachment3D::on_ready() {
    if (!target_) target_ = find_body();
    index_ = (target_ && !bone.empty()) ? target_->find_bone(bone) : -1;
    if (target_ && index_ < 0 && !bone.empty())
        WR_WARN("%s: no bone '%s' on %s", name().c_str(), bone.c_str(),
                target_->name().c_str());
}

bool BoneAttachment3D::discover() {
    if (!target_) target_ = find_body();
    if (!target_) return false;
    if (index_ < 0 && !bone.empty()) index_ = target_->find_bone(bone);
    if (index_ < 0) return false;
    // Whatever makes the node's CURRENT place true: bone * offset =
    // here, so offset = bone^-1 * here.
    offset = target_->bone_global(index_).inverse() * global_transform();
    return true;
}

void BoneAttachment3D::on_process(float) {
    if (!target_) {
        target_ = find_body();
        index_ = (target_ && !bone.empty()) ? target_->find_bone(bone) : -1;
    }
    if (!target_ || index_ < 0) return;
    // AFTER THE BODY HAS RESOLVED, or one frame behind it. Both the
    // pose and this run in on_process, and the tree walks parents
    // before children -- so an attachment under the figure sees the
    // pose from this frame. One that is not is a frame late, which
    // at sixty frames a second is sixteen milliseconds of a gun
    // lagging a wrist and nobody has ever seen it.
    set_global_transform(target_->bone_global(index_) * offset);
}

// -------------------------------------------------------- AnimationPlayer

void AnimationPlayer::add_clip(const Ref<AnimationClip> &clip) {
    if (!clip) return;
    clips_.push_back(clip);
}

Array AnimationPlayer::get_clips() const {
    Array a;
    a.reserve(clips_.size());
    for (const Ref<AnimationClip> &c : clips_) a.push_back(Variant(c.get()));
    return a;
}

void AnimationPlayer::set_clips(const Array &a) {
    clips_.clear();
    clips_.reserve(a.size());
    for (const Variant &v : a) {
        Object *o = v.to_object();
        if (o)
            clips_.push_back(Ref<AnimationClip>(o->cast_to<AnimationClip>()));
    }
}

Ref<AnimationClip> AnimationPlayer::clip(const std::string &name) const {
    for (const Ref<AnimationClip> &c : clips_)
        if (c && c->name == name) return c;
    return {};
}

std::vector<std::string> AnimationPlayer::clip_names() const {
    std::vector<std::string> out;
    for (const Ref<AnimationClip> &c : clips_)
        if (c) out.push_back(c->name);
    return out;
}

Skinned3D *AnimationPlayer::target() const {
    if (cached_ && cached_->is_inside_tree()) return cached_;
    cached_ = nullptr;
    // The body this player drives is its parent, or its parent's
    // nearest Skinned3D child -- so a player can hang off the
    // character node rather than off the mesh.
    for (Node *p = parent(); p; p = p->parent()) {
        if (Skinned3D *s = p->cast_to<Skinned3D>()) { cached_ = s; break; }
        if (Node *found = p->find_by_class("Skinned3D")) {
            cached_ = found->cast_to<Skinned3D>();
            break;
        }
    }
    return cached_;
}

void AnimationPlayer::on_ready() {
    set_processing(true);
    // Resolve every clip against the skeleton it will be played on.
    // A clip that matches nothing is a clip recorded on another rig,
    // and playing it silently is worse than saying so.
    Skinned3D *s = target();
    if (!s || !s->skeleton()) return;
    for (const Ref<AnimationClip> &c : clips_) {
        if (!c) continue;
        const int hit = c->retarget(*s->skeleton());
        if (hit == 0)
            WR_WARN("animation: '%s' names no bone of skeleton '%s'",
                    c->name.c_str(), s->skeleton()->resource_name().c_str());
    }
}

void AnimationPlayer::play(const std::string &name, float fade, float speed) {
    // ASKING FOR WHAT IS ALREADY PLAYING CHANGES ONLY THE SPEED.
    //
    // Including while a cross-fade INTO it is still running, which
    // is the case this used to miss: `a_ == name` was checked but
    // `b_ == name` was not, so a caller that re-issued the same
    // clip during the fade -- which a gait does, every frame, as
    // the speed it is scaled by drifts -- restarted the fade
    // target at phase zero each time. The result is the first
    // fifth of a second of a run cycle played over and over for
    // as long as the player is still accelerating, which reads as
    // a stutter and was reported as the run animation playing its
    // beginning twice.
    if ((a_ == name && fade_left_ <= 0.0f) || b_ == name) {
        speed_ = speed;
        return;
    }
    if (!has(name)) return;
    speed_ = speed;
    if (a_.empty() || fade <= 0.0f) {
        a_ = name;
        phase_a_ = 0.0f;
        b_.clear();
        mix_ = 0.0f;
        fade_left_ = 0.0f;
        return;
    }
    b_ = name;
    phase_b_ = 0.0f;
    mix_ = 0.0f;
    fade_len_ = fade;
    fade_left_ = fade;
}

void AnimationPlayer::blend(const std::string &a, const std::string &b,
                            float mix, float speed) {
    // A HAND-DRIVEN BLEND CANCELS A CROSS-FADE. They both own the same
    // two slots, and letting a fade keep running underneath means the
    // mix the caller asked for is overwritten a frame later.
    fade_left_ = 0.0f;
    if (a_ != a) { a_ = a; phase_a_ = 0.0f; }
    if (b_ != b) { b_ = b; phase_b_ = phase_a_; }
    mix_ = std::clamp(mix, 0.0f, 1.0f);
    speed_ = speed;
}

std::vector<bool> AnimationPlayer::mask_for(
        const std::vector<std::string> &names) const {
    Skinned3D *s = target();
    if (!s || !s->skeleton() || names.empty()) return {};
    const Skeleton &sk = *s->skeleton();
    std::vector<bool> mask(size_t(sk.count()), false);
    // A named bone brings its descendants with it: "spine" means the
    // upper body, not one vertebra. Parents come first, so one forward
    // pass marks a whole subtree.
    for (const std::string &n : names) {
        const int b = sk.find(n);
        if (b >= 0) mask[size_t(b)] = true;
    }
    for (int i = 0; i < sk.count(); i++) {
        const int p = sk.bones[size_t(i)].parent;
        if (p >= 0 && mask[size_t(p)]) mask[size_t(i)] = true;
    }
    return mask;
}

void AnimationPlayer::one_shot(const std::string &name,
                               const std::vector<std::string> &mask,
                               float speed, float fade) {
    if (!has(name)) return;
    shot_ = name;
    shot_t_ = 0.0f;
    shot_speed_ = speed;
    shot_fade_ = fade;
    shot_mask_ = mask_for(mask);
}

void AnimationPlayer::one_shot_from(const std::string &name,
                                    const std::string &from, float speed,
                                    float fade) {
    if (from.empty())
        one_shot(name, {}, speed, fade);
    else
        one_shot(name, {from}, speed, fade);
}

void AnimationPlayer::on_process(float dt) {
    Skinned3D *s = target();
    if (!s || !s->pose().valid()) return;
    Pose &pose = s->pose();

    Ref<AnimationClip> ca = clip(a_);
    Ref<AnimationClip> cb = clip(b_);
    phase_a_ += dt * speed_;
    phase_b_ += dt * speed_;

    if (fade_left_ > 0.0f) {
        fade_left_ = std::max(0.0f, fade_left_ - dt);
        mix_ = fade_len_ > 0.0f ? 1.0f - fade_left_ / fade_len_ : 1.0f;
        if (fade_left_ <= 0.0f) {
            a_ = b_;
            phase_a_ = phase_b_;
            b_.clear();
            mix_ = 0.0f;
            ca = clip(a_);
            cb = {};
        }
    }

    // FROM THE REST EVERY FRAME. A pose is not accumulated: a clip that
    // stops touching a bone must let it go back where it belongs, and
    // a pose that is only ever written over keeps the last thing any
    // clip did to that bone for ever.
    pose.reset();
    if (ca) ca->sample(pose, phase_a_, 1.0f);
    if (cb && mix_ > 0.001f) cb->sample(pose, phase_b_, mix_);

    if (!shot_.empty()) {
        Ref<AnimationClip> cs = clip(shot_);
        if (!cs || cs->duration <= 0.0f) {
            shot_.clear();
        } else {
            shot_t_ += dt * shot_speed_;
            const float p = shot_t_ / cs->duration;
            // Eased in and out, so a one-shot does not pop on or off
            // the body it is laid over.
            float w = 1.0f;
            const float f = shot_fade_ / std::max(0.05f, cs->duration);
            if (p < f) w = p / f;
            else if (p > 1.0f - f) w = (1.0f - p) / f;
            if (p >= 1.0f) {
                shot_.clear();
            } else {
                cs->sample_masked(pose, shot_t_, std::clamp(w, 0.0f, 1.0f),
                                  shot_mask_);
            }
        }
    }

    pose.resolve();
    s->mark_dirty();
}

// ------------------------------------------------------------ reflection

static void register_animated_classes() {
    ClassBuilder<BoneAttachment3D>()
        .field("bone", &BoneAttachment3D::bone)
        .field("offset", &BoneAttachment3D::offset)
        .method("discover", &BoneAttachment3D::discover)
        .method("set_target", &BoneAttachment3D::set_target).args("body")
        .method("get_target", &BoneAttachment3D::target);

    ClassBuilder<Skinned3D>()
        .prop("skeleton", &Skinned3D::get_skeleton, &Skinned3D::set_skeleton_ptr)
        .method("find_bone", &Skinned3D::find_bone).args("name")
        .method("bone_global",
                static_cast<Transform3D (Skinned3D::*)(const std::string &) const>(
                        &Skinned3D::bone_global))
                .args("name");
    ClassBuilder<AnimationPlayer>()
        .method("play", &AnimationPlayer::play).args("name", "fade", "speed")
        .method("blend", &AnimationPlayer::blend).args("a", "b", "mix", "speed")
        .method("one_shot", &AnimationPlayer::one_shot_from)
                .args("name", "bone", "speed", "fade")
        .method("stop_one_shot", &AnimationPlayer::stop_one_shot)
        .method("acting", &AnimationPlayer::acting)
        .method("has", &AnimationPlayer::has).args("name")
        .method("playing", &AnimationPlayer::playing)
        .method("fading_to", &AnimationPlayer::fading_to)
        .method("fade_phase", &AnimationPlayer::fade_phase)
        .method("fade_left", &AnimationPlayer::fade_left)
        .prop("phase", &AnimationPlayer::phase, &AnimationPlayer::set_phase)
        .prop("clips", &AnimationPlayer::get_clips, &AnimationPlayer::set_clips);
}

WR_REGISTER(register_animated_classes)

}  // namespace wr
