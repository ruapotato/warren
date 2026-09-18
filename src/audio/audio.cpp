#include "audio/audio.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/bind.h"
#include "core/log.h"

namespace wr {
namespace {

void sdl_callback(void *userdata, Uint8 *stream, int len) {
    AudioServer *server = (AudioServer *)userdata;
    float *out = (float *)stream;
    const int frames = len / int(sizeof(float) * 2);
    std::memset(stream, 0, size_t(len));
    server->mix(out, frames);
}

// Linear interpolation between source frames. Not the best
// resampler, and for pitch shifts inside a semitone or two -- which
// is what doppler and per-instance variation ask for -- the
// difference from a windowed sinc is not audible over a game.
inline float sample_at(const AudioClip &clip, double frame, int channel) {
    const size_t n = clip.frames();
    if (!n) return 0.0f;
    const double clamped = std::max(0.0, frame);
    const size_t i0 = size_t(clamped);
    if (i0 + 1 >= n) {
        const size_t last = n - 1;
        return clip.samples[last * size_t(clip.channels) + size_t(channel)];
    }
    const float t = float(clamped - double(i0));
    const float a = clip.samples[i0 * size_t(clip.channels) + size_t(channel)];
    const float b =
        clip.samples[(i0 + 1) * size_t(clip.channels) + size_t(channel)];
    return a + (b - a) * t;
}

}  // namespace

// ------------------------------------------------------------- the server

bool AudioServer::init(const Config &cfg) {
    if (running_) return true;
    cfg_ = cfg;
    voices_.assign(size_t(std::max(1, cfg.max_voices)), Voice{});
    master_gain_.store(cfg.master_gain);

    if (!cfg.open_device) {
        running_ = true;
        WR_INFO("audio: %d Hz, %d voices, no device (offline)", cfg_.rate,
                cfg_.max_voices);
        return true;
    }

    if (SDL_WasInit(SDL_INIT_AUDIO) == 0 &&
        SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        WR_WARN("audio: no audio subsystem (%s); running silent", SDL_GetError());
        running_ = true;
        return true;
    }

    SDL_AudioSpec want{}, have{};
    want.freq = cfg_.rate;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = Uint16(cfg_.buffer_frames);
    want.callback = sdl_callback;
    want.userdata = this;
    device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have,
                                  SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!device_) {
        // NOT AN ERROR. A machine with no sound card, a CI runner, a
        // dedicated server: all of them should run the game, and a
        // game that refuses to start because it cannot make a noise
        // is a worse outcome than a quiet one.
        WR_WARN("audio: no device (%s); running silent", SDL_GetError());
        running_ = true;
        return true;
    }
    cfg_.rate = have.freq;
    cfg_.buffer_frames = have.samples;
    SDL_PauseAudioDevice(device_, 0);
    running_ = true;
    WR_INFO("audio: %d Hz, %d frames a buffer, %d voices, device '%s'",
            cfg_.rate, cfg_.buffer_frames, cfg_.max_voices,
            SDL_GetAudioDeviceName(0, 0) ? SDL_GetAudioDeviceName(0, 0) : "?");
    return true;
}

void AudioServer::shutdown() {
    if (!running_) return;
    if (device_) {
        // Close BEFORE the voices go: the callback holds the lock and
        // reads the table, and freeing a clip out from under it is
        // the one race this design has to avoid.
        SDL_CloseAudioDevice(device_);
        device_ = 0;
    }
    {
        std::lock_guard<std::mutex> g(lock_);
        voices_.clear();
    }
    running_ = false;
}

VoiceId AudioServer::play(const Ref<AudioClip> &clip, float gain, float pitch,
                          bool loop) {
    if (!running_ || !clip || clip->frames() == 0) return kInvalidVoice;
    std::lock_guard<std::mutex> g(lock_);
    for (uint32_t i = 0; i < uint32_t(voices_.size()); i++) {
        Voice &v = voices_[i];
        if (v.state.active) continue;
        v.clip = clip;
        v.cursor = 0.0;
        v.state = VoiceState{};
        v.state.active = true;
        v.state.loop = loop;
        v.state.gain_l = gain;
        v.state.gain_r = gain;
        v.state.pitch = pitch;
        // The generation makes a handle to a finished voice safe to
        // hold: the slot is reused, the generation is not.
        return (v.generation << 16) | i;
    }
    // Out of voices. Not worth a warning every frame -- a game that
    // wants to know can read active_voices().
    return kInvalidVoice;
}

void AudioServer::stop(VoiceId id) {
    if (id == kInvalidVoice) return;
    std::lock_guard<std::mutex> g(lock_);
    const uint32_t i = id & 0xFFFF;
    if (i >= voices_.size()) return;
    Voice &v = voices_[i];
    if ((v.generation << 16 | i) != id) return;
    v.state.active = false;
    v.clip.reset();
    v.generation++;
}

void AudioServer::stop_all() {
    std::lock_guard<std::mutex> g(lock_);
    for (Voice &v : voices_) {
        if (!v.state.active) continue;
        v.state.active = false;
        v.clip.reset();
        v.generation++;
    }
}

bool AudioServer::alive(VoiceId id) const {
    if (id == kInvalidVoice) return false;
    std::lock_guard<std::mutex> g(lock_);
    const uint32_t i = id & 0xFFFF;
    if (i >= voices_.size()) return false;
    const Voice &v = voices_[i];
    return v.state.active && (v.generation << 16 | i) == id;
}

void AudioServer::set_voice(VoiceId id, const VoiceState &state) {
    if (id == kInvalidVoice) return;
    std::lock_guard<std::mutex> g(lock_);
    const uint32_t i = id & 0xFFFF;
    if (i >= voices_.size()) return;
    Voice &v = voices_[i];
    if (!v.state.active || (v.generation << 16 | i) != id) return;
    // `active` stays the mixer's to clear: a voice that ran off the
    // end of its clip this buffer must not be resurrected by a
    // parameter update that was in flight.
    const bool was = v.state.active;
    v.state = state;
    v.state.active = was;
}

float AudioServer::position(VoiceId id) const {
    if (id == kInvalidVoice) return 0.0f;
    std::lock_guard<std::mutex> g(lock_);
    const uint32_t i = id & 0xFFFF;
    if (i >= voices_.size()) return 0.0f;
    const Voice &v = voices_[i];
    if (!v.state.active || (v.generation << 16 | i) != id || !v.clip)
        return 0.0f;
    return float(v.cursor) / float(std::max(1, v.clip->rate));
}

int AudioServer::active_voices() const {
    std::lock_guard<std::mutex> g(lock_);
    int n = 0;
    for (const Voice &v : voices_)
        if (v.state.active) n++;
    return n;
}

void AudioServer::mix(float *out, int frames) {
    if (!out || frames <= 0) return;
    std::lock_guard<std::mutex> g(lock_);
    const float master = master_gain_.load();
    for (Voice &v : voices_) {
        if (!v.state.active || v.state.paused || !v.clip) continue;
        const AudioClip &clip = *v.clip;
        const size_t n = clip.frames();
        if (!n) {
            v.state.active = false;
            v.generation++;
            continue;
        }
        // The clip may have been recorded at a different rate from
        // the device; that ratio and the voice's pitch are the same
        // multiplication, so they are applied as one.
        const double step = double(v.state.pitch) * double(clip.rate) /
                            double(std::max(1, cfg_.rate));
        const float gl = v.state.gain_l * master;
        const float gr = v.state.gain_r * master;
        const bool mono = clip.channels == 1;

        for (int f = 0; f < frames; f++) {
            if (v.cursor >= double(n)) {
                if (!v.state.loop) {
                    v.state.active = false;
                    v.clip.reset();
                    v.generation++;
                    break;
                }
                v.cursor = std::fmod(v.cursor, double(n));
            }
            const float l = sample_at(clip, v.cursor, 0);
            const float r = mono ? l : sample_at(clip, v.cursor, 1);
            out[f * 2 + 0] += l * gl;
            out[f * 2 + 1] += r * gr;
            v.cursor += step;
        }
    }
    mixed_frames_.fetch_add(uint64_t(frames));
}

std::string AudioServer::report() const {
    char b[192];
    std::snprintf(b, sizeof(b),
                  "audio: %d/%d voices, %d Hz, %s, %llu frames mixed",
                  active_voices(), cfg_.max_voices, cfg_.rate,
                  device_ ? "device open" : "silent",
                  (unsigned long long)mixed_frames_.load());
    return b;
}

}  // namespace wr
