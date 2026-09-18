// Warren -- the mixer, and sound through a portal.
//
// Headless: the server is opened with no device and the test pumps
// the mixer itself, so what is checked is the samples rather than
// whether a sound card was plugged in.
//
// The interesting half is acoustic_path. A generator humming in the
// far room is forty metres away through the rock and two metres away
// through the arch, and it has to be heard at two metres coming from
// the arch -- otherwise the aperture sounds like a picture, which is
// the one impression this engine exists to avoid.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "audio/audio.h"
#include "core/log.h"
#include "scene/audio_nodes.h"
#include "scene/portal.h"
#include "scene/scene_tree.h"

using namespace wr;

namespace {

int g_fail = 0, g_checks = 0;
void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        g_fail++;
    }
}

// Root-mean-square of one channel of an interleaved stereo buffer.
double rms(const std::vector<float> &buf, int channel) {
    double sum = 0;
    size_t n = 0;
    for (size_t i = size_t(channel); i < buf.size(); i += 2) {
        sum += double(buf[i]) * buf[i];
        n++;
    }
    return n ? std::sqrt(sum / double(n)) : 0.0;
}

std::vector<float> pump(AudioServer &s, int frames) {
    std::vector<float> buf(size_t(frames) * 2, 0.0f);
    s.mix(buf.data(), frames);
    return buf;
}

}  // namespace

int main() {
    log_set_level(LogLevel::Warn);
    ClassDB::register_all();
    std::printf("audio\n");

    // ------------------------------------------------ the clip loader
    {
        Ref<AudioClip> t = AudioClip::tone(440.0f, 0.25f);
        check(t && t->frames() == 12000, "a tone is the length it was asked for");
        check(t && std::fabs(t->duration() - 0.25f) < 1e-4f,
              "and reports that duration");

        // Round-trip a clip through a 16-bit WAV in memory: header,
        // chunk walk, sample conversion.
        const int n = 1000;
        std::vector<uint8_t> wav;
        auto u32 = [&](uint32_t v) {
            for (int i = 0; i < 4; i++) wav.push_back(uint8_t(v >> (8 * i)));
        };
        auto u16 = [&](uint16_t v) {
            wav.push_back(uint8_t(v));
            wav.push_back(uint8_t(v >> 8));
        };
        const uint32_t data_bytes = uint32_t(n) * 2;
        wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
        u32(36 + data_bytes);
        wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
        u32(16);
        u16(1);        // PCM
        u16(1);        // mono
        u32(48000);
        u32(96000);
        u16(2);
        u16(16);
        // A LIST chunk in the middle, which is what a real tool
        // writes and what a loader that trusts the 44-byte layout
        // reads as audio.
        wav.insert(wav.end(), {'L', 'I', 'S', 'T'});
        u32(4);
        wav.insert(wav.end(), {'I', 'N', 'F', 'O'});
        wav.insert(wav.end(), {'d', 'a', 't', 'a'});
        u32(data_bytes);
        for (int i = 0; i < n; i++) {
            const int16_t v = int16_t(std::sin(float(i) * 0.1f) * 16384.0f);
            u16(uint16_t(v));
        }
        Ref<AudioClip> w = AudioClip::from_wav_memory(wav.data(), wav.size());
        check(w && w->frames() == size_t(n), "a wav with a LIST chunk loads");
        check(w && w->rate == 48000 && w->channels == 1,
              "with the right rate and channel count");
        if (w) {
            float worst = 0;
            for (int i = 0; i < n; i++)
                worst = std::max(worst,
                                 std::fabs(w->samples[size_t(i)] -
                                           std::sin(float(i) * 0.1f) * 0.5f));
            check(worst < 1.0f / 1000.0f, "and the samples survive the trip");
        }
    }

    // ------------------------------------------------------- the mixer
    AudioServer server;
    AudioServer::Config cfg;
    cfg.open_device = false;
    cfg.rate = 48000;
    cfg.max_voices = 8;
    check(server.init(cfg), "an offline server starts without a device");
    check(!server.has_device(), "and opens no device");

    Ref<AudioClip> tone = AudioClip::tone(440.0f, 1.0f);
    {
        VoiceId v = server.play(tone, 1.0f);
        check(v != kInvalidVoice, "a clip can be played");
        check(server.alive(v), "and the voice is alive");

        VoiceState st;
        st.active = true;
        st.gain_l = 1.0f;
        st.gain_r = 0.25f;
        server.set_voice(v, st);
        std::vector<float> buf = pump(server, 4096);
        const double l = rms(buf, 0), r = rms(buf, 1);
        char what[160];
        std::snprintf(what, sizeof(what),
                      "per-channel gain reaches the output (L %.4f, R %.4f)", l,
                      r);
        check(l > 0.05 && std::fabs(r / std::max(l, 1e-9) - 0.25) < 0.02, what);

        server.stop(v);
        check(!server.alive(v), "and stopping ends it");
        buf = pump(server, 512);
        check(rms(buf, 0) == 0.0, "after which it makes no sound");
    }
    {
        // A voice ends on its own at the end of a one-shot, and the
        // handle to it stops being alive rather than becoming a
        // handle to whatever took its slot.
        Ref<AudioClip> blip = AudioClip::tone(440.0f, 0.01f);   // 480 frames
        VoiceId v = server.play(blip, 1.0f);
        VoiceState st;
        st.active = true;
        st.gain_l = st.gain_r = 1.0f;
        server.set_voice(v, st);
        pump(server, 1024);
        check(!server.alive(v), "a one-shot ends by itself");
        VoiceId w = server.play(tone, 1.0f);
        check(w != v, "and the slot it freed hands out a different handle");
        server.stop(w);
    }
    {
        // Running out of voices is a refusal, not a crash or a
        // silent overwrite of something already sounding.
        std::vector<VoiceId> all;
        for (int i = 0; i < 16; i++) all.push_back(server.play(tone, 1.0f));
        int got = 0;
        for (VoiceId v : all)
            if (v != kInvalidVoice) got++;
        check(got == cfg.max_voices, "the voice limit is a limit");
        server.stop_all();
        check(server.active_voices() == 0, "and stop_all clears them");
    }

    // ------------------------------------------- SOUND THROUGH A PORTAL
    {
        SceneTree tree;
        Node3D *scene = new Node3D();
        scene->set_name("Scene");
        tree.set_scene(scene);

        // The listener at the origin. A portal two metres in front of
        // it, linked to one eighty metres away. The source sits just
        // behind the far portal -- so it is eighty metres through the
        // rock and about three through the arch.
        Portal3D *a = new Portal3D();
        a->set_name("A");
        a->width = a->height = 3.0f;
        a->set_position({0, 0, -2});
        scene->add_child(a);

        Portal3D *b = new Portal3D();
        b->set_name("B");
        b->width = b->height = 3.0f;
        b->set_position({80, 0, 0});
        scene->add_child(b);
        a->link_to(b);

        std::vector<Portal3D *> portals;
        collect_portals(scene, &portals);
        check(portals.size() == 2, "both portals are found");

        const Vec3 listener(0, 0, 0);
        const Vec3 source(80, 0, 1.0f);   // a metre in front of B

        Vec3 dir;
        int hops = 0;
        const float straight =
            acoustic_path(listener, source, portals, 0, &dir, &hops);
        char what[200];
        std::snprintf(what, sizeof(what),
                      "with no portal hops the sound is %.1f m away", straight);
        check(straight > 79.0f && hops == 0, what);

        const float through =
            acoustic_path(listener, source, portals, 1, &dir, &hops);
        std::printf("       source 80 m east, behind a portal 2 m in front:\n"
                    "         through the rock %.1f m, through the arch "
                    "%.2f m, arriving from (%.2f %.2f %.2f)\n",
                    straight, through, double(dir.x), double(dir.y),
                    double(dir.z));
        std::snprintf(what, sizeof(what),
                      "through the portal it is %.2f m away, not %.1f",
                      through, straight);
        check(through < 4.0f && hops == 1, what);

        // AND IT COMES FROM THE HOLE. Direction is what separates
        // this from turning the volume up: the sound has to arrive
        // from the aperture the listener can see, not from eighty
        // metres east.
        std::snprintf(what, sizeof(what),
                      "and arrives from the aperture (%.2f %.2f %.2f)",
                      double(dir.x), double(dir.y), double(dir.z));
        check(dir.z < -0.9f && std::fabs(dir.x) < 0.2f, what);

        // Move the source deeper into the far room and the path
        // lengthens by the same amount: the portal is an opening, not
        // a speaker fixed at one volume.
        Vec3 d2;
        const float deeper =
            acoustic_path(listener, Vec3(80, 0, 9.0f), portals, 1, &d2, &hops);
        std::snprintf(what, sizeof(what),
                      "a source deeper in the far room is further (%.2f vs "
                      "%.2f)",
                      deeper, through);
        check(deeper > through + 7.0f, what);

        // --- and the gain and panning that come out of it
        AudioPlayer3D *player = new AudioPlayer3D();
        player->set_name("Hum");
        player->clip = AudioClip::tone(220.0f, 1.0f);
        player->loop = true;
        player->max_distance = 60.0f;
        player->reference_distance = 1.0f;
        player->set_position(source);
        scene->add_child(player);

        AudioSystem sys;
        sys.set_server(&server);
        audio_system_install(&sys);
        player->play();
        check(player->playing(), "a 3D player starts a voice");

        Camera3D *cam = new Camera3D();
        cam->set_name("Camera");
        cam->set_position(listener);
        cam->look_at({0, 0, -1});
        scene->add_child(cam);
        cam->make_current();

        tree.process(1.0f / 60.0f);
        sys.update(&tree, 1.0f / 60.0f);

        std::snprintf(what, sizeof(what),
                      "and it hears the source through the portal (%.2f m, %d "
                      "hop)",
                      double(player->effective_distance()),
                      player->portals_used());
        check(player->portals_used() == 1 &&
                  player->effective_distance() < 4.0f,
              what);

        std::vector<float> buf = pump(server, 4096);
        const double heard = rms(buf, 0) + rms(buf, 1);
        std::snprintf(what, sizeof(what),
                      "and it is audible (rms %.4f)", heard);
        check(heard > 0.01, what);

        // With portal hops turned off the same source is eighty
        // metres away, past max_distance, and silent -- which is the
        // measurement that says the portal is doing the work.
        player->portal_hops = 0;
        sys.update(&tree, 1.0f / 60.0f);
        buf = pump(server, 4096);
        const double muted = rms(buf, 0) + rms(buf, 1);
        std::printf("         heard at rms %.4f with portal hops, %.4f "
                    "without\n", heard, muted);
        std::snprintf(what, sizeof(what),
                      "and silent without the portal (rms %.4f against %.4f)",
                      muted, heard);
        check(muted < heard * 0.05, what);

        player->stop();
        audio_system_install(nullptr);
        tree.set_scene(nullptr);
    }

    server.shutdown();
    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
