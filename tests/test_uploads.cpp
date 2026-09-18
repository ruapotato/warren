// Warren -- what happens when more is uploaded in one frame than the
// staging ring holds.
//
// Loading a model asks for every one of its textures at once. That is
// routinely more than the ring, and there are three things a backend
// can do about it: drop the overflow (the model comes out untextured,
// silently), stall the frame (a hitch at every load), or hold the
// overflow over to the next frame. Warren holds it over.
//
// THE PART THAT IS EASY TO GET WRONG is the mip chain. Mips are built
// by blitting from level zero, so a chain built for a texture whose
// level zero was deferred is a chain of whatever was in the image
// when it was created. Level zero then lands the next frame and the
// mips are never rebuilt -- so the texture is right close up and
// garbage at any distance that samples a smaller level. On a
// character that reads as dirt on the model, which is a very long
// way from "an upload was deferred", and it is exactly what happened:
// five textures on a mesh were fine and seven were not.
//
// Every texture here is a SOLID colour, so every mip of it must be
// that same colour. Anything else is the bug.
#include <SDL2/SDL.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "platform/window.h"
#include "rhi/rhi.h"

using namespace wr;
using namespace wr::rhi;

namespace {

// Big enough that the lot cannot fit in one frame's ring: sixteen
// 512-pixel RGBA textures is sixteen megabytes before mips.
constexpr uint32_t kSize = 512;
constexpr int kCount = 16;
constexpr uint32_t kMip = 3;      // 64x64, well down the chain

int g_fail = 0;

void check(bool ok, const char *what) {
    std::printf("    %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fail++;
}

struct Result {
    bool available = false;
    std::string device;
    int clean = 0;          // textures whose mip is the right colour
    int checked = 0;
    uint8_t worst[4] = {0, 0, 0, 0};
    uint8_t wanted[4] = {0, 0, 0, 0};
};

Result run(Backend backend, bool validation) {
    Result out;
    WindowConfig wc;
    wc.backend = backend;
    wc.width = wc.height = 64;
    wc.resizable = false;
    wc.debug = validation;
    wc.title = "uploads";
    Window window;
    if (!window.open(wc)) return out;
    SDL_HideWindow(window.sdl_window());

    DeviceDesc dd;
    dd.backend = backend;
    dd.window = window.sdl_window();
    dd.validation = validation;
    dd.vsync = false;
    Device *dev = create_device(dd);
    if (!dev) return out;
    out.device = dev->caps().device_name;
    out.available = true;

    // A distinct solid colour each, so a mip that came from the wrong
    // place cannot accidentally look right.
    std::vector<TextureH> textures;
    std::vector<std::array<uint8_t, 4>> colours;
    std::vector<uint8_t> pixels(size_t(kSize) * kSize * 4);
    for (int i = 0; i < kCount; i++) {
        TextureDesc td;
        td.width = td.height = kSize;
        td.format = Format::RGBA8;
        td.usage = TextureUsage::Sampled | TextureUsage::TransferSrc |
                   TextureUsage::TransferDst;
        td.mips = 0;                       // the full chain
        td.name = "solid";
        TextureH h = dev->create_texture(td);
        if (!h.valid()) break;

        const std::array<uint8_t, 4> c{uint8_t(20 + i * 13),
                                       uint8_t(200 - i * 9),
                                       uint8_t(60 + i * 7), 255};
        for (size_t p = 0; p < pixels.size(); p += 4)
            std::memcpy(&pixels[p], c.data(), 4);
        // ALL OF THEM IN ONE GO, which is the whole point: this is
        // what loading a model looks like to the device.
        dev->write_texture(h, pixels.data(), pixels.size());
        dev->generate_mips(h);
        textures.push_back(h);
        colours.push_back(c);
    }

    // Enough frames for every deferral to drain. Each frame moves at
    // least a ring's worth, so a handful covers sixteen megabytes.
    for (int f = 0; f < 12; f++) {
        if (CommandList *cmd = dev->begin_frame()) {
            (void)cmd;
            dev->end_frame();
        }
    }
    dev->wait_idle();

    const uint32_t side = kSize >> kMip;
    std::vector<uint8_t> back(size_t(side) * side * 4);
    for (size_t i = 0; i < textures.size(); i++) {
        const size_t got =
            dev->read_texture(textures[i], back.data(), back.size(), kMip);
        if (got != back.size()) continue;
        out.checked++;
        bool same = true;
        for (size_t p = 0; p < back.size(); p += 4) {
            for (int ch = 0; ch < 3; ch++) {
                // A mip of a solid colour is that colour. One step of
                // rounding per level is the only slack allowed.
                if (std::abs(int(back[p + ch]) - int(colours[i][size_t(ch)])) > 2) {
                    same = false;
                    break;
                }
            }
            if (!same) {
                std::memcpy(out.worst, &back[p], 4);
                std::memcpy(out.wanted, colours[i].data(), 4);
                break;
            }
        }
        if (same) out.clean++;
    }

    for (TextureH h : textures) dev->destroy(h);
    destroy_device(dev);
    return out;
}

}  // namespace

int main(int argc, char **argv) {
    bool validation = false;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--validation")) validation = true;
    log_set_level(LogLevel::Warn);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("uploads\n  no video device: %s\n", SDL_GetError());
        return 77;
    }
    std::printf("uploads\n");

    Result runs[2] = {run(Backend::Vulkan, validation),
                      run(Backend::OpenGL, validation)};
    const char *names[2] = {"Vulkan", "OpenGL"};

    if (!runs[0].available && !runs[1].available) {
        std::printf("  neither backend would start; skipping\n");
        SDL_Quit();
        return 77;
    }

    for (int r = 0; r < 2; r++) {
        if (!runs[r].available) {
            std::printf("  %s: not available\n", names[r]);
            continue;
        }
        std::printf("  %-7s %s\n", names[r], runs[r].device.c_str());
        char what[220];
        std::snprintf(what, sizeof(what),
                      "[%s] all %d textures were readable at mip %u",
                      names[r], kCount, kMip);
        check(runs[r].checked == kCount, what);
        std::snprintf(what, sizeof(what),
                      "[%s] and every mip is still its own solid colour "
                      "(%d/%d; first bad pixel %u,%u,%u wanted %u,%u,%u)",
                      names[r], runs[r].clean, runs[r].checked,
                      runs[r].worst[0], runs[r].worst[1], runs[r].worst[2],
                      runs[r].wanted[0], runs[r].wanted[1], runs[r].wanted[2]);
        check(runs[r].clean == runs[r].checked && runs[r].checked > 0, what);
    }

    SDL_Quit();
    std::printf("  %s\n", g_fail ? "FAILED" : "all good");
    return g_fail ? 1 : 0;
}
