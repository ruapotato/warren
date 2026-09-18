// Manifold -- loading and making sounds.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "audio/audio.h"
#include "core/bind.h"
#include "core/log.h"

namespace mf {
namespace {

struct Reader {
    const uint8_t *p = nullptr;
    size_t left = 0;
    bool ok = true;

    uint32_t u32() {
        if (left < 4) { ok = false; return 0; }
        uint32_t v;
        std::memcpy(&v, p, 4);
        p += 4;
        left -= 4;
        return v;
    }
    uint16_t u16() {
        if (left < 2) { ok = false; return 0; }
        uint16_t v;
        std::memcpy(&v, p, 2);
        p += 2;
        left -= 2;
        return v;
    }
    void skip(size_t n) {
        if (left < n) { ok = false; left = 0; return; }
        p += n;
        left -= n;
    }
};

}  // namespace

Ref<AudioClip> AudioClip::from_wav_memory(const uint8_t *data, size_t size,
                                          const char *name) {
    if (!data || size < 44) {
        MF_ERROR("wav: '%s' is too short to be a RIFF file", name);
        return {};
    }
    Reader r{data, size};
    if (std::memcmp(r.p, "RIFF", 4) != 0) {
        MF_ERROR("wav: '%s' is not RIFF", name);
        return {};
    }
    r.skip(8);  // "RIFF", size
    if (std::memcmp(r.p, "WAVE", 4) != 0) {
        MF_ERROR("wav: '%s' is RIFF but not WAVE", name);
        return {};
    }
    r.skip(4);

    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *pcm = nullptr;
    size_t pcm_bytes = 0;

    // CHUNK BY CHUNK, NOT BY OFFSET. A wav written by a tool often
    // carries LIST or fact chunks before the data, and a loader that
    // assumes the canonical 44-byte header reads metadata as audio
    // and produces a burst of noise.
    while (r.ok && r.left >= 8) {
        char id[5] = {};
        std::memcpy(id, r.p, 4);
        r.skip(4);
        const uint32_t len = r.u32();
        if (!r.ok || len > r.left) break;
        if (!std::strcmp(id, "fmt ")) {
            Reader f{r.p, len};
            format = f.u16();
            channels = f.u16();
            rate = f.u32();
            f.u32();            // byte rate
            f.u16();            // block align
            bits = f.u16();
        } else if (!std::strcmp(id, "data")) {
            pcm = r.p;
            pcm_bytes = len;
        }
        r.skip(len + (len & 1));   // chunks are word aligned
    }

    if (!pcm || !channels || !rate) {
        MF_ERROR("wav: '%s' has no usable fmt/data chunk", name);
        return {};
    }
    // 1 = integer PCM, 3 = IEEE float.
    if (format != 1 && format != 3) {
        MF_ERROR("wav: '%s' is compressed (format %u), which is not supported",
                 name, format);
        return {};
    }

    Ref<AudioClip> clip(new AudioClip());
    clip->channels = int(channels);
    clip->rate = int(rate);
    const size_t bytes_per = size_t(bits) / 8;
    if (!bytes_per) return {};
    const size_t count = pcm_bytes / bytes_per;
    clip->samples.resize(count);
    for (size_t i = 0; i < count; i++) {
        const uint8_t *s = pcm + i * bytes_per;
        switch (bits) {
            case 8:
                // Unsigned, unlike every other width.
                clip->samples[i] = (float(s[0]) - 128.0f) / 128.0f;
                break;
            case 16: {
                int16_t v;
                std::memcpy(&v, s, 2);
                clip->samples[i] = float(v) / 32768.0f;
                break;
            }
            case 24: {
                const int32_t v = (int32_t(int8_t(s[2])) << 16) |
                                  (int32_t(s[1]) << 8) | int32_t(s[0]);
                clip->samples[i] = float(v) / 8388608.0f;
                break;
            }
            case 32:
                if (format == 3) {
                    std::memcpy(&clip->samples[i], s, 4);
                } else {
                    int32_t v;
                    std::memcpy(&v, s, 4);
                    clip->samples[i] = float(double(v) / 2147483648.0);
                }
                break;
            default:
                MF_ERROR("wav: '%s' is %u-bit, which is not supported", name,
                         bits);
                return {};
        }
    }
    return clip;
}

Ref<AudioClip> AudioClip::load_wav(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        MF_ERROR("wav: could not open '%s'", path.c_str());
        return {};
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> bytes(size_t(std::max(0L, n)));
    const size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    bytes.resize(got);
    return from_wav_memory(bytes.data(), bytes.size(), path.c_str());
}

Ref<AudioClip> AudioClip::tone(float hz, float seconds, int rate,
                               float amplitude) {
    Ref<AudioClip> c(new AudioClip());
    c->channels = 1;
    c->rate = rate;
    const size_t n = size_t(std::max(0.0f, seconds) * float(rate));
    c->samples.resize(n);
    for (size_t i = 0; i < n; i++) {
        const float t = float(i) / float(rate);
        // A short fade at each end, because a sine that starts at
        // full amplitude is a click.
        const float fade = std::min(1.0f, std::min(t, seconds - t) * 200.0f);
        c->samples[i] =
            std::sin(6.28318530718f * hz * t) * amplitude * std::max(0.0f, fade);
    }
    return c;
}

Ref<AudioClip> AudioClip::noise(float seconds, int rate, float amplitude,
                                uint32_t seed) {
    Ref<AudioClip> c(new AudioClip());
    c->channels = 1;
    c->rate = rate;
    const size_t n = size_t(std::max(0.0f, seconds) * float(rate));
    c->samples.resize(n);
    uint32_t s = seed ? seed : 1;
    for (size_t i = 0; i < n; i++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        c->samples[i] = (float(s & 0xFFFFFF) / 8388608.0f - 1.0f) * amplitude;
    }
    return c;
}

static void register_audio_clip() {
    ClassBuilder<AudioClip>()
        .method("duration", &AudioClip::duration)
        .method("frames", &AudioClip::frames);
}
MF_REGISTER(register_audio_clip)

}  // namespace mf
