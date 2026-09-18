#include "scene/ui_system.h"

#include <algorithm>

#include "core/log.h"
#include "scene/controls.h"
#include "scene/scene_tree.h"

namespace wr {

void UiSystem::update(SceneTree *tree, const Frame &frame, float dt) {
    (void)dt;
    draw_.begin(frame.width, frame.height);
    roots_.clear();
    if (!tree || !tree->root()) {
        draw_.end();
        return;
    }
    layout(tree, frame);
    dispatch(frame);
    for (Control *c : roots_) paint(c, 1.0f);
    draw_.end();
}

// --- layout

void UiSystem::layout(SceneTree *tree, const Frame &frame) {
    const ui::Rect screen{0, 0, frame.width, frame.height};
    // A CONTROL WHOSE PARENT IS NOT A CONTROL IS A ROOT, and it is
    // laid out against the screen. That is what lets a UI hang
    // anywhere in the scene -- under the player, inside a level --
    // rather than only under one special node.
    std::vector<Node *> stack{tree->root()};
    while (!stack.empty()) {
        Node *n = stack.back();
        stack.pop_back();
        if (Control *c = n->cast_to<Control>()) {
            if (!c->visible) continue;
            roots_.push_back(c);
            c->update_layout(screen);
            continue;   // its children are its own business
        }
        for (const auto &child : n->children())
            if (child) stack.push_back(child.get());
    }
    // Drawn in tree order, so a UI declared later is on top.
    std::reverse(roots_.begin(), roots_.end());
}

// --- hit testing

Control *UiSystem::pick(Control *root, const Vec2 &at) {
    if (!root || !root->visible) return nullptr;
    if (root->clip_contents && !root->rect().contains(at)) return nullptr;

    // BACKWARDS. Later siblings draw on top, so they are hit first.
    const auto &kids = root->children();
    for (size_t i = kids.size(); i-- > 0;) {
        Control *c = kids[i] ? kids[i]->cast_to<Control>() : nullptr;
        if (!c) continue;
        if (Control *hit = pick(c, at)) return hit;
    }
    if (root->mouse_filter == Control::MouseFilter::Ignore) return nullptr;
    if (!root->rect().contains(at)) return nullptr;
    return root;
}

// --- input

bool UiSystem::keyboard_captured() const {
    return ui_focused_control() != nullptr;
}

void UiSystem::send(Control *c, UiEvent &e) {
    // Up the chain until something handles it, which is what
    // MouseFilter::Pass is for: a panel that wants to know about a
    // click without stopping the button underneath from getting it.
    for (Control *n = c; n; n = n->parent() ? n->parent()->cast_to<Control>()
                                            : nullptr) {
        e.position = {e.global.x - n->rect().x, e.global.y - n->rect().y};
        n->gui_input(e);
        if (e.handled) return;
        if (n->mouse_filter == Control::MouseFilter::Stop) return;
    }
}

void UiSystem::dispatch(const Frame &frame) {
    Control *under = nullptr;
    for (Control *root : roots_) {
        if (Control *hit = pick(root, frame.mouse)) {
            under = hit;
            break;
        }
    }

    if (under != hovered_) {
        if (hovered_) {
            hovered_->_set_hovered(false);
            hovered_->emit("mouse_exited");
        }
        hovered_ = under;
        if (hovered_) {
            hovered_->_set_hovered(true);
            hovered_->emit("mouse_entered");
        }
    }

    UiEvent e;
    e.global = frame.mouse;
    e.ctrl = frame.ctrl;
    e.shift = frame.shift;
    e.alt = frame.alt;

    if (frame.mouse_down && !was_down_) {
        // A CLICK ON NOTHING CLEARS THE FOCUS, which is what makes a
        // text field commit when you click away from it.
        if (!under) ui_set_focused_control(nullptr);
        pressed_ = under;
        if (under) {
            e.type = UiEvent::Type::MouseDown;
            e.button = 1;
            send(under, e);
        }
    } else if (!frame.mouse_down && was_down_) {
        // Delivered to whatever was pressed, not to whatever is
        // under the pointer now: a button you dragged off still
        // needs to know it was released.
        Control *target = pressed_ ? pressed_ : under;
        if (target) {
            e.type = UiEvent::Type::MouseUp;
            e.button = 1;
            send(target, e);
        }
        pressed_ = nullptr;
    } else {
        Control *target = pressed_ ? pressed_ : under;
        if (target) {
            e.type = UiEvent::Type::MouseMove;
            send(target, e);
        }
    }
    was_down_ = frame.mouse_down;

    if (frame.wheel != 0.0f && under) {
        UiEvent w;
        w.global = frame.mouse;
        w.type = UiEvent::Type::Wheel;
        w.wheel = frame.wheel;
        send(under, w);
    }

    // The keyboard goes to whatever has focus, wherever it is.
    Control *focus = ui_focused_control();
    if (!focus) return;
    if (!frame.text.empty()) {
        UiEvent t;
        t.type = UiEvent::Type::Text;
        t.text = frame.text;
        t.global = frame.mouse;
        t.position = {0, 0};
        focus->gui_input(t);
    }
    for (int key : frame.keys_pressed) {
        UiEvent k;
        k.type = UiEvent::Type::Key;
        k.key = key;
        k.ctrl = frame.ctrl;
        k.shift = frame.shift;
        k.global = frame.mouse;
        focus->gui_input(k);
    }
}

// --- drawing

void UiSystem::paint(Control *c, float inherited) {
    if (!c || !c->visible) return;
    const float alpha = inherited * std::clamp(c->opacity, 0.0f, 1.0f);
    if (alpha <= 0.001f) return;

    const float own = c->opacity;
    c->opacity = alpha;          // so draw_ui sees the inherited value
    draw_.text_scale = c->theme().font_scale;
    if (c->clip_contents) draw_.push_clip(c->rect());
    c->draw_ui(draw_);
    c->opacity = own;

    for (const auto &child : c->children())
        if (Control *cc = child ? child->cast_to<Control>() : nullptr)
            paint(cc, alpha);
    if (c->clip_contents) draw_.pop_clip();
}

}  // namespace wr
