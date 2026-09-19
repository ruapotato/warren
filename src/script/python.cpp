#include "python.h"

#if WARREN_PYTHON

#include <cstring>
#include <filesystem>

#include "app/engine.h"
#include "core/log.h"
#include "core/variant_json.h"
#include "procgen/sdf.h"
#include "procgen/texture.h"
#include "render/environment.h"
#include "resource/resource.h"
#include "render/material.h"
#include "render/mesh.h"
#include "platform/input.h"
#include "pyvalue.h"
#include "scene/scene_tree.h"

namespace wr {
namespace {

Engine *g_engine = nullptr;
SceneTree *g_tree = nullptr;
PyObject *g_module = nullptr;
bool g_running = false;

using python::from_python;
using python::object_to_python;
using python::to_python;

// THE BRIDGE DOES NOT REQUIRE AN ENGINE.
//
// A script reaches the world through the scene tree, and a scene tree
// is a smaller thing than an engine: no window, no device, no frame
// loop. Keeping the dependency at the tree is what lets the tests run
// the real binding headless, and lets an embedder drive one without
// handing over the whole engine.
SceneTree *current_tree() {
    if (g_tree) return g_tree;
    return g_engine ? g_engine->tree() : nullptr;
}

// --------------------------------------------------------- the module

PyObject *py_log(PyObject *, PyObject *args) {
    const char *s = nullptr;
    if (!PyArg_ParseTuple(args, "s", &s)) return nullptr;
    WR_INFO("%s", s);
    Py_RETURN_NONE;
}
PyObject *py_warn(PyObject *, PyObject *args) {
    const char *s = nullptr;
    if (!PyArg_ParseTuple(args, "s", &s)) return nullptr;
    WR_WARN("%s", s);
    Py_RETURN_NONE;
}
PyObject *py_error(PyObject *, PyObject *args) {
    const char *s = nullptr;
    if (!PyArg_ParseTuple(args, "s", &s)) return nullptr;
    WR_ERROR("%s", s);
    Py_RETURN_NONE;
}

PyObject *py_root(PyObject *, PyObject *) {
    SceneTree *t = current_tree();
    if (!t) Py_RETURN_NONE;
    return object_to_python(t->root());
}

PyObject *py_scene(PyObject *, PyObject *) {
    SceneTree *t = current_tree();
    if (!t) Py_RETURN_NONE;
    return object_to_python(t->scene());
}

PyObject *py_physics(PyObject *, PyObject *) {
    if (!g_engine) Py_RETURN_NONE;
    return object_to_python(g_engine->physics());
}

PyObject *py_camera(PyObject *, PyObject *) {
    SceneTree *t = current_tree();
    if (!t) Py_RETURN_NONE;
    return object_to_python((Object *)t->active_camera());
}

PyObject *py_instantiate(PyObject *, PyObject *args) {
    const char *name = nullptr;
    if (!PyArg_ParseTuple(args, "s", &name)) return nullptr;
    Object *o = ClassDB::instantiate(name);
    if (!o) {
        PyErr_Format(PyExc_ValueError, "no class named '%s'", name);
        return nullptr;
    }
    return object_to_python(o);
}

// A SHAPE, FROM A DICT.
//
// The same grammar the agent protocol takes, because it is the same
// parser: a script and a program driving the engine from outside
// should not have two shape languages to learn between them.
//
//     import warren
//     mesh = warren.shape({"op": "difference", "of": [
//         {"shape": "box", "size": [1, 1, 1], "round": 0.05},
//         {"shape": "sphere", "radius": 0.6}]})
PyObject *py_shape(PyObject *, PyObject *args, PyObject *kwargs) {
    static const char *keywords[] = {"spec", "cell_size", "detail", "smooth", nullptr};
    PyObject *spec = nullptr;
    float cell_size = 0.0f;
    int detail = 48;
    int smooth = 0;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|fip",
                                     const_cast<char **>(keywords), &spec,
                                     &cell_size, &detail, &smooth))
        return nullptr;
    Variant v;
    if (!from_python(spec, &v)) {
        PyErr_SetString(PyExc_TypeError, "shape() takes a dict");
        return nullptr;
    }
    std::string error;
    gen::Sdf sdf = gen::Sdf::from_json(json_from_variant(v), &error);
    if (!error.empty()) {
        PyErr_SetString(PyExc_ValueError, error.c_str());
        return nullptr;
    }
    gen::Sdf::MeshOptions options;
    options.cell_size = cell_size;
    options.target_cells = detail;
    options.smooth_normals = smooth != 0;
    Ref<Mesh> mesh = sdf.to_mesh(options, &error);
    if (!mesh) {
        PyErr_SetString(PyExc_ValueError, error.c_str());
        return nullptr;
    }
    // The Ref goes out of scope here; object_to_python takes its own.
    return object_to_python(mesh.get());
}

// A SURFACE, FROM A DICT -- the companion to shape().
//
//     warren.surface({"pattern": "bricks", "rows": 8,
//                     "colours": ["#3a3632", "#c08a63"]})
//
// Returns a Material with albedo, normal and roughness maps already
// uploaded. Without a device -- a headless tool -- it writes the
// four maps as PNGs instead, which is the only useful thing left to
// do and better than an exception.
PyObject *py_surface(PyObject *, PyObject *args, PyObject *kwargs) {
    static const char *keywords[] = {"spec", "size", "save", nullptr};
    PyObject *spec = nullptr;
    int size = 256;
    const char *save = nullptr;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|is",
                                     const_cast<char **>(keywords), &spec, &size,
                                     &save))
        return nullptr;
    Variant v;
    if (!from_python(spec, &v)) {
        PyErr_SetString(PyExc_TypeError, "surface() takes a dict");
        return nullptr;
    }
    const Json json = json_from_variant(v);
    const uint32_t n = uint32_t(std::clamp(size, 16, 2048));
    std::string error;

    if (save) {
        const gen::SurfaceImages images = gen::render_surface(json, n, &error);
        if (!error.empty()) {
            PyErr_SetString(PyExc_ValueError, error.c_str());
            return nullptr;
        }
        const std::string stem = save;
        images.albedo.write_png(stem + "_albedo.png");
        images.normal.write_png(stem + "_normal.png");
        images.orm.write_png(stem + "_orm.png");
        images.height.write_png(stem + "_height.png");
    }

    rhi::Device *device = g_engine ? g_engine->device() : resource_device();
    if (!device) {
        if (save) Py_RETURN_NONE;
        PyErr_SetString(PyExc_RuntimeError,
                        "no render device; pass save=\"name\" to write the maps "
                        "as PNGs instead");
        return nullptr;
    }
    Ref<Material> material = gen::make_material(device, json, n, &error);
    if (!material) {
        PyErr_SetString(PyExc_ValueError, error.c_str());
        return nullptr;
    }
    return object_to_python(material.get());
}

PyObject *py_classes(PyObject *, PyObject *) {
    std::vector<ClassInfo *> all = ClassDB::all();
    PyObject *list = PyList_New(Py_ssize_t(all.size()));
    for (size_t i = 0; i < all.size(); i++)
        PyList_SET_ITEM(list, Py_ssize_t(i),
                        PyUnicode_FromString(all[i]->name.c_str()));
    return list;
}

PyObject *py_key_down(PyObject *, PyObject *args) {
    int code = 0;
    if (!PyArg_ParseTuple(args, "i", &code)) return nullptr;
    return PyBool_FromLong(Input::key_down(Key(code)) ? 1 : 0);
}
PyObject *py_load(PyObject *, PyObject *args) {
    const char *path = nullptr;
    const char *hint = "";
    if (!PyArg_ParseTuple(args, "s|s", &path, &hint)) return nullptr;
    // The cache is part of the contract: loading a path twice gives
    // the same object, not two equal ones, which is what lets a
    // hundred bodies share one mesh and one material.
    Ref<Resource> r = ResourceLoader::load(path, hint);
    if (!r) {
        WR_WARN("python: nothing could load '%s'", path);
        Py_RETURN_NONE;
    }
    return object_to_python(r.get());
}

PyObject *py_screenshot(PyObject *, PyObject *args) {
    // WRITE THE FRAME TO A FILE, from the game.
    //
    // --shot has always been able to do this from the command
    // line, which is no use at all to somebody who has just SEEN
    // the thing they want to report: by the time they can get to
    // a terminal the moment is gone. A key that saves the frame
    // turns "the shirt looks torn when I run" into a picture of
    // the shirt looking torn when they run.
    const char *path = nullptr;
    if (!PyArg_ParseTuple(args, "s", &path)) return nullptr;
    if (!g_engine) Py_RETURN_FALSE;
    return PyBool_FromLong(g_engine->save_screenshot(path) ? 1 : 0);
}

PyObject *py_mouse_wheel(PyObject *, PyObject *) {
    // How far the wheel turned this frame. Tracked by the input
    // layer since it was written and never reachable from a
    // script, which meant a game could not bind the one control
    // everybody uses for "cycle the thing in my hand".
    return PyFloat_FromDouble(double(Input::wheel()));
}

PyObject *py_environment(PyObject *, PyObject *) {
    // One per engine, made on first ask and kept -- a script that
    // holds on to it must keep working, and handing back a fresh
    // proxy each time would make `env = wr.environment()` in _ready
    // a subtly dead object.
    static Ref<Environment> env;
    if (!g_engine || !g_engine->renderer()) Py_RETURN_NONE;
    if (!env) env = Ref<Environment>(new Environment());
    env->bind_renderer(g_engine->renderer());
    return object_to_python(env.get());
}

PyObject *py_key_pressed(PyObject *, PyObject *args) {
    int code = 0;
    if (!PyArg_ParseTuple(args, "i", &code)) return nullptr;
    return PyBool_FromLong(Input::key_just_pressed(Key(code)) ? 1 : 0);
}
PyObject *py_key_released(PyObject *, PyObject *args) {
    int code = 0;
    if (!PyArg_ParseTuple(args, "i", &code)) return nullptr;
    return PyBool_FromLong(Input::key_just_released(Key(code)) ? 1 : 0);
}
PyObject *py_mouse_down(PyObject *, PyObject *args) {
    int b = 1;
    if (!PyArg_ParseTuple(args, "i", &b)) return nullptr;
    return PyBool_FromLong(Input::mouse_down(MouseButton(b)) ? 1 : 0);
}
PyObject *py_mouse_pressed(PyObject *, PyObject *args) {
    int b = 1;
    if (!PyArg_ParseTuple(args, "i", &b)) return nullptr;
    return PyBool_FromLong(Input::mouse_just_pressed(MouseButton(b)) ? 1 : 0);
}
PyObject *py_mouse_released(PyObject *, PyObject *args) {
    int b = 1;
    if (!PyArg_ParseTuple(args, "i", &b)) return nullptr;
    return PyBool_FromLong(Input::mouse_just_released(MouseButton(b)) ? 1 : 0);
}
PyObject *py_mouse_captured(PyObject *, PyObject *) {
    Window *w = g_engine ? g_engine->window() : nullptr;
    return PyBool_FromLong(w && w->mouse_captured() ? 1 : 0);
}
PyObject *py_capture_mouse(PyObject *, PyObject *args) {
    int on = 1;
    if (!PyArg_ParseTuple(args, "p", &on)) return nullptr;
    Window *w = g_engine ? g_engine->window() : nullptr;
    // A script that asks to capture the mouse with no window is not
    // making a mistake -- it is running headless, in a test or under
    // the agent -- so this is silently nothing rather than an error.
    if (w) w->set_mouse_captured(on != 0);
    Py_RETURN_NONE;
}
PyObject *py_action_down(PyObject *, PyObject *args) {
    const char *a = nullptr;
    if (!PyArg_ParseTuple(args, "s", &a)) return nullptr;
    return PyBool_FromLong(Input::action_down(a) ? 1 : 0);
}
PyObject *py_action_pressed(PyObject *, PyObject *args) {
    const char *a = nullptr;
    if (!PyArg_ParseTuple(args, "s", &a)) return nullptr;
    return PyBool_FromLong(Input::action_just_pressed(a) ? 1 : 0);
}
PyObject *py_bind_action(PyObject *, PyObject *args) {
    const char *a = nullptr;
    int code = 0;
    if (!PyArg_ParseTuple(args, "si", &a, &code)) return nullptr;
    Input::bind_action(a, Key(code));
    Py_RETURN_NONE;
}
PyObject *py_mouse_motion(PyObject *, PyObject *) {
    return to_python(Variant(Input::mouse_motion()));
}
PyObject *py_action_vector(PyObject *, PyObject *args) {
    const char *nx, *px, *ny, *py;
    if (!PyArg_ParseTuple(args, "ssss", &nx, &px, &ny, &py)) return nullptr;
    return to_python(Variant(Input::action_vector(nx, px, ny, py)));
}

PyObject *py_time(PyObject *, PyObject *) {
    SceneTree *t = current_tree();
    return PyFloat_FromDouble(t ? t->time() : 0.0);
}
PyObject *py_fps(PyObject *, PyObject *) {
    return PyFloat_FromDouble(g_engine ? double(g_engine->clock().fps()) : 0.0);
}
PyObject *py_frame(PyObject *, PyObject *) {
    return PyLong_FromUnsignedLongLong(g_engine ? g_engine->frames() : 0);
}

PyObject *py_group(PyObject *, PyObject *args) {
    const char *g = nullptr;
    if (!PyArg_ParseTuple(args, "s", &g)) return nullptr;
    SceneTree *t = current_tree();
    if (!t) return PyList_New(0);
    const std::vector<Node *> &nodes = t->group(g);
    PyObject *list = PyList_New(Py_ssize_t(nodes.size()));
    for (size_t i = 0; i < nodes.size(); i++)
        PyList_SET_ITEM(list, Py_ssize_t(i), object_to_python(nodes[i]));
    return list;
}

PyMethodDef k_module_methods[] = {
    {"log", py_log, METH_VARARGS, "Write a line to the engine log."},
    {"warn", py_warn, METH_VARARGS, "Write a warning."},
    {"error", py_error, METH_VARARGS, "Write an error."},
    {"root", py_root, METH_NOARGS, "The scene tree's root node."},
    {"scene", py_scene, METH_NOARGS, "The current scene's root node."},
    {"physics", py_physics, METH_NOARGS, "The physics world."},
    {"camera", py_camera, METH_NOARGS, "The active camera."},
    {"instantiate", py_instantiate, METH_VARARGS,
     "Make an instance of an engine class by name."},
    {"classes", py_classes, METH_NOARGS, "Every registered class name."},
    {"shape", (PyCFunction)py_shape, METH_VARARGS | METH_KEYWORDS,
     "Build a Mesh from a procedural shape description."},
    {"surface", (PyCFunction)py_surface, METH_VARARGS | METH_KEYWORDS,
     "Build a Material from a procedural surface description."},
    {"group", py_group, METH_VARARGS, "Every node in a group."},
    {"load", py_load, METH_VARARGS,
     "Load a resource by path. Cached: the same path is the same object."},
    {"environment", py_environment, METH_NOARGS,
     "The sky, the sun and the fog."},
    {"key_down", py_key_down, METH_VARARGS, "Is a key held?"},
    {"key_pressed", py_key_pressed, METH_VARARGS,
     "Was a key pressed this frame?"},
    {"key_released", py_key_released, METH_VARARGS,
     "Was a key released this frame?"},
    {"screenshot", py_screenshot, METH_VARARGS,
     "Write the last frame to a PNG. Returns whether it worked."},
    {"mouse_wheel", py_mouse_wheel, METH_NOARGS,
     "How far the wheel turned this frame, in notches."},
    {"mouse_down", py_mouse_down, METH_VARARGS, "Is a mouse button held?"},
    {"mouse_pressed", py_mouse_pressed, METH_VARARGS,
     "Was a mouse button pressed this frame?"},
    {"mouse_released", py_mouse_released, METH_VARARGS,
     "Was a mouse button released this frame?"},
    {"mouse_captured", py_mouse_captured, METH_NOARGS,
     "Is the pointer locked to the window?"},
    {"capture_mouse", py_capture_mouse, METH_VARARGS,
     "Lock or release the pointer."},
    {"action_down", py_action_down, METH_VARARGS, "Is an action held?"},
    {"action_pressed", py_action_pressed, METH_VARARGS,
     "Was an action pressed this frame?"},
    {"action_vector", py_action_vector, METH_VARARGS,
     "A movement vector from four actions."},
    {"bind_action", py_bind_action, METH_VARARGS, "Bind a key to an action."},
    {"mouse_motion", py_mouse_motion, METH_NOARGS,
     "Mouse movement since last frame."},
    {"time", py_time, METH_NOARGS, "Seconds since the scene started."},
    {"fps", py_fps, METH_NOARGS, "Smoothed frames per second."},
    {"frame", py_frame, METH_NOARGS, "The frame number."},
    {nullptr, nullptr, 0, nullptr}};

PyModuleDef k_module = {PyModuleDef_HEAD_INIT,
                        "warren",
                        "The Warren engine.",
                        -1,
                        k_module_methods,
                        nullptr,
                        nullptr,
                        nullptr,
                        nullptr};

PyObject *init_module() {
    PyObject *m = PyModule_Create(&k_module);
    if (!m) return nullptr;
    if (!python::register_value_types(m)) return nullptr;
    if (!python::register_object_types(m)) return nullptr;

    // The key codes, so a script can say mf.Key.W rather than 26.
    PyObject *keys = PyDict_New();
    struct { const char *name; int code; } k[] = {
        {"A", int(Key::A)}, {"B", int(Key::B)}, {"C", int(Key::C)},
        {"D", int(Key::D)}, {"E", int(Key::E)}, {"F", int(Key::F)},
        {"G", int(Key::G)}, {"H", int(Key::H)}, {"I", int(Key::I)},
        {"J", int(Key::J)}, {"K", int(Key::K)}, {"L", int(Key::L)},
        {"M", int(Key::M)}, {"N", int(Key::N)}, {"O", int(Key::O)},
        {"P", int(Key::P)}, {"Q", int(Key::Q)}, {"R", int(Key::R)},
        {"S", int(Key::S)}, {"T", int(Key::T)}, {"U", int(Key::U)},
        {"V", int(Key::V)}, {"W", int(Key::W)}, {"X", int(Key::X)},
        {"Y", int(Key::Y)}, {"Z", int(Key::Z)},
        {"SPACE", int(Key::Space)}, {"ESCAPE", int(Key::Escape)},
        {"SHIFT", int(Key::LeftShift)}, {"CTRL", int(Key::LeftCtrl)},
        {"TAB", int(Key::Tab)}, {"RETURN", int(Key::Return)},
        {"UP", int(Key::Up)}, {"DOWN", int(Key::Down)},
        {"LEFT", int(Key::Left)}, {"RIGHT", int(Key::Right)},
        {"F1", int(Key::F1)}, {"F2", int(Key::F2)}, {"F3", int(Key::F3)},
        {"F4", int(Key::F4)}, {"F5", int(Key::F5)},
        // The number row, which any game with weapon slots needs and
        // which was the one gap a real game found first.
        {"NUM1", int(Key::Num1)}, {"NUM2", int(Key::Num2)},
        {"NUM3", int(Key::Num3)}, {"NUM4", int(Key::Num4)},
        {"NUM5", int(Key::Num5)}, {"NUM6", int(Key::Num6)},
        {"NUM7", int(Key::Num7)}, {"NUM8", int(Key::Num8)},
        {"NUM9", int(Key::Num9)}, {"NUM0", int(Key::Num0)},
        {"LSHIFT", int(Key::LeftShift)}, {"RSHIFT", int(Key::RightShift)},
        {"LCTRL", int(Key::LeftCtrl)}, {"RCTRL", int(Key::RightCtrl)},
        {"ALT", int(Key::LeftAlt)}, {"BACKSPACE", int(Key::Backspace)},
        {"DELETE", int(Key::Delete)}, {"HOME", int(Key::Home)},
        {"END", int(Key::End)}, {"COMMA", int(Key::Comma)},
        {"PERIOD", int(Key::Period)}, {"GRAVE", int(Key::Grave)},
    };
    for (const auto &e : k) {
        PyObject *v = PyLong_FromLong(e.code);
        PyDict_SetItemString(keys, e.name, v);
        Py_DECREF(v);
    }
    PyObject *ns = PyImport_ImportModule("types");
    if (ns) {
        PyObject *simple = PyObject_GetAttrString(ns, "SimpleNamespace");
        if (simple) {
            PyObject *obj = PyObject_Call(simple, PyTuple_New(0), keys);
            if (obj) PyModule_AddObject(m, "Key", obj);
            Py_DECREF(simple);
        }
        Py_DECREF(ns);
    }
    Py_DECREF(keys);

    // The mouse buttons, by the same route and for the same reason.
    PyObject *buttons = PyDict_New();
    struct { const char *name; int code; } mb[] = {
        {"LEFT", int(MouseButton::Left)},
        {"MIDDLE", int(MouseButton::Middle)},
        {"RIGHT", int(MouseButton::Right)},
        {"X1", int(MouseButton::X1)}, {"X2", int(MouseButton::X2)},
    };
    for (const auto &e : mb) {
        PyObject *v = PyLong_FromLong(e.code);
        PyDict_SetItemString(buttons, e.name, v);
        Py_DECREF(v);
    }
    PyObject *ns2 = PyImport_ImportModule("types");
    if (ns2) {
        PyObject *simple = PyObject_GetAttrString(ns2, "SimpleNamespace");
        if (simple) {
            PyObject *obj = PyObject_Call(simple, PyTuple_New(0), buttons);
            if (obj) PyModule_AddObject(m, "Mouse", obj);
            Py_DECREF(simple);
        }
        Py_DECREF(ns2);
    }
    Py_DECREF(buttons);

    PyModule_AddStringConstant(m, "version", "0.1.0");
    // A module initialiser that returns a module with an exception
    // still set is reported as "initialization raised unreported
    // exception", which names neither the module's fault nor the
    // line. Anything non-fatal above is cleared here.
    if (PyErr_Occurred()) {
        WR_WARN("python: the warren module set an error while initialising");
        PyErr_Print();
        PyErr_Clear();
    }
    return m;
}

void print_error(const char *what) {
    if (!PyErr_Occurred()) return;
    WR_ERROR("python: %s", what);
    PyErr_Print();
}

}  // namespace

// ------------------------------------------------------------ lifetime

bool Python::init(Engine *engine, const std::vector<std::string> &search_paths) {
    if (g_running) return true;
    g_engine = engine;

    // BEFORE Py_Initialize, or the module cannot be imported from a
    // script that runs during start-up.
    if (PyImport_AppendInittab("warren", init_module) == -1) {
        WR_ERROR("python: could not register the warren module");
        return false;
    }

    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    // Isolated, but not sealed off: a game's scripts are not
    // untrusted input, and they legitimately want the standard
    // library.
    config.site_import = 1;
    config.use_environment = 0;
    config.install_signal_handlers = 0;
    // The engine handles ctrl-c through the window, and a Python
    // signal handler here would swallow it.
    PyStatus status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) {
        WR_ERROR("python: %s", status.err_msg ? status.err_msg : "failed to start");
        return false;
    }

    g_module = PyImport_ImportModule("warren");
    if (!g_module) {
        print_error("the warren module would not import");
        return false;
    }
    g_running = true;

    // PRINT GOES TO THE ENGINE LOG, NOT TO THE PROCESS'S STDOUT.
    //
    // Two reasons, and either alone would be enough. A script's
    // print() should appear in the editor console, where the person
    // running it is looking, and until now it did not. And when the
    // engine is being driven over stdout by a program -- the agent
    // interface -- a bare print() in a script is not output, it is a
    // corrupted reply and a dead session.
    //
    // Buffered to the newline because print() writes the text and the
    // newline as two calls, and a log line per call would break every
    // message in half. stderr goes to warn rather than error so that
    // a script writing to it does not make error_count(), which the
    // demos and tests use as a pass/fail, report a failure.
    static const char *kRedirect =
        "import sys, warren\n"
        "class _WarrenOut:\n"
        "    def __init__(self, sink):\n"
        "        self._sink = sink\n"
        "        self._buf = ''\n"
        "    def write(self, text):\n"
        "        self._buf += text\n"
        "        while '\\n' in self._buf:\n"
        "            line, self._buf = self._buf.split('\\n', 1)\n"
        "            self._sink(line)\n"
        "        return len(text)\n"
        "    def flush(self):\n"
        "        if self._buf:\n"
        "            self._sink(self._buf)\n"
        "            self._buf = ''\n"
        "    def isatty(self):\n"
        "        return False\n"
        "sys.stdout = _WarrenOut(warren.log)\n"
        "sys.stderr = _WarrenOut(warren.warn)\n";
    if (PyRun_SimpleString(kRedirect) != 0) {
        PyErr_Clear();
        WR_WARN("python: could not redirect print() to the log");
    }

    for (const std::string &p : search_paths) add_search_path(p);
    WR_INFO("python: %s, %zu classes exposed", version().c_str(),
            ClassDB::all().size());
    return true;
}

void Python::set_tree(SceneTree *tree) { g_tree = tree; }

void Python::shutdown() {
    if (!g_running) return;
    Py_XDECREF(g_module);
    g_module = nullptr;
    Py_Finalize();
    g_running = false;
    g_engine = nullptr;
    g_tree = nullptr;
}

bool Python::running() { return g_running; }

void Python::refresh_classes() {
    if (!g_running || !g_module) return;
    python::refresh_object_types(g_module);
}

void Python::add_search_path(const std::string &path) {
    if (!g_running) return;
    PyObject *sys = PyImport_ImportModule("sys");
    if (!sys) return;
    PyObject *sys_path = PyObject_GetAttrString(sys, "path");
    if (sys_path) {
        PyObject *p = PyUnicode_FromString(path.c_str());
        PyList_Insert(sys_path, 0, p);
        Py_DECREF(p);
        Py_DECREF(sys_path);
    }
    Py_DECREF(sys);
}

bool Python::run_string(const std::string &source, const char *name) {
    if (!g_running) return false;
    PyObject *main = PyImport_AddModule("__main__");
    if (!main) return false;
    PyObject *globals = PyModule_GetDict(main);
    PyObject *result = PyRun_StringFlags(source.c_str(), Py_file_input, globals,
                                         globals, nullptr);
    if (!result) {
        print_error(name);
        return false;
    }
    Py_DECREF(result);
    return true;
}

bool Python::run_file(const std::string &path) {
    if (!g_running) return false;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        WR_ERROR("python: no such file '%s'", path.c_str());
        return false;
    }
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
        WR_ERROR("python: could not open '%s'", path.c_str());
        return false;
    }
    std::string source;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) source.append(buf, n);
    std::fclose(f);

    // __file__, because a script run from a file is entitled to know
    // where it is. A game's entry point is the one file that has to
    // find its own package, and without this the first line of every
    // one of them is a guess about the working directory.
    //
    // Also sys.path, with the script's own directory on it -- the
    // rule every Python runtime follows, and the reason `python
    // game/main.py` can `import game`.
    std::error_code abs_ec;
    std::filesystem::path full = std::filesystem::absolute(path, abs_ec);
    const std::string file = abs_ec ? path : full.string();
    const std::string dir =
        abs_ec ? std::string(".") : full.parent_path().string();
    add_search_path(dir);

    PyObject *main = PyImport_AddModule("__main__");
    if (main) {
        PyObject *v = PyUnicode_FromString(file.c_str());
        if (v) {
            PyObject_SetAttrString(main, "__file__", v);
            Py_DECREF(v);
        }
    }
    bool ok = run_string(source, path.c_str());
    // Left in place afterwards on purpose: a script that defines a
    // class and hands it to the tree is still running long after
    // this returns, and it may import at any point.
    return ok;
}

bool Python::attach_script(Node *node, const std::string &module_or_path) {
    if (!g_running || !node) return false;

    std::string module = module_or_path;
    std::filesystem::path p(module_or_path);
    if (p.extension() == ".py") {
        add_search_path(p.parent_path().string());
        module = p.stem().string();
    }

    PyObject *mod = PyImport_ImportModule(module.c_str());
    if (!mod) {
        print_error(("importing " + module).c_str());
        return false;
    }
    // THE CLASS IS NAMED AFTER THE FILE, or the file names it. One
    // file, one script, the way a scene tree wants it.
    PyObject *cls = PyObject_GetAttrString(mod, module.c_str());
    if (!cls) {
        PyErr_Clear();
        cls = PyObject_GetAttrString(mod, "Script");
    }
    if (!cls) {
        WR_ERROR("python: '%s' defines neither a class '%s' nor 'Script'",
                 module.c_str(), module.c_str());
        Py_DECREF(mod);
        return false;
    }

    // The node already exists, so the class is instantiated WITHOUT
    // creating a second engine object: the Python object is made and
    // then bound to the node that is already there.
    PyObject *peer = object_to_python(node);
    if (!peer) {
        Py_DECREF(cls);
        Py_DECREF(mod);
        return false;
    }
    PyObject *instance = PyObject_CallNoArgs(cls);
    Py_DECREF(cls);
    Py_DECREF(mod);
    if (!instance) {
        print_error(("instantiating " + module).c_str());
        Py_DECREF(peer);
        return false;
    }
    // COPY THE SCRIPT'S MEMBERS ONTO THE NODE'S OWN WRAPPER.
    //
    // The node already exists, so the script class cannot be the
    // thing that creates it. Instead its attributes and methods are
    // moved across, and `self` inside them is then the node -- which
    // is what a script attached to an existing node has to mean.
    PyObject *names = PyObject_Dir(instance);
    if (names) {
        const Py_ssize_t n = PyList_Size(names);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *key = PyList_GetItem(names, i);
            const char *k = PyUnicode_AsUTF8(key);
            if (!k) continue;
            // Public members, plus the four the engine calls.
            const bool lifecycle =
                !std::strcmp(k, "_ready") || !std::strcmp(k, "_process") ||
                !std::strcmp(k, "_physics_process") || !std::strcmp(k, "_exit");
            if (k[0] == '_' && !lifecycle) continue;
            PyObject *attr = PyObject_GetAttr(instance, key);
            if (!attr) {
                PyErr_Clear();
                continue;
            }
            if (PyObject_SetAttr(peer, key, attr) < 0) PyErr_Clear();
            Py_DECREF(attr);
        }
        Py_DECREF(names);
    }
    Py_DECREF(instance);

    node->set_script(new PythonScript(peer, module));
    Py_DECREF(peer);
    return true;
}

std::string Python::eval_repr(const std::string &expression, bool *ok) {
    if (ok) *ok = false;
    if (!g_running) return "<python is not running>";
    PyObject *main = PyImport_AddModule("__main__");
    PyObject *globals = PyModule_GetDict(main);
    PyObject *result =
        PyRun_String(expression.c_str(), Py_eval_input, globals, globals);
    if (!result) {
        PyObject *type = nullptr, *value = nullptr, *tb = nullptr;
        PyErr_Fetch(&type, &value, &tb);
        PyObject *s = value ? PyObject_Str(value) : nullptr;
        std::string msg = s ? PyUnicode_AsUTF8(s) : "error";
        Py_XDECREF(s);
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(tb);
        return msg;
    }
    PyObject *repr = PyObject_Repr(result);
    std::string out = repr ? PyUnicode_AsUTF8(repr) : "?";
    Py_XDECREF(repr);
    Py_DECREF(result);
    if (ok) *ok = true;
    return out;
}

std::string Python::version() {
    const char *v = Py_GetVersion();
    std::string s = v ? v : "unknown";
    size_t space = s.find(' ');
    return "Python " + (space == std::string::npos ? s : s.substr(0, space));
}

std::string Python::report() {
    if (!g_running) return "python: not running";
    return "python: " + version() + ", " +
           std::to_string(ClassDB::all().size()) + " classes exposed";
}

}  // namespace wr

#endif  // WARREN_PYTHON
