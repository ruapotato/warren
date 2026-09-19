#include "scene/controls.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/bind.h"
#include "core/log.h"

namespace wr {
namespace {

// PACKED AS sRGB BYTES, BECAUSE THAT IS WHAT EIGHT BITS ARE FOR.
//
// A Color is linear -- Color::hex converts on the way in -- and
// quantising a linear value to a byte spends almost all of its
// precision on highlights nobody looks at: the theme's panel colour,
// 0x262A30, becomes 5/255 and every dark shade in the interface
// collapses onto the same few levels.
//
// So the vertex carries the sRGB encoding, the shader decodes it,
// and the sRGB swapchain encodes it again on write. The original
// version skipped the encode here AND decoded in the shader, which
// cancelled out to the right colour on screen -- and hid the fact
// that the whole dark end of the palette was being stored in three
// bits.
uint32_t to_rgba(const Color &c, float opacity = 1.0f) {
    auto encode = [](float v) {
        v = std::clamp(v, 0.0f, 1.0f);
        const float s = v <= 0.0031308f ? v * 12.92f
                                        : 1.055f * std::pow(v, 1.0f / 2.4f) -
                                              0.055f;
        return uint8_t(s * 255.0f + 0.5f);
    };
    // Alpha is not a colour and is never encoded.
    const uint8_t a = uint8_t(std::clamp(c.a * opacity, 0.0f, 1.0f) * 255.0f +
                              0.5f);
    return ui::rgba(encode(c.r), encode(c.g), encode(c.b), a);
}

}  // namespace

// --------------------------------------------------------- containers

std::vector<Control *> Container::laid_out() const {
    std::vector<Control *> out;
    for (const auto &c : children())
        if (Control *ctrl = c ? c->cast_to<Control>() : nullptr)
            if (ctrl->visible) out.push_back(ctrl);
    return out;
}

void Container::place(Control *c, const ui::Rect &slot) {
    ui::Rect r = slot;
    const Vec2 minimum = c->combined_minimum_size();
    // SHRINK FLAGS ARE HONOURED INSIDE THE SLOT, not instead of it.
    // The container decided how much room this child gets; the
    // flags decide where in that room it sits.
    if (!(c->size_flags_horizontal & Control::SizeFill)) {
        const float w = std::min(slot.w, std::max(minimum.x, 1.0f));
        if (c->size_flags_horizontal & Control::SizeShrinkCentre)
            r.x = slot.x + (slot.w - w) * 0.5f;
        else if (c->size_flags_horizontal & Control::SizeShrinkEnd)
            r.x = slot.right() - w;
        r.w = w;
    }
    if (!(c->size_flags_vertical & Control::SizeFill)) {
        const float h = std::min(slot.h, std::max(minimum.y, 1.0f));
        if (c->size_flags_vertical & Control::SizeShrinkCentre)
            r.y = slot.y + (slot.h - h) * 0.5f;
        else if (c->size_flags_vertical & Control::SizeShrinkEnd)
            r.y = slot.bottom() - h;
        r.h = h;
    }
    c->_set_rect(r);
    // A CONTAINER OVERRIDES ITS CHILDREN'S ANCHORS. The child does
    // not get to place itself -- that is what putting it in a box
    // means -- so its own layout is skipped and only its children
    // are recursed into, against the rectangle it was given.
    c->layout_children();
}

Vec2 Container::minimum_size() const {
    Vec2 out{0, 0};
    for (Control *c : laid_out()) {
        const Vec2 m = c->combined_minimum_size();
        out.x = std::max(out.x, m.x);
        out.y = std::max(out.y, m.y);
    }
    return out;
}

void Container::layout_children() { arrange(laid_out()); }

// --- box

float BoxContainer::gap() const {
    return separation >= 0.0f ? separation : theme().separation;
}

Vec2 BoxContainer::minimum_size() const {
    const std::vector<Control *> kids = laid_out();
    Vec2 out{0, 0};
    for (Control *c : kids) {
        const Vec2 m = c->combined_minimum_size();
        if (vertical) {
            out.x = std::max(out.x, m.x);
            out.y += m.y;
        } else {
            out.x += m.x;
            out.y = std::max(out.y, m.y);
        }
    }
    if (kids.size() > 1) {
        const float total = gap() * float(kids.size() - 1);
        (vertical ? out.y : out.x) += total;
    }
    return out;
}

void BoxContainer::arrange(const std::vector<Control *> &kids) {
    if (kids.empty()) return;
    const float sep = gap();
    const float along = vertical ? rect_.h : rect_.w;
    const float across = vertical ? rect_.w : rect_.h;

    // What everyone needs, then what is left over for those that
    // asked to expand.
    float needed = sep * float(kids.size() - 1);
    float weight = 0.0f;
    for (Control *c : kids) {
        const Vec2 m = c->combined_minimum_size();
        needed += vertical ? m.y : m.x;
        const int flags = vertical ? c->size_flags_vertical
                                   : c->size_flags_horizontal;
        if (flags & SizeExpand) weight += std::max(0.0f, c->stretch_ratio);
    }
    const float spare = std::max(0.0f, along - needed);

    float cursor = vertical ? rect_.y : rect_.x;
    for (Control *c : kids) {
        const Vec2 m = c->combined_minimum_size();
        float size = vertical ? m.y : m.x;
        const int flags = vertical ? c->size_flags_vertical
                                   : c->size_flags_horizontal;
        if ((flags & SizeExpand) && weight > 0.0f)
            size += spare * (std::max(0.0f, c->stretch_ratio) / weight);
        const ui::Rect slot = vertical
                                  ? ui::Rect{rect_.x, cursor, across, size}
                                  : ui::Rect{cursor, rect_.y, size, across};
        place(c, slot);
        cursor += size + sep;
    }
}

// --- grid

Vec2 GridContainer::minimum_size() const {
    const std::vector<Control *> kids = laid_out();
    const int cols = std::max(1, columns);
    const float sep = separation >= 0.0f ? separation : theme().separation;
    std::vector<float> col_width(size_t(cols), 0.0f);
    std::vector<float> row_height;
    for (size_t i = 0; i < kids.size(); i++) {
        const Vec2 m = kids[i]->combined_minimum_size();
        const size_t col = i % size_t(cols);
        const size_t row = i / size_t(cols);
        if (row >= row_height.size()) row_height.push_back(0.0f);
        col_width[col] = std::max(col_width[col], m.x);
        row_height[row] = std::max(row_height[row], m.y);
    }
    Vec2 out{0, 0};
    for (float w : col_width) out.x += w;
    for (float h : row_height) out.y += h;
    if (cols > 1) out.x += sep * float(cols - 1);
    if (row_height.size() > 1) out.y += sep * float(row_height.size() - 1);
    return out;
}

void GridContainer::arrange(const std::vector<Control *> &kids) {
    if (kids.empty()) return;
    const int cols = std::max(1, columns);
    const int rows = int((kids.size() + size_t(cols) - 1) / size_t(cols));
    const float sep = separation >= 0.0f ? separation : theme().separation;
    const float cw = (rect_.w - sep * float(cols - 1)) / float(cols);
    const float ch = (rect_.h - sep * float(rows - 1)) / float(std::max(rows, 1));
    for (size_t i = 0; i < kids.size(); i++) {
        const int col = int(i % size_t(cols));
        const int row = int(i / size_t(cols));
        place(kids[i], {rect_.x + float(col) * (cw + sep),
                        rect_.y + float(row) * (ch + sep), cw, ch});
    }
}

// --- margin

Vec2 MarginContainer::minimum_size() const {
    const float p = theme().padding;
    const float l = margin_left >= 0 ? margin_left : p;
    const float t = margin_top >= 0 ? margin_top : p;
    const float r = margin_right >= 0 ? margin_right : p;
    const float b = margin_bottom >= 0 ? margin_bottom : p;
    Vec2 inner = Container::minimum_size();
    return {inner.x + l + r, inner.y + t + b};
}

void MarginContainer::arrange(const std::vector<Control *> &kids) {
    const float p = theme().padding;
    const float l = margin_left >= 0 ? margin_left : p;
    const float t = margin_top >= 0 ? margin_top : p;
    const float r = margin_right >= 0 ? margin_right : p;
    const float b = margin_bottom >= 0 ? margin_bottom : p;
    const ui::Rect inner = rect_.inset(l, t, r, b);
    for (Control *c : kids) place(c, inner);
}

// --- centre

void CenterContainer::arrange(const std::vector<Control *> &kids) {
    for (Control *c : kids) {
        const Vec2 m = c->combined_minimum_size();
        place(c, {rect_.x + (rect_.w - m.x) * 0.5f,
                  rect_.y + (rect_.h - m.y) * 0.5f, m.x, m.y});
    }
}

// --- panel container

Vec2 PanelContainer::minimum_size() const {
    const float p = theme().padding;
    Vec2 inner = Container::minimum_size();
    return {inner.x + p * 2, inner.y + p * 2};
}

void PanelContainer::arrange(const std::vector<Control *> &kids) {
    const ui::Rect inner = rect_.inset(theme().padding);
    for (Control *c : kids) place(c, inner);
}

void PanelContainer::draw_ui(ui::DrawList &out) {
    const Theme &t = theme();
    out.rect_rounded(rect_, to_rgba(use_theme_colour ? t.panel : colour,
                                    opacity),
                     t.corner_radius);
}

// ------------------------------------------------------------ widgets

void Panel::draw_ui(ui::DrawList &out) {
    const Theme &t = theme();
    out.rect_rounded(rect_, to_rgba(use_theme_colour ? t.panel : colour,
                                    opacity),
                     t.corner_radius);
    if (border) out.rect_outline(rect_, to_rgba(t.border, opacity),
                                 t.border_width);
}

void ColorRect::draw_ui(ui::DrawList &out) {
    out.rect(rect_, to_rgba(colour, opacity));
}

Vec2 Label::minimum_size() const {
    // Asked of the same draw list that will render it, so a label's
    // idea of its width and its actual width cannot disagree.
    ui::DrawList probe;
    probe.text_scale = theme().font_scale;
    return {probe.text_width(text), probe.text_height()};
}

void Label::draw_ui(ui::DrawList &out) {
    const Theme &t = theme();
    const ui::TextAlign a = align == 1   ? ui::TextAlign::Centre
                            : align == 2 ? ui::TextAlign::Right
                                         : ui::TextAlign::Left;
    if (outline > 0.0f && outline_colour.a > 0.0f && opacity > 0.0f) {
        const uint32_t edge = to_rgba(outline_colour, opacity);
        if (outline <= 1.0f) {
            // A drop shadow: one stamp, offset.
            ui::Rect r = rect_;
            r.x += shadow_offset.x;
            r.y += shadow_offset.y;
            out.text_in(r, text, edge, a, vertical_centre);
        } else {
            // Stamped all the way round. Eight offsets rather than
            // four: the corners are what stop a diagonal edge in
            // the scene showing through between the stamps.
            const float d = outline;
            static const float kx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
            static const float ky[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
            for (int i = 0; i < 8; i++) {
                ui::Rect r = rect_;
                r.x += kx[i] * d;
                r.y += ky[i] * d;
                out.text_in(r, text, edge, a, vertical_centre);
            }
        }
    }
    out.text_in(rect_, text,
                to_rgba(use_theme_colour ? t.text : colour, opacity), a,
                vertical_centre);
}

Button::Button() {
    mouse_filter = MouseFilter::Stop;
    focus_mode = FocusMode::Click;
}

Vec2 Button::minimum_size() const {
    ui::DrawList probe;
    probe.text_scale = theme().font_scale;
    const float p = theme().padding;
    return {probe.text_width(text) + p * 2, probe.text_height() + p};
}

void Button::draw_ui(ui::DrawList &out) {
    const Theme &t = theme();
    Color fill = t.control;
    if (disabled) fill = t.control;
    else if (held_ || (toggle_mode && pressed)) fill = t.control_pressed;
    else if (hovered()) fill = t.control_hover;
    out.rect_rounded(rect_, to_rgba(fill, disabled ? opacity * 0.5f : opacity),
                     t.corner_radius);
    if (has_focus())
        out.rect_outline(rect_, to_rgba(t.accent, opacity), t.border_width);
    out.text_in(rect_, text,
                to_rgba(disabled ? t.text_dim : t.text, opacity),
                ui::TextAlign::Centre);
}

void Button::gui_input(const UiEvent &e) {
    if (disabled) return;
    if (e.type == UiEvent::Type::MouseDown && e.button == 1) {
        held_ = true;
        grab_focus();
        e.handled = true;
    } else if (e.type == UiEvent::Type::MouseUp && e.button == 1) {
        // THE RELEASE OVER THE CONTROL IS THE CLICK, which is what
        // lets someone press a button and then change their mind by
        // dragging off it.
        const bool inside = rect_.contains(e.global);
        if (held_ && inside) {
            if (toggle_mode) {
                pressed = !pressed;
                Array args{Variant(pressed)};
                emitv("toggled", args);
            }
            emit("pressed");
        }
        held_ = false;
        e.handled = true;
    }
}

CheckBox::CheckBox() {
    toggle_mode = true;
    text = "CheckBox";
}

Vec2 CheckBox::minimum_size() const {
    ui::DrawList probe;
    probe.text_scale = theme().font_scale;
    const float box = probe.text_height() + 4.0f;
    return {box + 6.0f + probe.text_width(text), box};
}

void CheckBox::draw_ui(ui::DrawList &out) {
    const Theme &t = theme();
    const float box = out.text_height() + 4.0f;
    const ui::Rect square{rect_.x, rect_.y + (rect_.h - box) * 0.5f, box, box};
    out.rect_rounded(square,
                     to_rgba(hovered() ? t.control_hover : t.control, opacity),
                     t.corner_radius);
    if (pressed) out.rect_rounded(square.inset(3.0f), to_rgba(t.accent, opacity),
                                  t.corner_radius * 0.5f);
    out.rect_outline(square, to_rgba(has_focus() ? t.accent : t.border, opacity),
                     t.border_width);
    out.text_in({square.right() + 6.0f, rect_.y,
                 rect_.w - box - 6.0f, rect_.h},
                text, to_rgba(t.text, opacity), ui::TextAlign::Left);
}

float ProgressBar::fraction() const {
    const float span = max_value - min_value;
    if (span <= 1e-6f) return 0.0f;
    return std::clamp((value - min_value) / span, 0.0f, 1.0f);
}

Vec2 ProgressBar::minimum_size() const {
    ui::DrawList probe;
    probe.text_scale = theme().font_scale;
    return {60.0f, probe.text_height() + 4.0f};
}

void ProgressBar::draw_ui(ui::DrawList &out) {
    const Theme &t = theme();
    out.rect_rounded(rect_, to_rgba(t.control, opacity), t.corner_radius);
    const float f = fraction();
    if (f > 0.0f) {
        ui::Rect fill = rect_;
        fill.w *= f;
        out.rect_rounded(fill, to_rgba(t.accent, opacity), t.corner_radius);
    }
    if (show_percentage) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d%%", int(f * 100.0f + 0.5f));
        out.text_in(rect_, buf, to_rgba(t.text, opacity),
                    ui::TextAlign::Centre);
    }
}

Slider::Slider() {
    mouse_filter = MouseFilter::Stop;
    focus_mode = FocusMode::Click;
}

Vec2 Slider::minimum_size() const {
    return vertical ? Vec2(18.0f, 60.0f) : Vec2(60.0f, 18.0f);
}

void Slider::set_value(float v) {
    const float low = std::min(min_value, max_value);
    const float high = std::max(min_value, max_value);
    float clamped = std::clamp(v, low, high);
    if (step > 0.0f)
        clamped = low + std::round((clamped - low) / step) * step;
    clamped = std::clamp(clamped, low, high);
    if (clamped == value) return;
    value = clamped;
    Array args{Variant(double(value))};
    emitv("value_changed", args);
}

void Slider::set_from_position(const Vec2 &local) {
    const float span = max_value - min_value;
    const float t = vertical ? 1.0f - local.y / std::max(rect_.h, 1.0f)
                             : local.x / std::max(rect_.w, 1.0f);
    set_value(min_value + span * std::clamp(t, 0.0f, 1.0f));
}

void Slider::draw_ui(ui::DrawList &out) {
    const Theme &t = theme();
    const float span = max_value - min_value;
    const float f = span > 1e-6f
                        ? std::clamp((value - min_value) / span, 0.0f, 1.0f)
                        : 0.0f;
    const float track = 6.0f;
    if (vertical) {
        const ui::Rect bar{rect_.centre().x - track * 0.5f, rect_.y, track,
                           rect_.h};
        out.rect_rounded(bar, to_rgba(t.control, opacity), track * 0.5f);
        const float y = rect_.bottom() - rect_.h * f;
        out.rect_rounded({bar.x, y, track, rect_.bottom() - y},
                         to_rgba(t.accent, opacity), track * 0.5f);
        const float knob = 12.0f;
        out.rect_rounded({rect_.centre().x - knob * 0.5f, y - knob * 0.5f, knob,
                          knob},
                         to_rgba(hovered() || dragging_ ? t.control_hover
                                                        : t.text_dim,
                                 opacity),
                         knob * 0.5f);
    } else {
        const ui::Rect bar{rect_.x, rect_.centre().y - track * 0.5f, rect_.w,
                           track};
        out.rect_rounded(bar, to_rgba(t.control, opacity), track * 0.5f);
        out.rect_rounded({bar.x, bar.y, bar.w * f, track},
                         to_rgba(t.accent, opacity), track * 0.5f);
        const float knob = 12.0f;
        out.rect_rounded({rect_.x + rect_.w * f - knob * 0.5f,
                          rect_.centre().y - knob * 0.5f, knob, knob},
                         to_rgba(hovered() || dragging_ ? t.control_hover
                                                        : t.text_dim,
                                 opacity),
                         knob * 0.5f);
    }
}

void Slider::gui_input(const UiEvent &e) {
    if (e.type == UiEvent::Type::MouseDown && e.button == 1) {
        dragging_ = true;
        grab_focus();
        set_from_position(e.position);
        e.handled = true;
    } else if (e.type == UiEvent::Type::MouseUp && e.button == 1) {
        dragging_ = false;
        e.handled = true;
    } else if (e.type == UiEvent::Type::MouseMove && dragging_) {
        set_from_position(e.position);
        e.handled = true;
    }
}

LineEdit::LineEdit() {
    mouse_filter = MouseFilter::Stop;
    focus_mode = FocusMode::Click;
}

Vec2 LineEdit::minimum_size() const {
    ui::DrawList probe;
    probe.text_scale = theme().font_scale;
    return {80.0f, probe.text_height() + theme().padding};
}

void LineEdit::set_text(const std::string &t) {
    text = t;
    caret_ = int(text.size());
}

void LineEdit::draw_ui(ui::DrawList &out) {
    const Theme &t = theme();
    out.rect_rounded(rect_, to_rgba(t.control, opacity), t.corner_radius);
    out.rect_outline(rect_, to_rgba(has_focus() ? t.accent : t.border, opacity),
                     t.border_width);
    const ui::Rect inner = rect_.inset(6.0f, 0, 6.0f, 0);
    out.push_clip(inner);
    if (text.empty() && !has_focus()) {
        out.text_in(inner, placeholder, to_rgba(t.text_dim, opacity),
                    ui::TextAlign::Left);
    } else {
        const std::string shown =
            secret ? std::string(text.size(), '*') : text;
        out.text_in(inner, shown, to_rgba(t.text, opacity),
                    ui::TextAlign::Left);
        if (has_focus()) {
            const float cx =
                inner.x + out.text_width(shown.substr(0, size_t(caret_)));
            out.rect({cx, inner.y + (inner.h - out.text_height()) * 0.5f,
                      std::max(1.0f, out.text_scale), out.text_height()},
                     to_rgba(t.accent, opacity));
        }
    }
    out.pop_clip();
}

void LineEdit::gui_input(const UiEvent &e) {
    switch (e.type) {
        case UiEvent::Type::MouseDown:
            grab_focus();
            e.handled = true;
            break;
        case UiEvent::Type::Text:
            if (!has_focus()) break;
            for (char c : e.text) {
                if (c < 32 || c >= 127) continue;
                if (int(text.size()) >= max_length) break;
                text.insert(text.begin() + caret_, c);
                caret_++;
            }
            if (!e.text.empty()) {
                Array args{Variant(text)};
                emitv("text_changed", args);
            }
            e.handled = true;
            break;
        case UiEvent::Type::Key: {
            if (!has_focus()) break;
            // 42 backspace, 40 return, 79 right, 80 left, 76 delete:
            // the scancodes Input uses.
            if (e.key == 42 && caret_ > 0) {
                text.erase(text.begin() + (caret_ - 1));
                caret_--;
                Array args{Variant(text)};
                emitv("text_changed", args);
            } else if (e.key == 76 && caret_ < int(text.size())) {
                text.erase(text.begin() + caret_);
                Array args{Variant(text)};
                emitv("text_changed", args);
            } else if (e.key == 80 && caret_ > 0) {
                caret_--;
            } else if (e.key == 79 && caret_ < int(text.size())) {
                caret_++;
            } else if (e.key == 40) {
                Array args{Variant(text)};
                emitv("text_submitted", args);
                release_focus();
            }
            e.handled = true;
            break;
        }
        default:
            break;
    }
}

void LineEdit::focus_exited() {
    Array args{Variant(text)};
    emitv("text_submitted", args);
}

Vec2 TextureRect::minimum_size() const {
    if (!texture) return custom_minimum_size;
    return {float(texture->width()), float(texture->height())};
}

void TextureRect::draw_ui(ui::DrawList &out) {
    // The UI renderer binds one atlas, so a textured rect is drawn
    // as a tint for now; a per-control texture needs a second bind
    // group and is the next thing this wants.
    out.rect(rect_, to_rgba(modulate, opacity));
}

// ----------------------------------------------------------- reflection

static void register_controls() {
    // Abstract: a Container with no arrange() is not a thing a
    // scene can contain, and ClassBuilder(false) is how a class says
    // it is in the hierarchy but not constructible.
    ClassBuilder<Container>(false);
    ClassBuilder<BoxContainer>(false)
        .field("vertical", &BoxContainer::vertical)
        .field("separation", &BoxContainer::separation);
    ClassBuilder<HBoxContainer>();
    ClassBuilder<VBoxContainer>();
    ClassBuilder<GridContainer>()
        .field("columns", &GridContainer::columns, "range:1,16")
        .field("separation", &GridContainer::separation);
    ClassBuilder<MarginContainer>()
        .field("margin_left", &MarginContainer::margin_left)
        .field("margin_top", &MarginContainer::margin_top)
        .field("margin_right", &MarginContainer::margin_right)
        .field("margin_bottom", &MarginContainer::margin_bottom);
    ClassBuilder<CenterContainer>();
    ClassBuilder<PanelContainer>()
        .field("use_theme_colour", &PanelContainer::use_theme_colour)
        .field("colour", &PanelContainer::colour);

    ClassBuilder<Panel>()
        .field("use_theme_colour", &Panel::use_theme_colour)
        .field("colour", &Panel::colour)
        .field("border", &Panel::border);
    ClassBuilder<ColorRect>().field("colour", &ColorRect::colour);
    ClassBuilder<Label>()
        .prop("text", &Label::get_text, &Label::set_text)
        .field("use_theme_colour", &Label::use_theme_colour)
        .field("colour", &Label::colour)
        .field("align", &Label::align, "range:0,2")
        .field("vertical_centre", &Label::vertical_centre)
        .field("outline", &Label::outline, "range:0,8")
        .field("outline_colour", &Label::outline_colour)
        .field("shadow_offset", &Label::shadow_offset);
    ClassBuilder<Button>()
        .prop("text", &Button::get_text, &Button::set_text)
        .field("toggle_mode", &Button::toggle_mode)
        .field("pressed", &Button::pressed)
        .field("disabled", &Button::disabled)
        .signal("pressed")
        .signal("toggled", {VType::Bool});
    ClassBuilder<CheckBox>();
    ClassBuilder<ProgressBar>()
        .field("value", &ProgressBar::value)
        .field("min_value", &ProgressBar::min_value)
        .field("max_value", &ProgressBar::max_value)
        .field("show_percentage", &ProgressBar::show_percentage)
        .method("fraction", &ProgressBar::fraction);
    ClassBuilder<Slider>()
        .field("min_value", &Slider::min_value)
        .field("max_value", &Slider::max_value)
        .field("step", &Slider::step)
        .field("vertical", &Slider::vertical)
        .method("set_value", &Slider::set_value).args("value")
        .signal("value_changed", {VType::Float});
    ClassBuilder<LineEdit>()
        .prop("text", &LineEdit::get_text, &LineEdit::set_text)
        .field("placeholder", &LineEdit::placeholder)
        .field("secret", &LineEdit::secret)
        .field("max_length", &LineEdit::max_length)
        .signal("text_changed", {VType::String})
        .signal("text_submitted", {VType::String});
    ClassBuilder<TextureRect>()
        .field("modulate", &TextureRect::modulate);
}
WR_REGISTER(register_controls)

}  // namespace wr
