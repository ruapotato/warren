#include "script/stubs.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <unordered_set>

#include "core/log.h"
#include "core/object.h"
#include "script/python.h"

namespace wr {
namespace {

// A Variant type as a Python annotation. Several engine types map to
// Python's own; the rest are the value classes the binding exposes.
const char *annotation(VType t) {
    switch (t) {
        case VType::Nil: return "None";
        case VType::Bool: return "bool";
        case VType::Int: return "int";
        case VType::Float: return "float";
        case VType::String: return "str";
        case VType::Vec2: return "Vec2";
        case VType::Vec3: return "Vec3";
        case VType::Vec4: return "Vec4";
        case VType::Color: return "Color";
        case VType::Quat: return "Quat";
        case VType::Basis: return "Basis";
        case VType::Transform: return "Transform3D";
        case VType::Plane: return "Plane";
        case VType::AABB: return "AABB";
        case VType::Rect2: return "Rect2";
        case VType::Projection: return "Projection";
        case VType::Object: return "Object | None";
        case VType::Array: return "list";
        case VType::Dict: return "dict";
        default: return "Any";
    }
}

// A property or method name that Python cannot accept as an
// identifier would produce a stub that does not parse.
// The same, but an Object-typed slot names its actual class when the
// registry recorded one. `-> Node | None` beats `-> Object | None`.
std::string annotation(VType t, const std::string &class_name) {
    if (t == VType::Object && !class_name.empty()) {
        // A class a plugin registered may be absent from this build;
        // naming it anyway would leave the stub referring to nothing.
        if (ClassDB::get(class_name)) return class_name + " | None";
    }
    return annotation(t);
}

bool is_identifier(const std::string &s) {
    if (s.empty() || (!std::isalpha((unsigned char)s[0]) && s[0] != '_'))
        return false;
    for (char c : s)
        if (!std::isalnum((unsigned char)c) && c != '_') return false;
    static const std::unordered_set<std::string> keywords = {
        "None",  "True",   "False", "class",  "def",    "lambda", "return",
        "yield", "import", "from",  "global", "pass",   "del",    "not",
        "and",   "or",     "is",    "in",     "if",     "else",   "elif",
        "while", "for",    "try",   "except", "raise",  "with",   "as",
        "async", "await",  "print", "assert", "break",  "continue"};
    return !keywords.count(s);
}

std::string default_repr(const Variant &v) {
    switch (v.type()) {
        case VType::Nil: return "None";
        case VType::Bool: return v.to_bool() ? "True" : "False";
        case VType::Int: return std::to_string(v.to_int());
        case VType::Float: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", v.to_float());
            return buf;
        }
        default: return "...";
    }
}

void emit_method(std::string &out, const MethodInfo &m) {
    if (!is_identifier(m.name)) return;
    out += "    def " + m.name + "(self";
    const int required = m.required();
    for (size_t i = 0; i < m.args.size(); i++) {
        std::string arg = i < m.arg_names.size() && !m.arg_names[i].empty()
                              ? m.arg_names[i]
                              : "a" + std::to_string(i);
        if (!is_identifier(arg)) arg = "a" + std::to_string(i);
        out += ", " + arg + ": " +
               annotation(m.args[i], i < m.arg_classes.size() ? m.arg_classes[i]
                                                              : std::string());
        if (int(i) >= required) {
            const size_t d = i - size_t(required);
            out += " = " + (d < m.defaults.size() ? default_repr(m.defaults[d])
                                                  : std::string("..."));
        }
    }
    if (m.vararg) out += ", *args: Any";
    out += ") -> ";
    out += annotation(m.ret, m.ret_class);
    out += ": ...\n";
}

// Bases before derived classes, so the file reads top-down and a
// checker never sees a forward reference to a class body.
std::vector<ClassInfo *> in_dependency_order() {
    std::vector<ClassInfo *> all = ClassDB::all();
    std::sort(all.begin(), all.end(), [](ClassInfo *a, ClassInfo *b) {
        auto depth = [](ClassInfo *c) {
            int d = 0;
            for (ClassInfo *p = c->base; p; p = p->base) d++;
            return d;
        };
        const int da = depth(a), db = depth(b);
        if (da != db) return da < db;
        return a->name < b->name;
    });
    return all;
}

const char *k_preamble = R"(# Warren -- generated type stubs. Do not edit.
#
# Written from the engine's own class registry by `warren --stubs`,
# so this file describes exactly the classes, methods and properties
# the running engine exposes -- plugins included, if they were loaded
# when it was generated. Regenerate it when the engine changes.
#
# Put it next to your scripts, or anywhere on the type checker's path,
# and an editor will complete `mf.` correctly.

from typing import Any

version: str

# ------------------------------------------------------------ values
#
# Small value types. They are copied, not referenced: reading
# `node.position` gives a Vec3 that is not the node's, so mutating it
# changes nothing -- assign the whole property back.

class _Value:
    def __init__(self, *args: Any) -> None: ...
    def as_tuple(self) -> tuple[float, ...]: ...
    def __eq__(self, other: object) -> bool: ...
    def __ne__(self, other: object) -> bool: ...
    def __repr__(self) -> str: ...

class _Vector(_Value):
    def length(self) -> float: ...
    def normalized(self) -> Any: ...
    def dot(self, other: Any) -> float: ...
    def cross(self, other: Any) -> Any: ...
    def __add__(self, other: Any) -> Any: ...
    def __sub__(self, other: Any) -> Any: ...
    def __mul__(self, other: Any) -> Any: ...
    def __rmul__(self, other: Any) -> Any: ...
    def __truediv__(self, other: Any) -> Any: ...
    def __neg__(self) -> Any: ...

class Vec2(_Vector):
    x: float
    y: float
    def __init__(self, x: float = 0, y: float = 0) -> None: ...

class Vec3(_Vector):
    x: float
    y: float
    z: float
    def __init__(self, x: float = 0, y: float = 0, z: float = 0) -> None: ...

class Vec4(_Vector):
    x: float
    y: float
    z: float
    w: float
    def __init__(self, x: float = 0, y: float = 0, z: float = 0,
                 w: float = 0) -> None: ...

class Color(_Vector):
    r: float
    g: float
    b: float
    a: float
    def __init__(self, r: float = 0, g: float = 0, b: float = 0,
                 a: float = 1) -> None: ...

class Quat(_Value):
    x: float
    y: float
    z: float
    w: float
    def __init__(self, x: float = 0, y: float = 0, z: float = 0,
                 w: float = 1) -> None: ...
    @staticmethod
    def from_euler_yxz(yaw: float, pitch: float = 0,
                       roll: float = 0) -> Quat: ...
    @staticmethod
    def from_axis_angle(axis: Vec3, angle: float) -> Quat: ...
    @staticmethod
    def between(from_dir: Vec3, to_dir: Vec3) -> Quat: ...

class Basis(_Value):
    def __init__(self) -> None: ...
    @staticmethod
    def from_axis_angle(axis: Vec3, angle: float) -> Basis: ...

class Transform3D(_Value):
    origin: Vec3
    basis: Basis
    def __init__(self, origin_or_basis: Any = ..., origin: Vec3 = ...) -> None: ...

class Plane(_Value):
    normal: Vec3
    d: float
    def __init__(self, normal: Vec3 = ..., d: float = 0) -> None: ...

class AABB(_Value):
    min: Vec3
    max: Vec3
    def __init__(self, lo: Vec3 = ..., hi: Vec3 = ...) -> None: ...

class Rect2(_Value):
    x: float
    y: float
    width: float
    height: float
    def __init__(self, x: float = 0, y: float = 0, w: float = 0,
                 h: float = 0) -> None: ...

class Projection(_Value):
    def __init__(self) -> None: ...

# ----------------------------------------------------------- classes
)";

const char *k_functions = R"(
# --------------------------------------------------------- the module

def load(path: str, hint: str = "") -> Resource | None:
    """Load a resource by path. None if nothing could load it.

    CACHED, AND THAT IS PART OF THE CONTRACT: loading a path
    twice gives the SAME object, not two equal ones, which is
    what lets a hundred bodies share one mesh and one material.

    `hint` steers the loader. For an image it decides the colour
    space: "linear" or "data" for a normal map or an ORM, which
    are not pictures -- running them through the sRGB curve
    makes every surface smoother than it was authored and every
    normal weaker, which looks like a lighting bug and is a
    file-reading one.
    """

def shape(spec: dict, cell_size: float = 0.05, detail: int = 0,
          smooth: bool = True) -> Mesh | None:
    """Build a Mesh from a procedural shape description."""

def surface(spec: dict, size: int = 512, seed: int = 0) -> Material | None:
    """Build a Material from a procedural surface description."""

def frame_stats() -> dict:
    """What the last frame cost and what it drew.

    Keys: frame_ms, render_cpu_ms, frame, draws, triangles,
    views, portal_depth, portals_culled, visible_meshes,
    lights, shadow_draws, punctual_shadow_draws, shadows_reused.
    """

def quit() -> None:
    """Ask the engine to stop after this frame."""

def log(message: str) -> None:
    """Write a line to the engine log."""

def warn(message: str) -> None:
    """Write a warning."""

def error(message: str) -> None:
    """Write an error."""

def root() -> Node | None:
    """The scene tree's root node."""

def scene() -> Node | None:
    """The current scene's root node."""

def physics() -> Object | None:
    """The physics world."""

def camera() -> Camera3D | None:
    """The active camera."""

def instantiate(class_name: str) -> Object | None:
    """Make an instance of an engine class by name."""

def classes() -> list[str]:
    """Every registered class name."""

def group(name: str) -> list[Node]:
    """Every node in a group."""

class _Keys:
    """Key codes, as `warren.Key.W`. Letters, the number row as NUM1
    to NUM0, and the ones a game reaches for."""
    A: int; B: int; C: int; D: int; E: int; F: int; G: int; H: int
    I: int; J: int; K: int; L: int; M: int; N: int; O: int; P: int
    Q: int; R: int; S: int; T: int; U: int; V: int; W: int; X: int
    Y: int; Z: int
    NUM1: int; NUM2: int; NUM3: int; NUM4: int; NUM5: int
    NUM6: int; NUM7: int; NUM8: int; NUM9: int; NUM0: int
    F1: int; F2: int; F3: int; F4: int; F5: int; F6: int
    F7: int; F8: int; F9: int; F10: int; F11: int; F12: int
    SPACE: int; ESCAPE: int; TAB: int; RETURN: int; BACKSPACE: int
    SHIFT: int; LSHIFT: int; RSHIFT: int
    CTRL: int; LCTRL: int; RCTRL: int; ALT: int
    UP: int; DOWN: int; LEFT: int; RIGHT: int
    DELETE: int; HOME: int; END: int
    COMMA: int; PERIOD: int; GRAVE: int

class _Buttons:
    """Mouse buttons, as `warren.Mouse.LEFT`."""
    LEFT: int; MIDDLE: int; RIGHT: int; X1: int; X2: int

Key: _Keys
Mouse: _Buttons

def environment() -> Environment | None:
    """The sky, the sun and the fog. One object, kept -- holding on
    to it is fine. None with no renderer."""

def key_down(key: int) -> bool:
    """Is a key held?"""

def key_pressed(key: int) -> bool:
    """Was a key pressed this frame?"""

def key_released(key: int) -> bool:
    """Was a key released this frame?"""

def mouse_down(button: int) -> bool:
    """Is a mouse button held?"""

def mouse_pressed(button: int) -> bool:
    """Was a mouse button pressed this frame?"""

def mouse_released(button: int) -> bool:
    """Was a mouse button released this frame?"""

def screenshot(path: str) -> bool:
    """Write the last frame to a PNG. Returns whether it worked."""

def pose(position: Vec3 = ..., yaw: float = 0.0, pitch: float = 0.0,
         roll: float = 0.0, scale: float = 1.0) -> Transform3D:
    """A Transform3D with an orientation and a size.

    Transform3D is constructible from script and Basis is not, so
    without this a script can say where to put something and
    nothing else. Euler is the engine's (yaw, pitch, roll).
    """

def mouse_wheel() -> float:
    """How far the wheel turned this frame, in notches."""

def mouse_captured() -> bool:
    """Is the pointer locked to the window?"""

def capture_mouse(on: bool) -> None:
    """Lock or release the pointer. Nothing, with no window."""

def action_down(action: str) -> bool:
    """Is an action held?"""

def action_pressed(action: str) -> bool:
    """Was an action pressed this frame?"""

def action_vector(neg_x: str, pos_x: str, neg_y: str, pos_y: str) -> Vec2:
    """A movement vector from four actions."""

def bind_action(action: str, key: int) -> None:
    """Bind a key to an action."""

def mouse_motion() -> Vec2:
    """Mouse movement since the last frame."""

def time() -> float:
    """Seconds since the scene started."""

def fps() -> float:
    """Smoothed frames per second."""

def frame() -> int:
    """The frame number."""
)";

}  // namespace

std::string python_stubs() {
    std::string out = k_preamble;

    for (ClassInfo *ci : in_dependency_order()) {
        out += "\nclass " + ci->name;
        // Object is the root; everything else names its base, which
        // has already been emitted.
        if (ci->base) out += "(" + ci->base->name + ")";
        out += ":\n";

        size_t members = 0;

        // Properties, in declaration order -- the same order an
        // inspector would show them, not hash order.
        for (const std::string &name : ci->property_order) {
            const PropertyInfo *p = ci->find_property(name);
            if (!p || !is_identifier(name)) continue;
            out += "    " + name + ": " + annotation(p->type, p->class_name);
            if (!p->hint.empty()) out += "  # " + p->hint;
            out += "\n";
            members++;
        }
        if (members && !ci->method_order.empty()) out += "\n";

        // A class the registry can construct gets a no-argument
        // __init__; an abstract one deliberately does not, so a
        // checker catches `mf.Node3D()` on something that is not
        // constructible.
        if (ci->construct) {
            out += "    def __init__(self) -> None: ...\n";
            members++;
        }

        for (const std::string &name : ci->method_order) {
            const MethodInfo *m = ci->find_method(name);
            if (!m) continue;
            const size_t before = out.size();
            emit_method(out, *m);
            if (out.size() != before) members++;
        }

        // Signals are not attributes, but they are part of the class's
        // surface and a script author needs their names.
        if (!ci->signals.empty()) {
            std::vector<std::string> names;
            for (const auto &kv : ci->signals) names.push_back(kv.first);
            std::sort(names.begin(), names.end());
            out += "    # signals: ";
            for (size_t i = 0; i < names.size(); i++)
                out += (i ? ", " : "") + names[i];
            out += "\n";
        }

        if (!members) out += "    pass\n";
    }

    out += k_functions;
    // EVERY REGISTERED FUNCTION HAS TO BE IN THAT BLOCK.
    //
    // `k_functions` is hand-written, because a good docstring is
    // worth more than the one-line one in the method table. The
    // price is that it drifts: three functions -- load, shape
    // and surface -- were exposed by the engine and absent from
    // the stubs for as long as they had existed, so every editor
    // and every checker said a real call did not exist.
    //
    // Hand-written and checked is the combination that works.
    for (const char *name : python_module_functions()) {
        std::string want = std::string("\ndef ") + name + "(";
        if (out.find(want) == std::string::npos)
            WR_WARN("stubs: wr.%s is registered but not declared in "
                    "k_functions; add it", name);
    }
    return out;
}

bool write_python_stubs(const std::string &path) {
    std::filesystem::path p(path);
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    const std::string text = python_stubs();
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        WR_ERROR("stubs: could not write '%s'", path.c_str());
        return false;
    }
    const size_t n = std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    if (n != text.size()) {
        WR_ERROR("stubs: short write to '%s'", path.c_str());
        return false;
    }
    WR_INFO("stubs: wrote %s (%zu classes, %zu bytes)", path.c_str(),
            ClassDB::all().size(), text.size());
    return true;
}

}  // namespace wr
