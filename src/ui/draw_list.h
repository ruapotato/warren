// Manifold -- the primitives everything on screen is made of.
//
// Split out from the immediate-mode context because the engine has
// two user interfaces and they are different things on purpose:
//
//   the EDITOR's, which is immediate mode, because a tool's UI is a
//   view of state that changes underneath it and a retained widget
//   tree has to be told about every change;
//
//   the GAME's, which is a tree of Control nodes, because a game's
//   UI is authored, laid out, themed, saved in a scene file and
//   instanced like everything else -- and none of that is possible
//   for a thing that exists only for the duration of a function
//   call.
//
// They share this: rectangles, lines, glyphs, a clip stack, and one
// buffer of triangles that the renderer draws in a single pass.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "core/math/vector.h"

namespace mf::ui {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    Vec2 position() const { return {x, y}; }
    Vec2 size() const { return {w, h}; }
    Vec2 centre() const { return {x + w * 0.5f, y + h * 0.5f}; }
    bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
    bool contains(const Vec2 &p) const { return contains(p.x, p.y); }
    Rect inset(float m) const { return {x + m, y + m, w - m * 2, h - m * 2}; }
    Rect inset(float l, float t, float r, float b) const {
        return {x + l, y + t, w - l - r, h - t - b};
    }
    Rect clipped(const Rect &o) const {
        const float x0 = std::max(x, o.x), y0 = std::max(y, o.y);
        const float x1 = std::min(right(), o.right());
        const float y1 = std::min(bottom(), o.bottom());
        return {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
    bool empty() const { return w <= 0.0f || h <= 0.0f; }
};

struct Vertex {
    Vec2 position;
    Vec2 uv;
    uint32_t colour = 0xFFFFFFFFu;
};

struct DrawCommand {
    Rect clip;
    uint32_t first_index = 0;
    uint32_t index_count = 0;
};

struct DrawData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<DrawCommand> commands;
    void clear() {
        vertices.clear();
        indices.clear();
        commands.clear();
    }
    bool empty() const { return indices.empty(); }
};

// 0xAABBGGRR, which is what the vertex format wants.
constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) |
           (uint32_t(a) << 24);
}
inline uint32_t with_alpha(uint32_t c, float a) {
    const uint32_t alpha = uint32_t(std::clamp(a, 0.0f, 1.0f) * 255.0f);
    return (c & 0x00FFFFFFu) | (alpha << 24);
}

enum class TextAlign { Left, Centre, Right };

class DrawList {
public:
    DrawData data;
    // Device pixels per font dot. 1 is the 5x7 font at its own size.
    float text_scale = 2.0f;

    void begin(float width, float height);
    void end();

    void rect(const Rect &r, uint32_t colour);
    void rect_outline(const Rect &r, uint32_t colour, float thickness = 1.0f);
    // A rectangle with rounded corners, approximated by a stack of
    // spans. Cheap, and at the radii a UI uses indistinguishable
    // from an arc.
    void rect_rounded(const Rect &r, uint32_t colour, float radius);
    void line(Vec2 a, Vec2 b, uint32_t colour, float thickness = 1.0f);
    void triangle(Vec2 a, Vec2 b, Vec2 c, uint32_t colour);
    void text(Vec2 at, const std::string &s, uint32_t colour);
    void text_in(const Rect &r, const std::string &s, uint32_t colour,
                 TextAlign align, bool vcentre = true);

    float text_width(const std::string &s) const;
    float text_height() const;
    // How many characters of `s` fit in `width`.
    size_t text_fit(const std::string &s, float width) const;

    void push_clip(const Rect &r);
    void pop_clip();
    const Rect &clip() const { return clip_; }

private:
    void flush();
    Rect clip_{0, 0, 0, 0};
    std::vector<Rect> clip_stack_;
    uint32_t command_start_ = 0;
    float width_ = 0, height_ = 0;
};

}  // namespace mf::ui
