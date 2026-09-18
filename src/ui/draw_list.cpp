#include "ui/draw_list.h"

#include <cmath>

#include "ui/font.h"

namespace wr::ui {

void DrawList::begin(float width, float height) {
    data.clear();
    clip_stack_.clear();
    width_ = width;
    height_ = height;
    clip_ = {0, 0, width, height};
    command_start_ = 0;
}

void DrawList::end() { flush(); }

void DrawList::flush() {
    const uint32_t end = uint32_t(data.indices.size());
    if (end == command_start_) return;
    DrawCommand c;
    c.clip = clip_;
    c.first_index = command_start_;
    c.index_count = end - command_start_;
    data.commands.push_back(c);
    command_start_ = end;
}

void DrawList::push_clip(const Rect &r) {
    flush();
    clip_stack_.push_back(clip_);
    clip_ = clip_.clipped(r);
}

void DrawList::pop_clip() {
    flush();
    if (clip_stack_.empty()) {
        clip_ = {0, 0, width_, height_};
        return;
    }
    clip_ = clip_stack_.back();
    clip_stack_.pop_back();
}

void DrawList::rect(const Rect &r, uint32_t colour) {
    if (r.empty() || (colour >> 24) == 0) return;
    const uint32_t base = uint32_t(data.vertices.size());
    // A white texel, so a solid rectangle and a glyph share one
    // texture and one draw call.
    const Vec2 white(1.0f, 1.0f);
    data.vertices.push_back({{r.x, r.y}, white, colour});
    data.vertices.push_back({{r.right(), r.y}, white, colour});
    data.vertices.push_back({{r.right(), r.bottom()}, white, colour});
    data.vertices.push_back({{r.x, r.bottom()}, white, colour});
    const uint32_t idx[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
    data.indices.insert(data.indices.end(), idx, idx + 6);
}

void DrawList::rect_outline(const Rect &r, uint32_t colour, float t) {
    rect({r.x, r.y, r.w, t}, colour);
    rect({r.x, r.bottom() - t, r.w, t}, colour);
    rect({r.x, r.y + t, t, r.h - t * 2}, colour);
    rect({r.right() - t, r.y + t, t, r.h - t * 2}, colour);
}

void DrawList::rect_rounded(const Rect &r, uint32_t colour, float radius) {
    const float rad = std::min(radius, std::min(r.w, r.h) * 0.5f);
    if (rad <= 0.5f) {
        rect(r, colour);
        return;
    }
    // The middle as one span, then the corners as horizontal slices.
    // A UI's corner radius is three or four pixels; an arc drawn as
    // four spans is exact to within half a pixel and costs four
    // quads instead of thirty triangles.
    rect({r.x, r.y + rad, r.w, r.h - rad * 2}, colour);
    const int steps = std::max(2, int(rad));
    for (int i = 0; i < steps; i++) {
        const float t0 = float(i) / float(steps);
        const float t1 = float(i + 1) / float(steps);
        const float y0 = rad * t0, y1 = rad * t1;
        const float inset0 = rad - std::sqrt(std::max(0.0f, rad * rad -
                                                              (rad - y0) *
                                                                  (rad - y0)));
        const float inset1 = rad - std::sqrt(std::max(0.0f, rad * rad -
                                                              (rad - y1) *
                                                                  (rad - y1)));
        const float inset = std::max(inset0, inset1);
        rect({r.x + inset, r.y + y0, r.w - inset * 2, y1 - y0}, colour);
        rect({r.x + inset, r.bottom() - y1, r.w - inset * 2, y1 - y0}, colour);
    }
}

void DrawList::line(Vec2 a, Vec2 b, uint32_t colour, float thickness) {
    const Vec2 d(b.x - a.x, b.y - a.y);
    const float len = std::sqrt(d.x * d.x + d.y * d.y);
    if (len < 1e-4f) return;
    const Vec2 n(-d.y / len * thickness * 0.5f, d.x / len * thickness * 0.5f);
    const uint32_t base = uint32_t(data.vertices.size());
    const Vec2 white(1.0f, 1.0f);
    data.vertices.push_back({{a.x + n.x, a.y + n.y}, white, colour});
    data.vertices.push_back({{b.x + n.x, b.y + n.y}, white, colour});
    data.vertices.push_back({{b.x - n.x, b.y - n.y}, white, colour});
    data.vertices.push_back({{a.x - n.x, a.y - n.y}, white, colour});
    const uint32_t idx[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
    data.indices.insert(data.indices.end(), idx, idx + 6);
}

void DrawList::triangle(Vec2 a, Vec2 b, Vec2 c, uint32_t colour) {
    const uint32_t base = uint32_t(data.vertices.size());
    const Vec2 white(1.0f, 1.0f);
    data.vertices.push_back({a, white, colour});
    data.vertices.push_back({b, white, colour});
    data.vertices.push_back({c, white, colour});
    data.indices.push_back(base);
    data.indices.push_back(base + 1);
    data.indices.push_back(base + 2);
}

float DrawList::text_width(const std::string &s) const {
    return float(s.size()) * float(kGlyphAdvance) * text_scale;
}

float DrawList::text_height() const {
    return float(kGlyphHeight) * text_scale;
}

size_t DrawList::text_fit(const std::string &s, float width) const {
    const float advance = float(kGlyphAdvance) * text_scale;
    if (advance <= 0.0f) return s.size();
    const size_t n = size_t(std::max(0.0f, width) / advance);
    return std::min(n, s.size());
}

void DrawList::text(Vec2 at, const std::string &s, uint32_t colour) {
    if ((colour >> 24) == 0) return;
    const float dot = text_scale;
    float x = at.x;
    for (char ch : s) {
        const uint8_t *cols = glyph_columns(uint8_t(ch));
        for (int c = 0; c < kGlyphWidth; c++) {
            const uint8_t bits = cols[c];
            if (!bits) continue;
            // Runs of set bits become one rectangle rather than one
            // per dot: a word is a few dozen quads instead of a few
            // hundred.
            int row = 0;
            while (row < kGlyphHeight) {
                if (!(bits & (1u << row))) {
                    row++;
                    continue;
                }
                int run = 0;
                while (row + run < kGlyphHeight && (bits & (1u << (row + run))))
                    run++;
                rect({x + float(c) * dot, at.y + float(row) * dot, dot,
                      float(run) * dot},
                     colour);
                row += run;
            }
        }
        x += float(kGlyphAdvance) * dot;
    }
}

void DrawList::text_in(const Rect &r, const std::string &s, uint32_t colour,
                       TextAlign align, bool vcentre) {
    // Clipped to the box rather than allowed to run out of it: a
    // label that overflows its container and paints over its
    // neighbour is worse than one that is cut off, because the cut
    // one is obviously wrong.
    const float w = text_width(s);
    float x = r.x;
    if (align == TextAlign::Centre) x = r.x + (r.w - w) * 0.5f;
    else if (align == TextAlign::Right) x = r.right() - w;
    const float y = vcentre ? r.y + (r.h - text_height()) * 0.5f : r.y;
    if (w > r.w) {
        push_clip(r);
        text({x, y}, s, colour);
        pop_clip();
    } else {
        text({x, y}, s, colour);
    }
}

}  // namespace wr::ui
