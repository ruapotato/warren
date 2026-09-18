// Manifold -- sound.
//
// PORTALS CARRY SOUND. That is the only reason this file is
// interesting. A mixer with distance attenuation and stereo panning
// is a solved problem and the solution is short; what is not solved
// anywhere else is what a footstep two rooms away should sound like
// when one of those rooms opens onto the other through a hole in the
// wall. It should come from the hole, at the distance the sound
// actually travelled to reach it -- which in this engine is a
// question the portal graph can answer, and answering it is what
// makes the aperture feel like an opening rather than a picture.
//
// Everything below is in service of that: the mixer is deliberately
// plain so the interesting part is legible.
//
// THREADING. The device calls back on its own thread and must never
// wait for the game. The main thread publishes a snapshot of every
// voice's gain, pitch and position under a mutex it holds for a few
// microseconds; the audio thread takes the same mutex, copies what it
// needs and then mixes without touching anything the game owns. No
// allocation and no logging happen on the audio thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/math/mathdefs.h"
#include "core/object.h"

namespace mf {

// A decoded sound. Interleaved float samples, which is what the mixer
// wants and what a WAV decodes to anyway; a minute of stereo at 48k
// is 23 MB, so anything longer should be streamed and is not yet.
class AudioClip : public Object {
    MF_CLASS(AudioClip, Object)

public:
    std::vector<float> samples;
    int channels = 1;
    int rate = 48000;

    size_t frames() const {
        return channels ? samples.size() / size_t(channels) : 0;
    }
    float duration() const {
        return rate ? float(frames()) / float(rate) : 0.0f;
    }

    // RIFF/WAVE, 8/16/24/32-bit PCM and 32-bit float. Enough to load
    // what a tool chain produces without pulling in a decoder.
    static Ref<AudioClip> load_wav(const std::string &path);
    static Ref<AudioClip> from_wav_memory(const uint8_t *data, size_t size,
                                          const char *name = "<memory>");
    // For tests and for a game with no assets yet.
    static Ref<AudioClip> tone(float hz, float seconds, int rate = 48000,
                               float amplitude = 0.5f);
    static Ref<AudioClip> noise(float seconds, int rate = 48000,
                                float amplitude = 0.5f, uint32_t seed = 1);
};

// What the mixer needs to know about one sounding voice. Written by
// the main thread, read by the audio thread, and deliberately made of
// nothing but plain numbers so that copying it is a memcpy.
struct VoiceState {
    bool active = false;
    bool loop = false;
    bool paused = false;
    // Linear gain per output channel. Panning, distance and
    // occlusion have all been folded in by the time it gets here --
    // the mixer does not know what a portal is.
    float gain_l = 0.0f;
    float gain_r = 0.0f;
    // Playback rate relative to the clip's own, with doppler in it.
    float pitch = 1.0f;
};

using VoiceId = uint32_t;
constexpr VoiceId kInvalidVoice = 0xFFFFFFFFu;

class AudioServer {
public:
    struct Config {
        int rate = 48000;
        int channels = 2;       // stereo; the mixer assumes it
        int buffer_frames = 512;
        int max_voices = 64;
        float master_gain = 1.0f;
        // A server with no device mixes when asked and nothing else.
        // Tests use it; so does a dedicated server, which should not
        // need a sound card to run a game.
        bool open_device = true;
    };

    bool init(const Config &cfg);
    void shutdown();
    bool running() const { return running_; }
    bool has_device() const { return device_ != 0; }
    const Config &config() const { return cfg_; }

    // Start a clip. Returns a handle that stays valid until the voice
    // finishes or is stopped; a stale handle is ignored rather than
    // being an error, because a sound ending on its own is normal and
    // whoever started it should not have to care.
    VoiceId play(const Ref<AudioClip> &clip, float gain = 1.0f,
                 float pitch = 1.0f, bool loop = false);
    void stop(VoiceId id);
    void stop_all();
    bool alive(VoiceId id) const;
    void set_voice(VoiceId id, const VoiceState &state);
    // Where the voice has got to, in seconds.
    float position(VoiceId id) const;

    void set_master_gain(float g) { master_gain_.store(g); }
    float master_gain() const { return master_gain_.load(); }

    // MIX `frames` FRAMES INTO `out`, interleaved stereo.
    //
    // Called by the device thread; called directly by a test, which
    // is the whole reason it is public. Adds to whatever is there, so
    // the caller clears.
    void mix(float *out, int frames);

    int active_voices() const;
    std::string report() const;

private:
    struct Voice {
        Ref<AudioClip> clip;    // held so it cannot be freed mid-sound
        double cursor = 0.0;    // in source frames, fractional for pitch
        VoiceState state;
        uint32_t generation = 1;
    };

    Config cfg_;
    uint32_t device_ = 0;
    bool running_ = false;
    std::atomic<float> master_gain_{1.0f};
    // One lock for the voice table. Held for the length of a memcpy
    // on the game side and for the length of a mix on the audio
    // side; an audio callback that blocks for microseconds is fine,
    // one that allocates is not.
    mutable std::mutex lock_;
    std::vector<Voice> voices_;
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> mixed_frames_{0};
};

}  // namespace mf
