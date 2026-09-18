// Warren -- driving a tree of Controls.
//
// Three passes a frame, in this order and for a reason:
//
//   LAYOUT first, because input and drawing both need to know where
//   everything is, and a control that moved this frame should be
//   clicked where it is now rather than where it was.
//
//   INPUT second, because a button pressed this frame should look
//   pressed this frame. Handling input after drawing is how a UI
//   ends up one frame behind the pointer.
//
//   DRAW last.
//
// Hit testing walks the tree BACKWARDS -- last child first -- because
// later siblings are drawn on top, and the thing on top is the thing
// you clicked.
#pragma once

#include <string>
#include <vector>

#include "scene/control.h"
#include "ui/draw_list.h"

namespace wr {

class SceneTree;

class UiSystem {
public:
    // What the platform collected. Kept separate from Input so a
    // test can drive it without a window.
    struct Frame {
        float width = 0, height = 0;
        Vec2 mouse{0, 0};
        bool mouse_down = false;
        bool mouse_right = false;
        float wheel = 0.0f;
        std::string text;
        std::vector<int> keys_pressed;   // scancodes, this frame
        bool ctrl = false, shift = false, alt = false;
    };

    void update(SceneTree *tree, const Frame &frame, float dt);
    const ui::DrawList &draw_list() const { return draw_; }
    ui::DrawList &draw_list() { return draw_; }

    // True when a control is under the pointer, so the game should
    // not also act on the click.
    bool mouse_over_ui() const { return hovered_ != nullptr; }
    bool keyboard_captured() const;
    Control *hovered() const { return hovered_; }

    // The topmost control at a point that will accept the mouse.
    static Control *pick(Control *root, const Vec2 &at);

private:
    void layout(SceneTree *tree, const Frame &frame);
    void dispatch(const Frame &frame);
    void paint(Control *c, float inherited_opacity);
    void send(Control *c, UiEvent &e);

    ui::DrawList draw_;
    std::vector<Control *> roots_;
    Control *hovered_ = nullptr;
    Control *pressed_ = nullptr;
    bool was_down_ = false;
};

// The focused control, wherever it is. One at a time, across every
// root, because a keyboard has one destination.
Control *ui_focused_control();
void ui_set_focused_control(Control *c);

}  // namespace wr
