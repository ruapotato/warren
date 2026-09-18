#include "scene/audio_nodes.h"

#include <algorithm>
#include <cmath>

#include "core/bind.h"
#include "core/log.h"
#include "scene/portal.h"
#include "scene/scene_tree.h"

namespace mf {
namespace {
AudioListener3D *g_listener = nullptr;
AudioSystem *g_system = nullptr;
}  // namespace

AudioSystem *audio_system();

// --------------------------------------------------------- the path

float acoustic_path(const Vec3 &listener, const Vec3 &source,
                    const std::vector<Portal3D *> &portals, int max_hops,
                    Vec3 *out_direction, int *out_hops) {
    // The straight line is always a candidate and is usually the
    // answer; everything below is looking for a shorter one.
    Vec3 to_source = source - listener;
    float best = to_source.length();
    Vec3 best_dir = best > 1e-5f ? to_source * (1.0f / best) : Vec3(0, 0, -1);
    int best_hops = 0;

    if (max_hops > 0) {
        for (Portal3D *p : portals) {
            Portal3D *q = p ? p->link() : nullptr;
            if (!p || !q || !p->active || !q->active) continue;

            // THE SOUND ENTERS AT THE APERTURE AND LEAVES AT ITS
            // PARTNER. Which end is which is decided by the
            // listener: you hear through the hole you can see, so
            // the near aperture is the one nearer to you.
            //
            // The length is the distance from the ear to the near
            // aperture, plus the distance from the far aperture to
            // the source measured IN THE FAR ROOM -- which is what
            // makes this different from pretending the portal is a
            // speaker. A source just behind the far aperture is
            // close; one across the far room is not.
            const Vec3 near_point = p->closest_point(listener);
            const Vec3 far_point = q->closest_point(source);
            const float d = (near_point - listener).length() +
                            (far_point - source).length();
            if (d >= best) continue;

            // Only if the sound can actually get out of the far
            // aperture towards the source: a portal is one-sided,
            // and a source behind the far portal is not audible
            // through the front of it.
            if (!q->faces(source)) continue;

            best = d;
            const Vec3 to_hole = near_point - listener;
            const float n = to_hole.length();
            best_dir = n > 1e-5f ? to_hole * (1.0f / n) : p->normal();
            best_hops = 1;
        }
    }

    if (out_direction) *out_direction = best_dir;
    if (out_hops) *out_hops = best_hops;
    return best;
}

void collect_portals(Node *root, std::vector<Portal3D *> *out) {
    out->clear();
    if (!root) return;
    std::vector<Node *> stack{root};
    while (!stack.empty()) {
        Node *n = stack.back();
        stack.pop_back();
        for (const auto &c : n->children())
            if (c) stack.push_back(c.get());
        if (Portal3D *p = n->cast_to<Portal3D>())
            if (p->active && p->linked() && p->visible_in_tree())
                out->push_back(p);
    }
}

// ------------------------------------------------------- the listener

void AudioListener3D::make_current() { g_listener = this; }
bool AudioListener3D::is_current() const { return g_listener == this; }

// -------------------------------------------------------- the players

void AudioPlayer3D::on_ready() {
    if (autoplay) play();
}

void AudioPlayer3D::on_exit_tree() { stop(); }

void AudioPlayer3D::play() {
    AudioSystem *sys = audio_system();
    if (!sys || !sys->server() || !clip) return;
    stop();
    // Silent to begin with: the first update places it, and starting
    // at full volume would put one buffer of an un-panned, un-
    // attenuated sound in front of the correct one.
    voice_ = sys->server()->play(clip, 0.0f, pitch, loop);
}

void AudioPlayer3D::stop() {
    AudioSystem *sys = audio_system();
    if (sys && sys->server()) sys->server()->stop(voice_);
    voice_ = kInvalidVoice;
}

bool AudioPlayer3D::playing() const {
    AudioSystem *sys = audio_system();
    return sys && sys->server() && sys->server()->alive(voice_);
}

float AudioPlayer3D::playback_position() const {
    AudioSystem *sys = audio_system();
    return sys && sys->server() ? sys->server()->position(voice_) : 0.0f;
}

void AudioPlayer3D::update_voice(const Transform3D &listener,
                                 const Vec3 &listener_vel,
                                 const std::vector<Portal3D *> &portals) {
    AudioSystem *sys = audio_system();
    if (!sys || !sys->server()) return;
    AudioServer *server = sys->server();
    if (!server->alive(voice_)) {
        voice_ = kInvalidVoice;
        return;
    }

    const Vec3 pos = global_position();
    Vec3 dir;
    int hops = 0;
    const float distance =
        acoustic_path(listener.origin, pos, portals, portal_hops, &dir, &hops);
    effective_distance_ = distance;
    effective_direction_ = dir;
    portals_used_ = hops;

    VoiceState st;
    st.active = true;
    st.loop = loop;

    // Inverse distance with a reference, windowed to zero at the
    // maximum so the sound can be dropped without a click.
    const float ref = std::max(reference_distance, 1e-3f);
    const float maxd = std::max(max_distance, ref + 1e-3f);
    float gain = 0.0f;
    if (distance < maxd) {
        gain = ref / std::max(distance, ref);
        const float t = distance / maxd;
        const float window = std::max(0.0f, 1.0f - t * t * t * t);
        gain *= window;
    }
    gain *= volume;
    // Each portal takes a little off, which is what makes a sound
    // arriving through one read as arriving through something.
    for (int i = 0; i < hops; i++) gain *= portal_transmission;

    // Equal-power panning on the listener's right axis. The listener
    // looks down -Z, so `right` is the basis's x.
    Transform3D l = listener;
    l.basis = l.basis.orthonormalized();
    const float side = dot(dir, l.basis.x());
    const float pan = clampf(side * panning, -1.0f, 1.0f);
    const float angle = (pan * 0.5f + 0.5f) * (PI * 0.5f);
    st.gain_l = gain * std::cos(angle);
    st.gain_r = gain * std::sin(angle);

    // Doppler from the rate of change of the PATH length, not of the
    // straight-line distance -- so a source running towards a portal
    // rises in pitch for a listener on the other side of it, which
    // is both correct and the whole joke.
    st.pitch = pitch;
    if (doppler > 0.0f && has_last_) {
        const float previous =
            acoustic_path(listener.origin, last_position_, portals,
                          portal_hops, nullptr, nullptr);
        const float closing = (previous - distance) * 60.0f;  // per second
        const float c = 343.0f;
        st.pitch *= clampf(c / std::max(c - closing * doppler, 1.0f), 0.5f,
                           2.0f);
    }
    (void)listener_vel;
    last_position_ = pos;
    has_last_ = true;

    server->set_voice(voice_, st);
}

void AudioPlayer::on_ready() {
    if (autoplay) play();
}
void AudioPlayer::on_exit_tree() { stop(); }

void AudioPlayer::play() {
    AudioSystem *sys = audio_system();
    if (!sys || !sys->server() || !clip) return;
    stop();
    voice_ = sys->server()->play(clip, volume, pitch, loop);
}

void AudioPlayer::stop() {
    AudioSystem *sys = audio_system();
    if (sys && sys->server()) sys->server()->stop(voice_);
    voice_ = kInvalidVoice;
}

bool AudioPlayer::playing() const {
    AudioSystem *sys = audio_system();
    return sys && sys->server() && sys->server()->alive(voice_);
}

void AudioPlayer::refresh() {
    AudioSystem *sys = audio_system();
    if (!sys || !sys->server() || !sys->server()->alive(voice_)) return;
    VoiceState st;
    st.active = true;
    st.loop = loop;
    st.gain_l = st.gain_r = volume;
    st.pitch = pitch;
    sys->server()->set_voice(voice_, st);
}

// --------------------------------------------------------- the system

AudioSystem *audio_system() { return g_system; }

void AudioSystem::update(SceneTree *tree, float dt) {
    g_system = this;
    if (!server_ || !tree || !tree->root()) return;

    // The listener: an explicit one if the scene placed it, the
    // active camera otherwise.
    if (g_listener && g_listener->is_inside_tree()) {
        listener_ = g_listener->global_transform();
    } else if (Camera3D *c = tree->active_camera()) {
        listener_ = c->global_transform();
    }
    if (has_last_ && dt > 1e-5f)
        listener_velocity_ = (listener_.origin - last_listener_pos_) * (1.0f / dt);
    last_listener_pos_ = listener_.origin;
    has_last_ = true;

    std::vector<Portal3D *> portals;
    collect_portals(tree->root(), &portals);

    heard_ = 0;
    std::vector<Node *> stack{tree->root()};
    while (!stack.empty()) {
        Node *n = stack.back();
        stack.pop_back();
        for (const auto &c : n->children())
            if (c) stack.push_back(c.get());
        if (AudioPlayer3D *p = n->cast_to<AudioPlayer3D>()) {
            p->update_voice(listener_, listener_velocity_, portals);
            if (p->playing()) heard_++;
        } else if (AudioPlayer *p2 = n->cast_to<AudioPlayer>()) {
            p2->refresh();
            if (p2->playing()) heard_++;
        }
    }
}

void audio_system_install(AudioSystem *s) { g_system = s; }

// ------------------------------------------------------------ reflection

static void register_audio_nodes() {
    ClassBuilder<AudioListener3D>()
        .method("make_current", &AudioListener3D::make_current)
        .method("is_current", &AudioListener3D::is_current);

    ClassBuilder<AudioPlayer3D>()
        .field("volume", &AudioPlayer3D::volume, "range:0,4")
        .field("pitch", &AudioPlayer3D::pitch, "range:0.25,4")
        .field("loop", &AudioPlayer3D::loop)
        .field("autoplay", &AudioPlayer3D::autoplay)
        .field("max_distance", &AudioPlayer3D::max_distance, "range:1,500")
        .field("reference_distance", &AudioPlayer3D::reference_distance,
               "range:0.1,20")
        .field("panning", &AudioPlayer3D::panning, "range:0,1")
        .field("doppler", &AudioPlayer3D::doppler, "range:0,4")
        .field("portal_hops", &AudioPlayer3D::portal_hops, "range:0,3")
        .field("portal_transmission", &AudioPlayer3D::portal_transmission,
               "range:0,1")
        .method("play", &AudioPlayer3D::play)
        .method("stop", &AudioPlayer3D::stop)
        .method("is_playing", &AudioPlayer3D::playing)
        .method("playback_position", &AudioPlayer3D::playback_position)
        .method("effective_distance", &AudioPlayer3D::effective_distance)
        .method("effective_direction", &AudioPlayer3D::effective_direction)
        .method("portals_used", &AudioPlayer3D::portals_used);

    ClassBuilder<AudioPlayer>()
        .field("volume", &AudioPlayer::volume, "range:0,4")
        .field("pitch", &AudioPlayer::pitch, "range:0.25,4")
        .field("loop", &AudioPlayer::loop)
        .field("autoplay", &AudioPlayer::autoplay)
        .method("play", &AudioPlayer::play)
        .method("stop", &AudioPlayer::stop)
        .method("is_playing", &AudioPlayer::playing);
}
MF_REGISTER(register_audio_nodes)

}  // namespace mf
