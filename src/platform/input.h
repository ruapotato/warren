// Manifold -- the keyboard, the mouse, and names for what they mean.
//
// Actions, not keys, the way Godot does it: gameplay asks for "jump",
// the binding table says what jump is, and rebinding is a data change.
// Raw key state is still there for tools and debug overlays.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/math/vector.h"

namespace mf {

// SDL scancodes, named. Only the ones a game reaches for; the raw
// scancode is accepted anywhere a Key is.
enum class Key : int {
    Unknown = 0,
    A = 4, B, C, D, E, F, G, H, I, J, K, L, M,
    N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num1 = 30, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9, Num0,
    Return = 40, Escape, Backspace, Tab, Space,
    Minus = 45, Equals, LeftBracket, RightBracket, Backslash,
    Semicolon = 51, Apostrophe, Grave, Comma, Period, Slash,
    CapsLock = 57,
    F1 = 58, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    PrintScreen = 70, ScrollLock, Pause, Insert, Home, PageUp,
    Delete = 76, End, PageDown, Right, Left, Down, Up,
    KpDivide = 84, KpMultiply, KpMinus, KpPlus, KpEnter,
    Kp1 = 89, Kp2, Kp3, Kp4, Kp5, Kp6, Kp7, Kp8, Kp9, Kp0,
    LeftCtrl = 224, LeftShift, LeftAlt, LeftSuper,
    RightCtrl = 228, RightShift, RightAlt, RightSuper,
    Count = 512
};

enum class MouseButton : int { Left = 1, Middle = 2, Right = 3, X1 = 4, X2 = 5 };

class Input {
public:
    // --- raw ------------------------------------------------------------
    static bool key_down(Key k);
    static bool key_just_pressed(Key k);
    static bool key_just_released(Key k);

    static bool mouse_down(MouseButton b);
    static bool mouse_just_pressed(MouseButton b);
    static bool mouse_just_released(MouseButton b);
    // Movement since the last frame, in pixels. With the mouse captured
    // this is relative motion and keeps working past the screen edge.
    static Vec2 mouse_motion();
    static Vec2 mouse_position();
    static float wheel();

    // --- actions ---------------------------------------------------------
    static void bind_action(const std::string &action, Key k);
    static void bind_action_mouse(const std::string &action, MouseButton b);
    static void clear_action(const std::string &action);
    static bool action_down(const std::string &action);
    static bool action_just_pressed(const std::string &action);
    static bool action_just_released(const std::string &action);
    // +1 when `positive` is held, -1 when `negative` is, 0 when both or
    // neither -- the building block of a movement vector.
    static float action_axis(const std::string &negative,
                             const std::string &positive);
    static Vec2 action_vector(const std::string &neg_x, const std::string &pos_x,
                              const std::string &neg_y, const std::string &pos_y);
    static std::vector<std::string> action_names();

    // --- text -------------------------------------------------------------
    static const std::string &text_typed();

    // Called by Window; not for games.
    static void _begin_frame();
    static void _feed_key(int scancode, bool down);
    static void _feed_mouse_button(int button, bool down);
    static void _feed_mouse_motion(float dx, float dy, float x, float y);
    static void _feed_wheel(float w);
    static void _feed_text(const char *utf8);
    static void _focus_lost();
};

}  // namespace mf
