#include "scene/control.h"

#include <algorithm>

#include "core/bind.h"
#include "core/log.h"
#include "scene/scene_tree.h"

namespace mf {
namespace {
Control *g_focused = nullptr;
}

// ---------------------------------------------------------------- theme

const Theme &Theme::fallback() {
    static Theme t;
    return t;
}

const Theme &Control::theme() const {
    for (const Node *n = this; n; n = n->parent())
        if (const ThemeProvider *p = n->cast_to<ThemeProvider>())
            if (p->theme_resource) return *p->theme_resource;
    return Theme::fallback();
}

// -------------------------------------------------------------- layout

void Control::set_anchors_preset(Preset p, bool keep_size) {
    const Vec2 size = keep_size ? Vec2(offset_right - offset_left,
                                       offset_bottom - offset_top)
                                : Vec2(0, 0);
    auto set = [&](float l, float t, float r, float b) {
        anchor_left = l;
        anchor_top = t;
        anchor_right = r;
        anchor_bottom = b;
    };
    switch (p) {
        case Preset::TopLeft: set(0, 0, 0, 0); break;
        case Preset::TopRight: set(1, 0, 1, 0); break;
        case Preset::BottomLeft: set(0, 1, 0, 1); break;
        case Preset::BottomRight: set(1, 1, 1, 1); break;
        case Preset::Centre: set(0.5f, 0.5f, 0.5f, 0.5f); break;
        case Preset::LeftWide: set(0, 0, 0, 1); break;
        case Preset::RightWide: set(1, 0, 1, 1); break;
        case Preset::TopWide: set(0, 0, 1, 0); break;
        case Preset::BottomWide: set(0, 1, 1, 1); break;
        case Preset::FullRect: set(0, 0, 1, 1); break;
    }
    if (p == Preset::FullRect || p == Preset::TopWide ||
        p == Preset::BottomWide || p == Preset::LeftWide ||
        p == Preset::RightWide) {
        // A stretching preset wants offsets of zero, or it stretches
        // to the parent and then adds a hundred pixels.
        offset_left = offset_top = offset_right = offset_bottom = 0.0f;
        if (p == Preset::TopWide) offset_bottom = size.y;
        if (p == Preset::BottomWide) offset_top = -size.y;
        if (p == Preset::LeftWide) offset_right = size.x;
        if (p == Preset::RightWide) offset_left = -size.x;
        return;
    }
    if (keep_size) {
        if (p == Preset::Centre) {
            offset_left = -size.x * 0.5f;
            offset_top = -size.y * 0.5f;
        } else {
            offset_left = (p == Preset::TopRight || p == Preset::BottomRight)
                              ? -size.x
                              : 0.0f;
            offset_top = (p == Preset::BottomLeft || p == Preset::BottomRight)
                             ? -size.y
                             : 0.0f;
        }
        offset_right = offset_left + size.x;
        offset_bottom = offset_top + size.y;
    }
}

Vec2 Control::combined_minimum_size() const {
    const Vec2 own = minimum_size();
    return {std::max(own.x, custom_minimum_size.x),
            std::max(own.y, custom_minimum_size.y)};
}

void Control::update_layout(const ui::Rect &parent) {
    // THE FOUR ANCHORS AND FOUR OFFSETS, and this is all they mean.
    const float l = parent.x + parent.w * anchor_left + offset_left;
    const float t = parent.y + parent.h * anchor_top + offset_top;
    const float r = parent.x + parent.w * anchor_right + offset_right;
    const float b = parent.y + parent.h * anchor_bottom + offset_bottom;
    const Vec2 minimum = combined_minimum_size();
    rect_ = {l, t, std::max(r - l, minimum.x), std::max(b - t, minimum.y)};
    layout_children();
}

void Control::layout_children() {
    // A plain Control is not a container: each child places itself
    // against this one's rectangle using its own anchors.
    for (const auto &c : children())
        if (Control *child = c ? c->cast_to<Control>() : nullptr)
            child->update_layout(rect_);
}

// --------------------------------------------------------------- focus

void Control::grab_focus() {
    if (focus_mode == FocusMode::None) return;
    if (g_focused == this) return;
    if (g_focused) {
        g_focused->_set_focused(false);
        g_focused->focus_exited();
    }
    g_focused = this;
    focused_ = true;
    focus_entered();
}

void Control::on_exit_tree() {
    release_focus();
}

void Control::release_focus() {
    if (g_focused != this) return;
    g_focused = nullptr;
    focused_ = false;
    focus_exited();
}

Control *ui_focused_control() { return g_focused; }
void ui_set_focused_control(Control *c) {
    if (g_focused == c) return;
    if (g_focused) {
        g_focused->_set_focused(false);
        g_focused->focus_exited();
    }
    g_focused = c;
    if (c) {
        c->_set_focused(true);
        c->focus_entered();
    }
}

// ----------------------------------------------------------- reflection

static void register_control() {
    ClassBuilder<Theme>()
        .field("background", &Theme::background)
        .field("panel", &Theme::panel)
        .field("text", &Theme::text)
        .field("text_dim", &Theme::text_dim)
        .field("accent", &Theme::accent)
        .field("control", &Theme::control)
        .field("control_hover", &Theme::control_hover)
        .field("control_pressed", &Theme::control_pressed)
        .field("border", &Theme::border)
        .field("corner_radius", &Theme::corner_radius, "range:0,16")
        .field("padding", &Theme::padding, "range:0,32")
        .field("separation", &Theme::separation, "range:0,32")
        .field("font_scale", &Theme::font_scale, "range:1,6")
        .field("border_width", &Theme::border_width, "range:0,8");

    ClassBuilder<Control>()
        .field("anchor_left", &Control::anchor_left, "range:0,1")
        .field("anchor_top", &Control::anchor_top, "range:0,1")
        .field("anchor_right", &Control::anchor_right, "range:0,1")
        .field("anchor_bottom", &Control::anchor_bottom, "range:0,1")
        .field("offset_left", &Control::offset_left)
        .field("offset_top", &Control::offset_top)
        .field("offset_right", &Control::offset_right)
        .field("offset_bottom", &Control::offset_bottom)
        .field("size_flags_horizontal", &Control::size_flags_horizontal)
        .field("size_flags_vertical", &Control::size_flags_vertical)
        .field("stretch_ratio", &Control::stretch_ratio, "range:0,8")
        .field("custom_minimum_size", &Control::custom_minimum_size)
        .field("visible", &Control::visible)
        .field("opacity", &Control::opacity, "range:0,1")
        .field("clip_contents", &Control::clip_contents)
        .method("grab_focus", &Control::grab_focus)
        .method("release_focus", &Control::release_focus)
        .method("has_focus", &Control::has_focus)
        .method("is_hovered", &Control::hovered)
        .signal("resized")
        .signal("mouse_entered")
        .signal("mouse_exited");

    ClassBuilder<ThemeProvider>();
}
MF_REGISTER(register_control)

}  // namespace mf
