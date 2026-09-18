// Warren -- the editor's user interface.
//
// IMMEDIATE MODE, because an editor's UI is a view of state that
// changes underneath it. A retained tree of widgets has to be told
// when a node was added, renamed, deleted or re-parented, and every
// one of those messages is a chance to be out of date; a UI that is
// rebuilt from the scene every frame cannot be.
//
// The whole thing produces one list of coloured, clipped triangles
// and one list of glyph quads, which the RHI draws in a single pass
// with one pipeline and one texture. No dependency: a font, a draw
// list and a few hundred lines of widget code, because an editor
// that cannot be built without fetching a UI library is an editor
// that does not build.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "core/math/vector.h"
#include "ui/draw_list.h"

namespace wr::ui {

struct Theme {
    uint32_t window = rgba(28, 30, 34, 242);
    uint32_t window_title = rgba(38, 42, 48, 255);
    uint32_t panel = rgba(22, 24, 27, 255);
    uint32_t text = rgba(222, 226, 232, 255);
    uint32_t text_dim = rgba(140, 146, 156, 255);
    uint32_t accent = rgba(86, 156, 214, 255);
    uint32_t accent_dim = rgba(56, 96, 134, 255);
    uint32_t control = rgba(48, 52, 58, 255);
    uint32_t control_hot = rgba(64, 70, 78, 255);
    uint32_t control_active = rgba(86, 156, 214, 255);
    uint32_t border = rgba(60, 64, 72, 255);
    uint32_t warning = rgba(220, 170, 90, 255);
    uint32_t error = rgba(224, 108, 108, 255);
    float padding = 6.0f;
    float spacing = 4.0f;
    float row_height = 20.0f;
    float indent = 14.0f;
    float scale = 2.0f;     // device pixels per font dot
};

// What the platform hands in each frame.
struct Input {
    float mouse_x = 0, mouse_y = 0;
    bool mouse_down = false;
    bool mouse_right = false;
    float wheel = 0.0f;
    // Characters typed this frame, already translated by the OS.
    std::string text;
    bool key_backspace = false, key_enter = false, key_escape = false;
    bool key_tab = false, key_left = false, key_right = false;
    bool key_delete = false;
    bool ctrl = false, shift = false, alt = false;
};

// An identifier for a widget, derived from its label and the window
// it is in. Stable across frames as long as the label is, which is
// what lets "which control is being dragged" survive a rebuild.
using Id = uint64_t;

class Context {
public:
    Theme theme;

    void begin_frame(float width, float height, const Input &input, float dt);
    void end_frame();
    const DrawData &draw_data() const { return draw_.data; }
    DrawList &draw_list() { return draw_; }
    // True when the pointer is over any window, so the game should
    // not also act on the click.
    bool wants_mouse() const { return wants_mouse_; }
    bool wants_keyboard() const { return active_text_ != 0; }

    // --- windows ------------------------------------------------------
    // A panel with a title bar, at a position the user can drag.
    // Returns false when collapsed, in which case skip its contents.
    bool begin_window(const char *title, Rect initial);
    void end_window();

    // --- layout -------------------------------------------------------
    void same_line(float spacing = -1.0f);
    void spacing(float h = -1.0f);
    void separator();
    void indent(float amount = -1.0f);
    void unindent(float amount = -1.0f);
    // Force the next widget's width; -1 goes back to filling.
    void set_next_width(float w) { next_width_ = w; }

    // --- widgets ------------------------------------------------------
    void text(const char *fmt, ...);
    void text_coloured(uint32_t colour, const char *fmt, ...);
    bool button(const char *label);
    bool checkbox(const char *label, bool *value);
    bool slider(const char *label, float *value, float low, float high);
    bool drag_float(const char *label, float *value, float step = 0.01f);
    bool input_text(const char *label, std::string *value);
    // A collapsing header. Returns true when open.
    bool tree_node(const char *label, bool default_open = false);
    void tree_pop();
    // A selectable row; returns true on click.
    bool selectable(const char *label, bool selected);
    void progress(float fraction, const char *overlay = nullptr);

    // --- raw drawing, for a viewport overlay or a gizmo ----------------
    void rect(const Rect &r, uint32_t colour);
    void rect_outline(const Rect &r, uint32_t colour, float thickness = 1.0f);
    void line(Vec2 a, Vec2 b, uint32_t colour, float thickness = 1.0f);
    void draw_text(Vec2 at, const char *s, uint32_t colour);
    float text_width(const char *s) const;
    float text_height() const;
    void push_clip(const Rect &r);
    void pop_clip();

    // Where the next widget will go, for a caller doing its own
    // layout inside a window.
    Rect content_rect();
    Vec2 cursor() const { return {cursor_x_, cursor_y_}; }
    void set_cursor(Vec2 p) {
        cursor_x_ = p.x;
        cursor_y_ = p.y;
    }

    Id id_of(const char *label) const;

private:
    struct Window {
        Id id = 0;
        std::string title;
        Rect rect;
        bool open = true;
        bool collapsed = false;
        float scroll = 0.0f;
        float content_height = 0.0f;
    };

    Window *current();
    Rect next_row(float height);
    bool mouse_in(const Rect &r) const;
    void push_rect(const Rect &r, uint32_t colour);

    // The same primitives the Control tree draws with. Two user
    // interfaces, one triangle buffer, one renderer.
    DrawList draw_;
    std::vector<Window> windows_;
    int current_window_ = -1;

    Input input_;
    Input previous_;
    float width_ = 0, height_ = 0, dt_ = 0;
    float cursor_x_ = 0, cursor_y_ = 0, line_start_x_ = 0;
    float row_max_height_ = 0;
    float indent_ = 0;
    float next_width_ = -1.0f;
    bool same_line_ = false;

    Id hot_ = 0, active_ = 0, active_text_ = 0;
    Id dragging_window_ = 0;
    Vec2 drag_offset_{0, 0};
    std::string text_buffer_;
    int text_cursor_ = 0;
    bool wants_mouse_ = false;
    std::vector<std::pair<Id, bool>> tree_open_;
};

}  // namespace wr::ui
