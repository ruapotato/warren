#include "input.h"

#include <cstring>

namespace wr {
namespace {

struct ActionBinding {
    std::vector<int> keys;
    std::vector<int> buttons;
};

struct State {
    bool key[int(Key::Count)] = {};
    bool key_prev[int(Key::Count)] = {};
    bool button[8] = {};
    bool button_prev[8] = {};
    Vec2 motion;
    Vec2 position;
    float wheel = 0.0f;
    std::string text;
    std::unordered_map<std::string, ActionBinding> actions;
};

State &st() {
    static State s;
    return s;
}

bool valid_key(Key k) {
    return int(k) > 0 && int(k) < int(Key::Count);
}
bool valid_button(int b) { return b >= 0 && b < 8; }

}  // namespace

bool Input::key_down(Key k) { return valid_key(k) && st().key[int(k)]; }
bool Input::key_just_pressed(Key k) {
    return valid_key(k) && st().key[int(k)] && !st().key_prev[int(k)];
}
bool Input::key_just_released(Key k) {
    return valid_key(k) && !st().key[int(k)] && st().key_prev[int(k)];
}

bool Input::mouse_down(MouseButton b) {
    return valid_button(int(b)) && st().button[int(b)];
}
bool Input::mouse_just_pressed(MouseButton b) {
    return valid_button(int(b)) && st().button[int(b)] && !st().button_prev[int(b)];
}
bool Input::mouse_just_released(MouseButton b) {
    return valid_button(int(b)) && !st().button[int(b)] && st().button_prev[int(b)];
}

Vec2 Input::mouse_motion() { return st().motion; }
Vec2 Input::mouse_position() { return st().position; }
float Input::wheel() { return st().wheel; }

void Input::bind_action(const std::string &a, Key k) {
    st().actions[a].keys.push_back(int(k));
}
void Input::bind_action_mouse(const std::string &a, MouseButton b) {
    st().actions[a].buttons.push_back(int(b));
}
void Input::clear_action(const std::string &a) { st().actions.erase(a); }

std::vector<std::string> Input::action_names() {
    std::vector<std::string> v;
    for (const auto &kv : st().actions) v.push_back(kv.first);
    return v;
}

// An action is down when ANY of its bindings is, and just-pressed when
// it is down now and was not before -- computed from the same binding
// set, so adding a second key to an action cannot make it fire twice.
static bool action_state(const std::string &a, bool previous) {
    auto it = st().actions.find(a);
    if (it == st().actions.end()) return false;
    const ActionBinding &b = it->second;
    for (int k : b.keys)
        if (k > 0 && k < int(Key::Count) && (previous ? st().key_prev[k] : st().key[k]))
            return true;
    for (int m : b.buttons)
        if (m >= 0 && m < 8 && (previous ? st().button_prev[m] : st().button[m]))
            return true;
    return false;
}

bool Input::action_down(const std::string &a) { return action_state(a, false); }
bool Input::action_just_pressed(const std::string &a) {
    return action_state(a, false) && !action_state(a, true);
}
bool Input::action_just_released(const std::string &a) {
    return !action_state(a, false) && action_state(a, true);
}

float Input::action_axis(const std::string &neg, const std::string &pos) {
    return (action_down(pos) ? 1.0f : 0.0f) - (action_down(neg) ? 1.0f : 0.0f);
}

Vec2 Input::action_vector(const std::string &nx, const std::string &px,
                          const std::string &ny, const std::string &py) {
    Vec2 v(action_axis(nx, px), action_axis(ny, py));
    // Normalised past unit length so diagonal movement is not faster,
    // but a partly-pressed stick keeps its magnitude.
    float l = v.length();
    return l > 1.0f ? v / l : v;
}

const std::string &Input::text_typed() { return st().text; }

void Input::_begin_frame() {
    State &s = st();
    std::memcpy(s.key_prev, s.key, sizeof(s.key));
    std::memcpy(s.button_prev, s.button, sizeof(s.button));
    s.motion = Vec2();
    s.wheel = 0.0f;
    s.text.clear();
}

void Input::_feed_key(int scancode, bool down) {
    if (scancode > 0 && scancode < int(Key::Count)) st().key[scancode] = down;
}
void Input::_feed_mouse_button(int button, bool down) {
    if (valid_button(button)) st().button[button] = down;
}
void Input::_feed_mouse_motion(float dx, float dy, float x, float y) {
    st().motion += Vec2(dx, dy);
    st().position = Vec2(x, y);
}
void Input::_feed_wheel(float w) { st().wheel += w; }
void Input::_feed_text(const char *utf8) {
    if (utf8) st().text += utf8;
}

// EVERYTHING IS RELEASED WHEN THE WINDOW LOSES FOCUS. Alt-tabbing while
// holding W otherwise leaves the player walking into a wall until they
// come back and press it again.
void Input::_focus_lost() {
    State &s = st();
    std::memset(s.key, 0, sizeof(s.key));
    std::memset(s.button, 0, sizeof(s.button));
    s.motion = Vec2();
}

}  // namespace wr
