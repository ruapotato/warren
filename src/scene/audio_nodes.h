// Warren -- sound in the world.
//
// THE POINT OF THIS FILE IS ONE FUNCTION: how far away a sound is,
// and which direction it comes from, when the shortest way from it
// to your ear goes through a hole in a wall.
//
// Every engine can attenuate by distance. In a world with portals the
// straight-line distance is the wrong number and the straight-line
// direction is the wrong direction: a generator humming in the far
// room is forty metres away through the rock and two metres away
// through the arch, and it should be heard at two metres, coming from
// the arch. Getting that wrong does not sound like a bug -- it sounds
// like the portal is a picture rather than an opening, which is the
// one impression this engine exists to avoid.
#pragma once

#include "audio/audio.h"
#include "scene/node.h"
#include "scene/nodes.h"

namespace wr {

class PhysicsWorld;
class Portal3D;

// Where the listener is. Falls back to the active camera, which is
// what a first-person game wants and saves it a node.
class AudioListener3D : public Node3D {
    WR_CLASS(AudioListener3D, Node3D)

public:
    void make_current();
    bool is_current() const;
    // For the same reason Camera3D does it: the listener is held as
    // a raw pointer and a scene change frees it.
    void on_exit_tree() override;
};

// A sound with a position.
class AudioPlayer3D : public Node3D {
    WR_CLASS(AudioPlayer3D, Node3D)

public:
    Ref<AudioClip> clip;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool autoplay = false;
    // Inverse-square out to `max_distance`, then nothing. The falloff
    // is windowed the same way a punctual light's is, so a sound can
    // be culled at a finite range without a step where it stops.
    float max_distance = 40.0f;
    // Below this the sound is at full volume; inside a source it does
    // not become infinitely loud.
    float reference_distance = 1.0f;
    // 0 is no directionality at all, 1 is hard left/right. A little
    // less than full makes a sound passing behind the listener move
    // smoothly rather than snapping across.
    float panning = 0.85f;
    // Metres per second, for doppler. 0 turns it off.
    float doppler = 0.0f;
    // HOW MANY PORTALS THE SOUND MAY COME THROUGH. One is the case
    // that matters and costs a loop over the portals; two is a room
    // through a room and costs that squared.
    int portal_hops = 1;
    // What a portal does to a sound passing through it. Nothing
    // physical says this should be less than one, but a portal that
    // muffles very slightly reads as an opening rather than as a
    // hole in the logic.
    float portal_transmission = 0.9f;

    void play();
    void stop();
    bool playing() const;
    float playback_position() const;

    // Worked out every frame from where the listener is; exposed
    // because a game may want to know how loud something ended up,
    // and because the test reads it.
    float effective_distance() const { return effective_distance_; }
    Vec3 effective_direction() const { return effective_direction_; }
    int portals_used() const { return portals_used_; }

    void on_ready() override;
    void on_exit_tree() override;
    // Called by AudioSystem, not by the tree: it needs the listener.
    void update_voice(const Transform3D &listener, const Vec3 &listener_vel,
                      const std::vector<Portal3D *> &portals);

private:
    VoiceId voice_ = kInvalidVoice;
    float effective_distance_ = 0.0f;
    Vec3 effective_direction_{0, 0, -1};
    int portals_used_ = 0;
    Vec3 last_position_{0, 0, 0};
    bool has_last_ = false;
};

// A sound with no position: music, a menu click, a voice-over.
class AudioPlayer : public Node {
    WR_CLASS(AudioPlayer, Node)

public:
    Ref<AudioClip> clip;
    float volume = 1.0f;
    float pitch = 1.0f;
    bool loop = false;
    bool autoplay = false;

    void play();
    void stop();
    bool playing() const;

    void on_ready() override;
    void on_exit_tree() override;
    void refresh();

private:
    VoiceId voice_ = kInvalidVoice;
};

// Walks the tree once a frame, finds the listener, and updates every
// player. Owned by the Engine.
class AudioSystem {
public:
    void set_server(AudioServer *s) { server_ = s; }
    AudioServer *server() const { return server_; }
    void update(SceneTree *tree, float dt);
    // The listener's transform this frame, for anything that wants it.
    const Transform3D &listener() const { return listener_; }
    int players_heard() const { return heard_; }

private:
    AudioServer *server_ = nullptr;
    Transform3D listener_;
    Vec3 listener_velocity_{0, 0, 0};
    Vec3 last_listener_pos_{0, 0, 0};
    bool has_last_ = false;
    int heard_ = 0;
};

// THE FUNCTION THIS FILE EXISTS FOR.
//
// The shortest acoustic path from `source` to `listener`: straight
// through the air, or through one of the portals if that is shorter.
// Returns the path length, writes the unit direction the sound
// arrives from (pointing from the listener towards where it seems to
// come from) and how many portals it went through.
//
// Exposed rather than tucked away because it is the interesting part
// and because a test should be able to ask it directly.
float acoustic_path(const Vec3 &listener, const Vec3 &source,
                    const std::vector<Portal3D *> &portals, int max_hops,
                    Vec3 *out_direction, int *out_hops);

// Make this the system the player nodes talk to. The Engine calls it
// at start-up so that a node playing a sound in on_ready finds a
// server rather than silence.
void audio_system_install(AudioSystem *s);

// Every active, linked portal in the tree. The audio system needs the
// same list the renderer builds and has no renderer to ask.
void collect_portals(Node *root, std::vector<Portal3D *> *out);

}  // namespace wr
