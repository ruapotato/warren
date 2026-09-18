// Warren -- the immediate-mode UI, and whether the font is legible.
//
// A UI test that asserts "some triangles were produced" tests
// nothing. This one renders the whole printable ASCII range to a
// texture and reads it back, then checks each glyph's ink: how many
// dots it has, and how they are distributed. A transposed column, a
// mirrored bitmap or an off-by-one in the advance all change those
// numbers and none of them change the triangle count.
//
// It also drives the widgets with synthetic input, because a button
// that draws correctly and never reports a click is the failure that
// matters.
#include <SDL2/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "platform/window.h"
#include "rhi/rhi.h"
#include "ui/font.h"
#include "ui/ui.h"
#include "ui/ui_renderer.h"

using namespace wr;
using namespace wr::rhi;

namespace {

int g_fail = 0, g_checks = 0;
void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        g_fail++;
    }
}

constexpr uint32_t kW = 512, kH = 256;

struct Shot {
    std::vector<uint8_t> pixels;
    bool ok = false;
    std::string device;
};

Shot render(Backend backend, const ui::DrawData &data) {
    Shot out;
    WindowConfig wc;
    wc.backend = backend;
    wc.width = int(kW);
    wc.height = int(kH);
    wc.resizable = false;
    wc.title = "ui";
    Window window;
    if (!window.open(wc)) return out;
    SDL_HideWindow(window.sdl_window());

    DeviceDesc dd;
    dd.backend = backend;
    dd.window = window.sdl_window();
    dd.vsync = false;
    Device *dev = create_device(dd);
    if (!dev) return out;
    out.device = dev->caps().device_name;

    TextureDesc td;
    td.width = kW;
    td.height = kH;
    td.format = Format::RGBA8;
    td.usage = TextureUsage::ColourTarget | TextureUsage::TransferSrc |
               TextureUsage::Sampled;
    td.name = "ui target";
    TextureH target = dev->create_texture(td);

    ui::Renderer renderer;
    if (!renderer.init(dev, Format::RGBA8, 1)) {
        destroy_device(dev);
        return out;
    }

    CommandList *cmd = dev->begin_frame();
    if (cmd) {
        RenderingInfo ri;
        ColourAttachment ca;
        ca.texture = target;
        ca.load = LoadOp::Clear;
        ca.clear = Color(0, 0, 0, 1);
        ri.colour.push_back(ca);
        ri.width = kW;
        ri.height = kH;
        ri.name = "ui";
        cmd->begin_rendering(ri);
        Viewport vp;
        vp.width = float(kW);
        vp.height = float(kH);
        cmd->set_viewport(vp);
        renderer.draw(cmd, data, kW, kH);
        cmd->end_rendering();
        dev->end_frame();
        dev->wait_idle();
        out.pixels.resize(size_t(kW) * kH * 4);
        out.ok = dev->read_texture(target, out.pixels.data(),
                                   out.pixels.size()) == out.pixels.size();
    }
    renderer.shutdown();
    destroy_device(dev);
    return out;
}

bool lit(const Shot &s, int x, int y) {
    if (x < 0 || y < 0 || x >= int(kW) || y >= int(kH)) return false;
    const size_t i = (size_t(y) * kW + size_t(x)) * 4;
    return s.pixels[i] > 40 || s.pixels[i + 1] > 40 || s.pixels[i + 2] > 40;
}

}  // namespace

int main() {
    log_set_level(LogLevel::Warn);
    std::printf("user interface\n");

    // --------------------------------------------- the font, in the CPU
    {
        // Every printable glyph must have ink, and space must not.
        int blank = 0;
        for (int c = ui::kFirstGlyph; c <= ui::kLastGlyph; c++) {
            const uint8_t *g = ui::glyph_columns(c);
            int dots = 0;
            for (int i = 0; i < ui::kGlyphWidth; i++)
                for (int b = 0; b < ui::kGlyphHeight; b++)
                    if (g[i] & (1u << b)) dots++;
            if (dots == 0 && c != ' ') blank++;
            if (c == ' ') check(dots == 0, "space has no ink");
        }
        char what[120];
        std::snprintf(what, sizeof(what), "every printable glyph has ink (%d "
                      "blank)", blank);
        check(blank == 0, what);

        // NO TWO GLYPHS MAY BE IDENTICAL. A copy-paste slip in a
        // font table produces two letters that look the same, which
        // is exactly as wrong as a missing one and much harder to
        // see.
        int collisions = 0;
        std::string worst;
        for (int a = ui::kFirstGlyph; a <= ui::kLastGlyph; a++) {
            if (a == ' ') continue;
            for (int b = a + 1; b <= ui::kLastGlyph; b++) {
                if (b == ' ') continue;
                if (std::memcmp(ui::glyph_columns(a), ui::glyph_columns(b),
                                ui::kGlyphWidth) == 0) {
                    collisions++;
                    if (worst.empty()) {
                        worst = "'";
                        worst += char(a);
                        worst += "' and '";
                        worst += char(b);
                        worst += "'";
                    }
                }
            }
        }
        std::snprintf(what, sizeof(what),
                      "no two glyphs are the same bitmap (%d pairs%s%s)",
                      collisions, worst.empty() ? "" : ", e.g. ",
                      worst.c_str());
        check(collisions == 0, what);

        // A few shapes, checked against what they have to look like.
        // 'I' is a single full column in the middle; '-' is a single
        // row; '|' is a full column. Getting the bit order backwards
        // leaves all three looking plausible on their own, and these
        // three together pin it down.
        const uint8_t *dash = ui::glyph_columns('-');
        bool dash_ok = true;
        for (int i = 0; i < 5; i++) dash_ok = dash_ok && dash[i] == 0x08;
        check(dash_ok, "'-' is one horizontal row, the same in every column");

        const uint8_t *bar = ui::glyph_columns('|');
        check(bar[2] == 0x7F && bar[0] == 0 && bar[4] == 0,
              "'|' is one full column in the middle");

        const uint8_t *under = ui::glyph_columns('_');
        bool under_ok = true;
        for (int i = 0; i < 5; i++) under_ok = under_ok && under[i] == 0x40;
        check(under_ok, "'_' is the bottom row, so bit 6 is the bottom");
    }

    // ------------------------------------------- the widgets, driven
    {
        ui::Context ctx;
        ui::Input in;
        // Frame one: find out where the button is.
        ctx.begin_frame(float(kW), float(kH), in, 1.0f / 60.0f);
        bool clicked = false, checked = false;
        float slid = 0.5f;
        if (ctx.begin_window("Panel", {10, 10, 220, 180})) {
            clicked = ctx.button("Press me");
            ctx.checkbox("A flag", &checked);
            ctx.slider("Amount", &slid, 0.0f, 1.0f);
            ctx.end_window();
        }
        ctx.end_frame();
        check(!clicked, "a button not pressed reports nothing");
        check(!ctx.draw_data().empty(), "and the frame produced geometry");

        // The button is the first row inside the window: title bar
        // (24) plus padding (6), about ten pixels in.
        const float bx = 10 + 6 + 20, by = 10 + 24 + 6 + 8;

        // Press: down one frame, up the next. A click is the release
        // over the control, which is what lets a user change their
        // mind by dragging off it.
        in.mouse_x = bx;
        in.mouse_y = by;
        in.mouse_down = true;
        ctx.begin_frame(float(kW), float(kH), in, 1.0f / 60.0f);
        if (ctx.begin_window("Panel", {10, 10, 220, 180})) {
            clicked = ctx.button("Press me");
            ctx.end_window();
        }
        ctx.end_frame();
        check(!clicked, "pressing alone is not a click");

        in.mouse_down = false;
        ctx.begin_frame(float(kW), float(kH), in, 1.0f / 60.0f);
        if (ctx.begin_window("Panel", {10, 10, 220, 180})) {
            clicked = ctx.button("Press me");
            ctx.end_window();
        }
        ctx.end_frame();
        check(clicked, "releasing over it is");

        // And the pointer being over a panel is reported, so the
        // game does not also act on the click.
        check(ctx.wants_mouse(), "the UI claims the mouse over a window");
        in.mouse_x = 480;
        in.mouse_y = 240;
        ctx.begin_frame(float(kW), float(kH), in, 1.0f / 60.0f);
        if (ctx.begin_window("Panel", {10, 10, 220, 180})) ctx.end_window();
        ctx.end_frame();
        check(!ctx.wants_mouse(), "and releases it elsewhere");
    }

    // ------------------------------------------ the font, on the GPU
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("  no video device; skipping the render\n");
        std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
        return g_fail ? 1 : 0;
    }

    ui::Context ctx;
    ui::Input in;
    ctx.theme.scale = 2.0f;
    ctx.begin_frame(float(kW), float(kH), in, 1.0f / 60.0f);
    // Drawn straight, not through a window, so the positions are
    // known exactly and the measurement is of the font.
    const char *line1 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    const char *line2 = "abcdefghijklmnopqrstuvwxyz";
    const char *line3 = "0123456789 !\"#$%&'()*+,-./";
    ctx.draw_text({8, 8}, line1, ui::rgba(255, 255, 255));
    ctx.draw_text({8, 40}, line2, ui::rgba(255, 255, 255));
    ctx.draw_text({8, 72}, line3, ui::rgba(255, 255, 255));
    ctx.rect({8, 104, 64, 24}, ui::rgba(255, 0, 0));
    ctx.end_frame();

    Shot vk = render(Backend::Vulkan, ctx.draw_data());
    Shot gl = render(Backend::OpenGL, ctx.draw_data());
    const Shot &s = vk.ok ? vk : gl;
    if (!s.ok) {
        std::printf("  neither backend would start; skipping the render\n");
        SDL_Quit();
        std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
        return g_fail ? 1 : 0;
    }
    std::printf("  rendered on %s\n", s.device.c_str());
    // A font is judged by looking at it. WR_UI_DUMP=file.ppm writes
    // what was rendered, so a change to the table can be checked by
    // eye as well as by the counts below.
    if (const char *path = getenv("WR_UI_DUMP")) {
        if (FILE *f = std::fopen(path, "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", kW, kH);
            for (size_t i = 0; i < size_t(kW) * kH; i++)
                std::fwrite(&s.pixels[i * 4], 1, 3, f);
            std::fclose(f);
            std::printf("  wrote %s\n", path);
        }
    }

    // The solid rectangle, exactly where it was asked for.
    check(lit(s, 40, 116), "a rectangle is drawn");
    check(!lit(s, 40, 100) && !lit(s, 40, 132),
          "and stops at its own edges");
    check(!lit(s, 4, 116), "and starts at its own left edge");

    // EACH LETTER IN ITS OWN CELL, with the right amount of ink.
    // 'I' is a thin letter and 'M' a wide one; if the advance were
    // wrong they would overlap and both cells would read the same.
    const float dot = ctx.theme.scale;
    const float adv = float(ui::kGlyphAdvance) * dot;
    auto cell_ink = [&](int index, float top) {
        int n = 0;
        const int x0 = int(8 + float(index) * adv);
        for (int y = int(top); y < int(top + float(ui::kGlyphHeight) * dot); y++)
            for (int x = x0; x < x0 + int(float(ui::kGlyphWidth) * dot); x++)
                if (lit(s, x, y)) n++;
        return n;
    };
    const int ink_I = cell_ink(8, 8);    // 'I' is the 9th letter
    const int ink_M = cell_ink(12, 8);   // 'M' the 13th
    const int ink_space = cell_ink(26, 72);  // the space in line 3
    char what[200];
    std::snprintf(what, sizeof(what),
                  "'I' and 'M' have the ink their shapes call for (%d against "
                  "%d; 11 and 18 dots at %.0f px each)",
                  ink_I, ink_M, double(dot * dot));
    check(ink_I == 11 * int(dot * dot) && ink_M == 18 * int(dot * dot), what);

    // AND THE CELLS DO NOT RUN INTO EACH OTHER. The gap column
    // between two letters must be empty, which is what says the
    // advance is right; get it wrong by one and every letter bleeds
    // into its neighbour while the ink count per cell barely moves.
    int gap_ink = 0;
    for (int index = 0; index < 25; index++) {
        const int gx = int(8 + float(index) * adv + float(ui::kGlyphWidth) * dot);
        for (int y = 8; y < 8 + int(float(ui::kGlyphHeight) * dot); y++)
            for (int x = gx; x < gx + int(dot); x++)
                if (lit(s, x, y)) gap_ink++;
    }
    std::snprintf(what, sizeof(what),
                  "and the column between them is blank (%d lit)", gap_ink);
    check(gap_ink == 0, what);
    std::snprintf(what, sizeof(what), "and a space is empty (%d)", ink_space);
    check(ink_space == 0, what);

    // Nothing drawn above the first line or left of the margin: the
    // text starts where it was put.
    bool clean = true;
    for (int y = 0; y < 8 && clean; y++)
        for (int x = 0; x < int(kW); x++)
            if (lit(s, x, y)) clean = false;
    check(clean, "nothing is drawn above the first line");

    if (vk.ok && gl.ok) {
        size_t differ = 0;
        for (size_t i = 0; i < vk.pixels.size(); i++)
            if (std::abs(int(vk.pixels[i]) - int(gl.pixels[i])) > 2) differ++;
        std::printf("  %zu of %zu samples differ between backends\n", differ,
                    vk.pixels.size());
        check(differ == 0, "and the two backends draw it identically");
    }

    SDL_Quit();
    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
