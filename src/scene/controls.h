// Manifold -- the controls themselves.
//
// A container's job is to decide where its children go, which means
// it OVERRIDES their anchors rather than reading them. That is the
// one thing about a node UI that surprises people: a button inside a
// VBoxContainer cannot set its own position, and should not be able
// to, because the whole point of putting it in a box is that the box
// arranges it.
#pragma once

#include <string>
#include <vector>

#include "render/texture.h"
#include "scene/control.h"

namespace mf {

// --------------------------------------------------------- containers

class Container : public Control {
    MF_CLASS(Container, Control)

public:
    // A container's minimum size is what it needs to hold its
    // children at theirs, which is what lets a window shrink until
    // the content stops it and no further.
    Vec2 minimum_size() const override;
    void layout_children() override;

protected:
    // Children that take part: visible Controls, in order.
    std::vector<Control *> laid_out() const;
    // Give a child a rectangle, honouring its shrink flags within
    // the space it was allotted.
    static void place(Control *c, const ui::Rect &slot);
    virtual void arrange(const std::vector<Control *> &children) = 0;
};

// Children in a row or a column. Expanding children share whatever
// is left over in proportion to their stretch ratios.
class BoxContainer : public Container {
    MF_CLASS(BoxContainer, Container)

public:
    bool vertical = false;
    float separation = -1.0f;   // below zero: take the theme's
    Vec2 minimum_size() const override;

protected:
    void arrange(const std::vector<Control *> &children) override;
    float gap() const;
};

class HBoxContainer : public BoxContainer {
    MF_CLASS(HBoxContainer, BoxContainer)
public:
    HBoxContainer() { vertical = false; }
};

class VBoxContainer : public BoxContainer {
    MF_CLASS(VBoxContainer, BoxContainer)
public:
    VBoxContainer() { vertical = true; }
};

// A fixed number of columns, rows as deep as their tallest cell.
class GridContainer : public Container {
    MF_CLASS(GridContainer, Container)

public:
    int columns = 2;
    float separation = -1.0f;
    Vec2 minimum_size() const override;

protected:
    void arrange(const std::vector<Control *> &children) override;
};

class MarginContainer : public Container {
    MF_CLASS(MarginContainer, Container)

public:
    float margin_left = -1, margin_top = -1, margin_right = -1,
          margin_bottom = -1;   // below zero: the theme's padding
    Vec2 minimum_size() const override;

protected:
    void arrange(const std::vector<Control *> &children) override;
};

class CenterContainer : public Container {
    MF_CLASS(CenterContainer, Container)

protected:
    void arrange(const std::vector<Control *> &children) override;
};

// A margin container that also draws a panel behind itself.
class PanelContainer : public Container {
    MF_CLASS(PanelContainer, Container)

public:
    bool use_theme_colour = true;
    Color colour = Color::hex(0x262A30);
    Vec2 minimum_size() const override;
    void draw_ui(ui::DrawList &out) override;

protected:
    void arrange(const std::vector<Control *> &children) override;
};

// ------------------------------------------------------------ widgets

class Panel : public Control {
    MF_CLASS(Panel, Control)

public:
    bool use_theme_colour = true;
    Color colour = Color::hex(0x262A30);
    bool border = false;
    void draw_ui(ui::DrawList &out) override;
};

class ColorRect : public Control {
    MF_CLASS(ColorRect, Control)

public:
    Color colour = Color::white();
    void draw_ui(ui::DrawList &out) override;
};

class Label : public Control {
    MF_CLASS(Label, Control)

public:
    std::string text = "Label";
    // -1 uses the theme's colour, which is what almost every label
    // should do -- a UI where each label chose its own colour is a
    // UI nobody can restyle.
    bool use_theme_colour = true;
    Color colour = Color::white();
    int align = 0;              // 0 left, 1 centre, 2 right
    bool vertical_centre = true;

    Vec2 minimum_size() const override;
    void draw_ui(ui::DrawList &out) override;
    void set_text(const std::string &t) { text = t; }
    std::string get_text() const { return text; }
};

class Button : public Control {
    MF_CLASS(Button, Control)

public:
    Button();
    std::string text = "Button";
    bool toggle_mode = false;
    bool pressed = false;       // for a toggle
    bool disabled = false;

    Vec2 minimum_size() const override;
    void draw_ui(ui::DrawList &out) override;
    void gui_input(const UiEvent &e) override;
    void set_text(const std::string &t) { text = t; }
    std::string get_text() const { return text; }

private:
    bool held_ = false;
};

class CheckBox : public Button {
    MF_CLASS(CheckBox, Button)

public:
    CheckBox();
    Vec2 minimum_size() const override;
    void draw_ui(ui::DrawList &out) override;
};

class ProgressBar : public Control {
    MF_CLASS(ProgressBar, Control)

public:
    float value = 0.5f;
    float min_value = 0.0f, max_value = 1.0f;
    bool show_percentage = true;
    Vec2 minimum_size() const override;
    void draw_ui(ui::DrawList &out) override;
    float fraction() const;
};

class Slider : public Control {
    MF_CLASS(Slider, Control)

public:
    Slider();
    float value = 0.5f;
    float min_value = 0.0f, max_value = 1.0f;
    float step = 0.0f;          // 0 is continuous
    bool vertical = false;

    Vec2 minimum_size() const override;
    void draw_ui(ui::DrawList &out) override;
    void gui_input(const UiEvent &e) override;
    void set_value(float v);

private:
    void set_from_position(const Vec2 &local);
    bool dragging_ = false;
};

class LineEdit : public Control {
    MF_CLASS(LineEdit, Control)

public:
    LineEdit();
    std::string text;
    std::string placeholder = "";
    bool secret = false;
    int max_length = 256;

    Vec2 minimum_size() const override;
    void draw_ui(ui::DrawList &out) override;
    void gui_input(const UiEvent &e) override;
    void focus_exited() override;
    void set_text(const std::string &t);
    std::string get_text() const { return text; }

private:
    int caret_ = 0;
    float blink_ = 0.0f;
};

class TextureRect : public Control {
    MF_CLASS(TextureRect, Control)

public:
    Ref<Texture> texture;
    Color modulate = Color::white();
    void draw_ui(ui::DrawList &out) override;
    Vec2 minimum_size() const override;
};

}  // namespace mf
