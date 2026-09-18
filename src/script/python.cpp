#include "python.h"

#if WARREN_PYTHON

#include <cstring>
#include <filesystem>

#include "app/engine.h"
#include "core/log.h"
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
    {"group", py_group, METH_VARARGS, "Every node in a group."},
    {"key_down", py_key_down, METH_VARARGS, "Is a key held?"},
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
    return run_string(source, path.c_str());
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
