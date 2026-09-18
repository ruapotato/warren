#include "ui/ui.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "ui/font.h"

namespace mf::ui {
namespace {

// FNV-1a over the label. Two widgets with the same label in the same
// window are the same widget as far as "which one is being dragged"
// is concerned, which is the immediate-mode bargain: labels are
// identity, so a list of identical buttons needs "Delete##3".
Id hash_label(const char *s, Id seed) {
    Id h = seed ? seed : 1469598103934665603ull;
    for (const char *p = s; *p; p++) {
        h ^= uint8_t(*p);
        h *= 1099511628211ull;
    }
    return h ? h : 1;
}

// The part of a label after "##" is identity only, not text.
const char *visible_end(const char *s) {
    const char *hash = std::strstr(s, "##");
    return hash ? hash : s + std::strlen(s);
}

}  // namespace

Id Context::id_of(const char *label) const {
    const Id seed = current_window_ >= 0
                        ? windows_[size_t(current_window_)].id
                        : 0xcbf29ce484222325ull;
    return hash_label(label, seed);
}

Context::Window *Context::current() {
    return current_window_ >= 0 ? &windows_[size_t(current_window_)] : nullptr;
}

bool Context::mouse_in(const Rect &r) const {
    return r.clipped(draw_.clip()).contains(input_.mouse_x, input_.mouse_y);
}

// ------------------------------------------------------------ the frame

void Context::begin_frame(float width, float height, const Input &input,
                          float dt) {
    previous_ = input_;
    input_ = input;
    width_ = width;
    height_ = height;
    dt_ = dt;
    draw_.begin(width, height);
    draw_.text_scale = theme.scale;
    current_window_ = -1;
    hot_ = 0;
    wants_mouse_ = false;
    if (!input_.mouse_down) dragging_window_ = 0;
    if (input_.key_escape) active_text_ = 0;
}

void Context::end_frame() {
    draw_.end();
    // THE ACTIVE WIDGET IS RELEASED AT THE END, NOT THE START.
    //
    // A click is the release over the control that was pressed --
    // that is what lets a user change their mind by dragging off a
    // button. Clearing `active` when the frame opens means the
    // widget never sees the frame in which the button came up, so it
    // can never report a click at all. Clearing it here, after every
    // widget has had its look, is the difference between a UI and a
    // picture of one.
    if (!input_.mouse_down) active_ = 0;
}

// ------------------------------------------------------------- drawing
//
// All of it is DrawList's now, shared with the Control tree. These
// stay as one-liners because the widget code below reads better
// saying ui.rect(...) than ui.draw_list().rect(...).

void Context::push_rect(const Rect &r, uint32_t colour) {
    draw_.rect(r, colour);
}

void Context::rect(const Rect &r, uint32_t colour) { draw_.rect(r, colour); }

void Context::rect_outline(const Rect &r, uint32_t colour, float t) {
    draw_.rect_outline(r, colour, t);
}

void Context::line(Vec2 a, Vec2 b, uint32_t colour, float thickness) {
    draw_.line(a, b, colour, thickness);
}

float Context::text_width(const char *s) const {
    return s ? draw_.text_width(std::string(s, visible_end(s))) : 0.0f;
}

float Context::text_height() const { return draw_.text_height(); }

void Context::draw_text(Vec2 at, const char *s, uint32_t colour) {
    if (s) draw_.text(at, std::string(s, visible_end(s)), colour);
}

void Context::push_clip(const Rect &r) { draw_.push_clip(r); }
void Context::pop_clip() { draw_.pop_clip(); }

// ------------------------------------------------------------- windows

bool Context::begin_window(const char *title, Rect initial) {
    const Id id = hash_label(title, 0xcbf29ce484222325ull);
    int index = -1;
    for (size_t i = 0; i < windows_.size(); i++)
        if (windows_[i].id == id) index = int(i);
    if (index < 0) {
        Window w;
        w.id = id;
        w.title = title;
        w.rect = initial;
        windows_.push_back(w);
        index = int(windows_.size()) - 1;
    }
    current_window_ = index;
    Window &w = windows_[size_t(index)];

    const float title_h = theme.row_height + 4.0f;
    const Rect bar{w.rect.x, w.rect.y, w.rect.w, title_h};

    // Dragging the title bar moves the window. Held by id, so the
    // window keeps moving even when the pointer runs off it.
    if (dragging_window_ == id) {
        w.rect.x = input_.mouse_x - drag_offset_.x;
        w.rect.y = input_.mouse_y - drag_offset_.y;
    } else if (input_.mouse_down && !previous_.mouse_down && !active_ &&
               bar.contains(input_.mouse_x, input_.mouse_y)) {
        dragging_window_ = id;
        drag_offset_ = {input_.mouse_x - w.rect.x, input_.mouse_y - w.rect.y};
    }
    // Kept on screen: a window dragged off the edge is a window the
    // user cannot get back.
    w.rect.x = std::max(-w.rect.w + 40.0f, std::min(w.rect.x, width_ - 40.0f));
    w.rect.y = std::max(0.0f, std::min(w.rect.y, height_ - title_h));

    if (w.rect.contains(input_.mouse_x, input_.mouse_y)) wants_mouse_ = true;

    const Rect body{w.rect.x, w.rect.y + title_h, w.rect.w,
                    w.collapsed ? 0.0f : w.rect.h - title_h};
    if (!w.collapsed) push_rect(body, theme.window);
    push_rect(bar, theme.window_title);
    rect_outline({w.rect.x, w.rect.y, w.rect.w,
                  w.collapsed ? title_h : w.rect.h},
                 theme.border);

    // The triangle that collapses it.
    const float tri = 8.0f;
    const Rect toggle{bar.x + 4.0f, bar.y + (title_h - tri) * 0.5f, tri, tri};
    const bool over_toggle = toggle.contains(input_.mouse_x, input_.mouse_y);
    push_rect(toggle.inset(w.collapsed ? 1.0f : 2.0f),
              over_toggle ? theme.accent : theme.text_dim);
    if (over_toggle && input_.mouse_down && !previous_.mouse_down)
        w.collapsed = !w.collapsed;

    draw_text({bar.x + 4.0f + tri + 6.0f,
               bar.y + (title_h - text_height()) * 0.5f},
              title, theme.text);

    if (w.collapsed) {
        current_window_ = index;
        return false;
    }

    // Scrolling, when the content was taller than the window last
    // frame. Measured rather than declared, which is the only way an
    // immediate-mode panel can know.
    const float view_h = body.h - theme.padding * 2.0f;
    if (w.content_height > view_h && mouse_in(body))
        w.scroll -= input_.wheel * 32.0f;
    w.scroll = std::max(0.0f, std::min(w.scroll,
                                       std::max(0.0f, w.content_height - view_h)));

    push_clip(body);
    cursor_x_ = line_start_x_ = body.x + theme.padding;
    cursor_y_ = body.y + theme.padding - w.scroll;
    indent_ = 0.0f;
    row_max_height_ = 0.0f;
    same_line_ = false;
    next_width_ = -1.0f;
    w.content_height = 0.0f;
    return true;
}

void Context::end_window() {
    Window *w = current();
    if (w && !w->collapsed) {
        const float title_h = theme.row_height + 4.0f;
        const float top = w->rect.y + title_h + theme.padding - w->scroll;
        w->content_height = cursor_y_ - top;

        // A scrollbar, but only when there is something to scroll.
        const float view_h = w->rect.h - title_h - theme.padding * 2.0f;
        if (w->content_height > view_h && view_h > 0.0f) {
            const float frac = view_h / w->content_height;
            const float bar_h = std::max(20.0f, view_h * frac);
            const float travel = view_h - bar_h;
            const float at = w->content_height > view_h
                                 ? w->scroll / (w->content_height - view_h)
                                 : 0.0f;
            const Rect bar{w->rect.right() - 6.0f,
                           w->rect.y + title_h + theme.padding + travel * at,
                           4.0f, bar_h};
            push_rect(bar, theme.control_hot);
        }
        pop_clip();
    }
    current_window_ = -1;
}

// -------------------------------------------------------------- layout

Rect Context::content_rect() {
    Window *w = current();
    if (!w) return {0, 0, width_, height_};
    const float title_h = theme.row_height + 4.0f;
    return {w->rect.x + theme.padding, w->rect.y + title_h + theme.padding,
            w->rect.w - theme.padding * 2.0f,
            w->rect.h - title_h - theme.padding * 2.0f};
}

Rect Context::next_row(float height) {
    const Rect content = content_rect();
    float width = next_width_ > 0.0f ? next_width_
                                     : content.right() - cursor_x_ -
                                           (indent_ > 0 ? 0.0f : 0.0f);
    if (width < 8.0f) width = 8.0f;
    const Rect r{cursor_x_, cursor_y_, width, height};
    if (same_line_) {
        cursor_x_ += width + theme.spacing;
        row_max_height_ = std::max(row_max_height_, height);
        same_line_ = false;
    } else {
        cursor_y_ += std::max(height, row_max_height_) + theme.spacing;
        cursor_x_ = line_start_x_ + indent_;
        row_max_height_ = 0.0f;
    }
    next_width_ = -1.0f;
    return r;
}

void Context::same_line(float spacing) {
    same_line_ = true;
    if (spacing > 0.0f) cursor_x_ += spacing;
}

void Context::spacing(float h) {
    cursor_y_ += h > 0.0f ? h : theme.spacing * 2.0f;
}

void Context::separator() {
    const Rect content = content_rect();
    cursor_y_ += theme.spacing;
    push_rect({content.x, cursor_y_, content.w, 1.0f}, theme.border);
    cursor_y_ += 1.0f + theme.spacing;
    cursor_x_ = line_start_x_ + indent_;
}

void Context::indent(float amount) {
    indent_ += amount > 0.0f ? amount : theme.indent;
    cursor_x_ = line_start_x_ + indent_;
}

void Context::unindent(float amount) {
    indent_ = std::max(0.0f, indent_ - (amount > 0.0f ? amount : theme.indent));
    cursor_x_ = line_start_x_ + indent_;
}

// ------------------------------------------------------------- widgets

void Context::text(const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    const Rect r = next_row(text_height());
    draw_text({r.x, r.y}, buf, theme.text);
}

void Context::text_coloured(uint32_t colour, const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    const Rect r = next_row(text_height());
    draw_text({r.x, r.y}, buf, colour);
}

bool Context::button(const char *label) {
    const Id id = id_of(label);
    const float w = text_width(label) + theme.padding * 4.0f;
    if (next_width_ <= 0.0f) next_width_ = w;
    const Rect r = next_row(theme.row_height);
    const bool over = mouse_in(r);
    if (over) hot_ = id;
    bool clicked = false;
    if (over && input_.mouse_down && !previous_.mouse_down) active_ = id;
    if (active_ == id && !input_.mouse_down) {
        clicked = over;
        active_ = 0;
    }
    push_rect(r, active_ == id   ? theme.control_active
              : over             ? theme.control_hot
                                 : theme.control);
    draw_text({r.x + (r.w - text_width(label)) * 0.5f,
               r.y + (r.h - text_height()) * 0.5f},
              label, theme.text);
    return clicked;
}

bool Context::checkbox(const char *label, bool *value) {
    const Id id = id_of(label);
    const Rect r = next_row(theme.row_height);
    const Rect box{r.x, r.y + (r.h - 14.0f) * 0.5f, 14.0f, 14.0f};
    const bool over = mouse_in(r);
    if (over) hot_ = id;
    bool changed = false;
    if (over && input_.mouse_down && !previous_.mouse_down && value) {
        *value = !*value;
        changed = true;
    }
    push_rect(box, over ? theme.control_hot : theme.control);
    if (value && *value) push_rect(box.inset(3.0f), theme.accent);
    rect_outline(box, theme.border);
    draw_text({box.right() + 6.0f, r.y + (r.h - text_height()) * 0.5f}, label,
              theme.text);
    return changed;
}

bool Context::slider(const char *label, float *value, float low, float high) {
    const Id id = id_of(label);
    const Rect r = next_row(theme.row_height);
    const bool over = mouse_in(r);
    if (over) hot_ = id;
    if (over && input_.mouse_down && !previous_.mouse_down) active_ = id;
    bool changed = false;
    if (active_ == id && value && high > low) {
        const float t = (input_.mouse_x - r.x) / std::max(r.w, 1.0f);
        const float v = low + (high - low) * std::max(0.0f, std::min(1.0f, t));
        if (v != *value) {
            *value = v;
            changed = true;
        }
    }
    push_rect(r, theme.control);
    const float t = value && high > low
                        ? std::max(0.0f, std::min(1.0f, (*value - low) / (high - low)))
                        : 0.0f;
    push_rect({r.x, r.y, r.w * t, r.h},
              active_ == id ? theme.control_active : theme.accent_dim);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.*s  %.3f", int(visible_end(label) - label),
                  label, value ? double(*value) : 0.0);
    draw_text({r.x + 4.0f, r.y + (r.h - text_height()) * 0.5f}, buf, theme.text);
    rect_outline(r, theme.border);
    return changed;
}

bool Context::drag_float(const char *label, float *value, float step) {
    const Id id = id_of(label);
    const Rect r = next_row(theme.row_height);
    const bool over = mouse_in(r);
    if (over) hot_ = id;
    if (over && input_.mouse_down && !previous_.mouse_down) active_ = id;
    bool changed = false;
    if (active_ == id && value) {
        const float dx = input_.mouse_x - previous_.mouse_x;
        if (dx != 0.0f) {
            *value += dx * step;
            changed = true;
        }
    }
    push_rect(r, active_ == id ? theme.control_active
              : over          ? theme.control_hot
                              : theme.control);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.*s  %.3f", int(visible_end(label) - label),
                  label, value ? double(*value) : 0.0);
    draw_text({r.x + 4.0f, r.y + (r.h - text_height()) * 0.5f}, buf, theme.text);
    rect_outline(r, theme.border);
    return changed;
}

bool Context::input_text(const char *label, std::string *value) {
    const Id id = id_of(label);
    const Rect r = next_row(theme.row_height);
    const bool over = mouse_in(r);
    if (over) hot_ = id;
    if (input_.mouse_down && !previous_.mouse_down) {
        if (over) {
            active_text_ = id;
            text_buffer_ = value ? *value : std::string();
            text_cursor_ = int(text_buffer_.size());
        } else if (active_text_ == id) {
            if (value) *value = text_buffer_;
            active_text_ = 0;
        }
    }

    bool committed = false;
    if (active_text_ == id) {
        for (char c : input_.text)
            if (c >= 32 && c < 127) {
                text_buffer_.insert(text_buffer_.begin() + text_cursor_, c);
                text_cursor_++;
            }
        if (input_.key_backspace && text_cursor_ > 0) {
            text_buffer_.erase(text_buffer_.begin() + (text_cursor_ - 1));
            text_cursor_--;
        }
        if (input_.key_left && text_cursor_ > 0) text_cursor_--;
        if (input_.key_right && text_cursor_ < int(text_buffer_.size()))
            text_cursor_++;
        if (input_.key_enter) {
            if (value) *value = text_buffer_;
            active_text_ = 0;
            committed = true;
        }
    }

    push_rect(r, active_text_ == id ? theme.panel : theme.control);
    rect_outline(r, active_text_ == id ? theme.accent : theme.border);
    const std::string &shown =
        active_text_ == id ? text_buffer_ : (value ? *value : std::string());
    const float ty = r.y + (r.h - text_height()) * 0.5f;
    push_clip(r.inset(2.0f));
    draw_text({r.x + 4.0f, ty}, shown.c_str(), theme.text);
    if (active_text_ == id) {
        const float cx = r.x + 4.0f + float(text_cursor_) *
                                          float(kGlyphAdvance) * theme.scale;
        push_rect({cx, ty, 1.0f * theme.scale, text_height()}, theme.accent);
    }
    pop_clip();
    // NO LABEL DRAWN. A text field wants its whole row for the text
    // -- a name half hidden behind a caption is worse than one with
    // the caption above it -- so the caller puts a text() in front
    // of it. The label is identity only, which is what the "##"
    // convention is for.
    return committed;
}

bool Context::tree_node(const char *label, bool default_open) {
    const Id id = id_of(label);
    bool *state = nullptr;
    for (auto &kv : tree_open_)
        if (kv.first == id) state = &kv.second;
    if (!state) {
        tree_open_.emplace_back(id, default_open);
        state = &tree_open_.back().second;
    }
    const Rect r = next_row(theme.row_height);
    const bool over = mouse_in(r);
    if (over) hot_ = id;
    if (over && input_.mouse_down && !previous_.mouse_down) *state = !*state;
    if (over) push_rect(r, theme.control);

    const float tri = 8.0f;
    const Rect arrow{r.x + 2.0f, r.y + (r.h - tri) * 0.5f, tri, tri};
    push_rect(arrow.inset(*state ? 1.0f : 2.5f),
              over ? theme.accent : theme.text_dim);
    draw_text({arrow.right() + 6.0f, r.y + (r.h - text_height()) * 0.5f}, label,
              theme.text);
    if (*state) indent();
    return *state;
}

void Context::tree_pop() { unindent(); }

bool Context::selectable(const char *label, bool selected) {
    const Id id = id_of(label);
    const Rect r = next_row(theme.row_height);
    const bool over = mouse_in(r);
    if (over) hot_ = id;
    const bool clicked = over && input_.mouse_down && !previous_.mouse_down;
    if (selected)
        push_rect(r, theme.accent_dim);
    else if (over)
        push_rect(r, theme.control);
    draw_text({r.x + 4.0f, r.y + (r.h - text_height()) * 0.5f}, label,
              selected ? theme.text : theme.text);
    return clicked;
}

void Context::progress(float fraction, const char *overlay) {
    const Rect r = next_row(theme.row_height);
    push_rect(r, theme.control);
    const float t = std::max(0.0f, std::min(1.0f, fraction));
    push_rect({r.x, r.y, r.w * t, r.h}, theme.accent_dim);
    if (overlay)
        draw_text({r.x + (r.w - text_width(overlay)) * 0.5f,
                   r.y + (r.h - text_height()) * 0.5f},
                  overlay, theme.text);
    rect_outline(r, theme.border);
}

}  // namespace mf::ui
