// Warren -- user interface as part of the scene.
//
// A Control is a rectangle in the scene tree. That sentence is the
// whole design and everything below follows from it: a UI made of
// nodes is authored in the editor, laid out by its parents, themed,
// SAVED IN A SCENE FILE and instanced like anything else -- so a
// health bar is a scene, an inventory slot is a scene, and a
// twenty-slot inventory is twenty instances of one file.
//
// That is what the editor's immediate-mode UI cannot be, and why the
// engine has both. A tool's UI is a view of state that changes
// underneath it and is better rebuilt every frame; a game's UI is
// content, and content wants a file.
//
// ANCHORS AND OFFSETS, which is Godot's model and the right one. A
// control's rectangle is derived, every frame, from four anchors
// (fractions of the parent) and four offsets (pixels from those
// anchors). Anchor all four to 0 and the offsets are a fixed box in
// the corner; anchor left/right to 0 and 1 and the control stretches
// with the window; anchor all four to 0.5 and it stays centred at a
// fixed size. One mechanism, and no separate "docking" concept.
#pragma once

#include <string>
#include <vector>

#include "core/object.h"
#include "scene/node.h"
#include "ui/draw_list.h"

namespace wr {

class Theme;

// What the input system hands a control.
struct UiEvent {
    enum class Type { MouseMove, MouseDown, MouseUp, Wheel, Text, Key };
    Type type = Type::MouseMove;
    Vec2 position{0, 0};      // in the control's own space
    Vec2 global{0, 0};        // in screen space
    float wheel = 0.0f;
    int button = 0;           // 1 left, 2 middle, 3 right
    int key = 0;
    std::string text;
    bool ctrl = false, shift = false, alt = false;
    // Set by a handler that has dealt with it; stops the event
    // travelling any further.
    mutable bool handled = false;
};

class Control : public Node {
    WR_CLASS(Control, Node)

public:
    // --- placement ------------------------------------------------------
    // Fractions of the parent's rectangle.
    float anchor_left = 0.0f, anchor_top = 0.0f;
    float anchor_right = 0.0f, anchor_bottom = 0.0f;
    // Pixels from the anchor. Right and bottom are measured from
    // their anchors too, so a right offset of -10 with a right
    // anchor of 1 means "ten pixels in from the right edge".
    float offset_left = 0.0f, offset_top = 0.0f;
    float offset_right = 100.0f, offset_bottom = 30.0f;

    // The named arrangements, because four anchors are four chances
    // to get it wrong and these cover nearly every case.
    enum class Preset {
        TopLeft, TopRight, BottomLeft, BottomRight, Centre,
        LeftWide, RightWide, TopWide, BottomWide, FullRect
    };
    void set_anchors_preset(Preset p, bool keep_size = true);

    // --- how a container should treat it ---------------------------------
    enum SizeFlags {
        SizeFill = 1 << 0,     // take the space given
        SizeExpand = 1 << 1,   // and ask for more of what is spare
        SizeShrinkCentre = 1 << 2,
        SizeShrinkEnd = 1 << 3,
    };
    int size_flags_horizontal = SizeFill;
    int size_flags_vertical = SizeFill;
    // Weight when several siblings expand.
    float stretch_ratio = 1.0f;
    Vec2 custom_minimum_size{0, 0};

    // --- input -----------------------------------------------------------
    enum class MouseFilter {
        Stop,    // handle it and stop it going further
        Pass,    // handle it and let it carry on
        Ignore,  // not interested; the pointer goes through
    };
    MouseFilter mouse_filter = MouseFilter::Stop;
    enum class FocusMode { None, Click, All };
    FocusMode focus_mode = FocusMode::None;

    // --- appearance ------------------------------------------------------
    bool visible = true;
    // Multiplied into everything this control and its children draw.
    float opacity = 1.0f;
    // Children are clipped to this control's rectangle.
    bool clip_contents = false;

    // --- the computed rectangle -------------------------------------------
    // Screen space, filled in by the layout pass. Not settable:
    // setting it would be overwritten next frame, which is the most
    // confusing thing a UI toolkit can do.
    const ui::Rect &rect() const { return rect_; }
    Vec2 position2d() const { return rect_.position(); }
    Vec2 size2d() const { return rect_.size(); }
    // What this control needs at minimum. A container asks its
    // children and a widget answers from its content.
    virtual Vec2 minimum_size() const { return custom_minimum_size; }
    Vec2 combined_minimum_size() const;

    // --- overridable ------------------------------------------------------
    // Draw into `out`. The rectangle is already computed and the
    // clip already set.
    virtual void draw_ui(ui::DrawList &out) { (void)out; }
    // A container arranges its children here. The default gives
    // each child the whole of this control's rectangle through its
    // own anchors.
    virtual void layout_children();
    // Mouse and keyboard, in this control's own coordinates.
    virtual void gui_input(const UiEvent &e) { (void)e; }
    virtual void focus_entered() {}
    virtual void focus_exited() {}

    // --- state the input system sets ---------------------------------------
    bool hovered() const { return hovered_; }
    bool has_focus() const { return focused_; }
    void grab_focus();
    void release_focus();

    // Walks up to the nearest Theme, or the default one.
    const Theme &theme() const;

    // Recomputes this control's rectangle from its parent's, then
    // its children's. Called by the UI system; also useful directly
    // when a tool needs a layout without a frame.
    void update_layout(const ui::Rect &parent_rect);

    // A CONTROL THAT LEAVES THE TREE STOPS BEING FOCUSED.
    //
    // The focus is one global pointer, and a node freed with its
    // scene leaves it dangling -- the same bug the active camera
    // and the audio listener each had, for the same reason. Any
    // global that names a node has to be cleared by the node, not
    // by whoever destroys the scene, because only the node knows
    // every way it can go.
    void on_exit_tree() override;

    void _set_hovered(bool h) { hovered_ = h; }
    void _set_focused(bool f) { focused_ = f; }
    void _set_rect(const ui::Rect &r) { rect_ = r; }

protected:
    ui::Rect rect_;

private:
    bool hovered_ = false;
    bool focused_ = false;
};

// ---------------------------------------------------------------- theme

// Colours and metrics, looked up by walking towards the root. A
// control with no Theme above it gets the default, so a UI works
// before anyone has thought about how it looks.
class Theme : public Object {
    WR_CLASS(Theme, Object)

public:
    Color background = Color::hex(0x1C1E22);
    Color panel = Color::hex(0x262A30);
    Color text = Color::hex(0xDEE2E8);
    Color text_dim = Color::hex(0x8C929C);
    Color accent = Color::hex(0x569CD6);
    Color control = Color::hex(0x30343A);
    Color control_hover = Color::hex(0x40464E);
    Color control_pressed = Color::hex(0x569CD6);
    Color border = Color::hex(0x3C4048);
    float corner_radius = 3.0f;
    float padding = 8.0f;
    float separation = 6.0f;
    float font_scale = 2.0f;
    float border_width = 1.0f;

    static const Theme &fallback();
};

// A node that supplies a theme to everything beneath it.
class ThemeProvider : public Control {
    WR_CLASS(ThemeProvider, Control)

public:
    Ref<Theme> theme_resource;
};

}  // namespace wr
