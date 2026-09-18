// Warren -- engine objects in Python.
//
// Every ClassDB class gets a Python type, built from the registry
// rather than written by hand, with the same inheritance. A Python
// class may subclass one, and when it does, the engine object is
// created underneath it and its `_ready`, `_process` and
// `_physics_process` are called by the scene tree -- which is the
// whole of what a scripting language has to do.
#include "pyvalue.h"

#if WARREN_PYTHON

#include <cstring>
#include <string>
#include <unordered_map>

#include "core/log.h"
#include "scene/node.h"

namespace wr::python {
namespace {

struct ObjectWrapper {
    PyObject_HEAD
    Object *obj;
    PyObject *dict;
    PyObject *weakrefs;
};

std::unordered_map<Object *, PyObject *> g_wrappers;
std::unordered_map<ClassInfo *, PyTypeObject *> g_types;
PyObject *g_module = nullptr;

// --------------------------------------------------------- bound method

struct BoundMethod {
    PyObject_HEAD
    Object *obj;
    const MethodInfo *info;
};

PyTypeObject *g_bound_method_type = nullptr;

void bound_dealloc(PyObject *self) {
    BoundMethod *b = (BoundMethod *)self;
    if (b->obj) b->obj->ref_release();
    PyTypeObject *t = Py_TYPE(self);
    PyObject_Free(self);
    Py_DECREF(t);
}

PyObject *bound_call(PyObject *self, PyObject *args, PyObject *kwargs) {
    BoundMethod *b = (BoundMethod *)self;
    if (kwargs && PyDict_Size(kwargs) > 0) {
        PyErr_SetString(PyExc_TypeError,
                        "engine methods take positional arguments only");
        return nullptr;
    }
    if (!b->obj) {
        PyErr_SetString(PyExc_ReferenceError, "the object has been freed");
        return nullptr;
    }
    const Py_ssize_t n = PyTuple_GET_SIZE(args);
    std::vector<Variant> argv;
    argv.reserve(size_t(n));
    for (Py_ssize_t i = 0; i < n; i++) {
        Variant v;
        if (!from_python(PyTuple_GET_ITEM(args, i), &v)) return nullptr;
        argv.push_back(std::move(v));
    }
    CallError err = CallError::Ok;
    Variant result = b->obj->call(b->info->name, argv.data(), int(argv.size()), &err);
    if (err != CallError::Ok) {
        PyErr_Format(PyExc_TypeError, "%s.%s: %s", b->obj->get_class_name(),
                     b->info->name.c_str(), call_error_name(err));
        return nullptr;
    }
    return to_python(result);
}

PyObject *bound_repr(PyObject *self) {
    BoundMethod *b = (BoundMethod *)self;
    return PyUnicode_FromFormat("<engine method %s.%s>",
                                b->obj ? b->obj->get_class_name() : "?",
                                b->info->name.c_str());
}

PyType_Slot k_bound_slots[] = {{Py_tp_dealloc, (void *)bound_dealloc},
                               {Py_tp_call, (void *)bound_call},
                               {Py_tp_repr, (void *)bound_repr},
                               {0, nullptr}};

PyObject *make_bound(Object *o, const MethodInfo *info) {
    BoundMethod *b = PyObject_New(BoundMethod, g_bound_method_type);
    if (!b) return nullptr;
    b->obj = o;
    if (o) o->ref_retain();
    b->info = info;
    return (PyObject *)b;
}

// ------------------------------------------------------------- the type

// The nearest engine class in a type's ancestry. For `mf.Node3D`
// itself that is Node3D; for a Python subclass of it, still Node3D --
// which is what has to be instantiated underneath.
ClassInfo *engine_class_of(PyTypeObject *type) {
    for (PyTypeObject *t = type; t; t = t->tp_base)
        for (auto &kv : g_types)
            if (kv.second == t) return kv.first;
    return nullptr;
}

void wrapper_dealloc(PyObject *self) {
    ObjectWrapper *w = (ObjectWrapper *)self;
    if (w->weakrefs) PyObject_ClearWeakRefs(self);
    if (w->obj) {
        g_wrappers.erase(w->obj);
        w->obj->script_peer = nullptr;
        w->obj->ref_release();
        w->obj = nullptr;
    }
    Py_CLEAR(w->dict);
    PyTypeObject *t = Py_TYPE(self);
    freefunc free_fn = (freefunc)PyType_GetSlot(t, Py_tp_free);
    if (free_fn) free_fn(self);
    else PyObject_Free(self);
    Py_DECREF(t);
}

PyObject *wrapper_repr(PyObject *self) {
    ObjectWrapper *w = (ObjectWrapper *)self;
    if (!w->obj) return PyUnicode_FromString("<freed engine object>");
    return PyUnicode_FromFormat("<%s %s>", w->obj->get_class_name(),
                                w->obj->is_class("Node")
                                    ? ((Node *)w->obj)->name().c_str()
                                    : "");
}

PyObject *wrapper_getattro(PyObject *self, PyObject *name) {
    ObjectWrapper *w = (ObjectWrapper *)self;
    const char *n = PyUnicode_AsUTF8(name);
    if (!n) return nullptr;

    // A Python attribute or method wins: a script that defines
    // `speed` means its own, and a script that overrides a method
    // means its own.
    PyObject *result = PyObject_GenericGetAttr(self, name);
    if (result) return result;
    if (!PyErr_ExceptionMatches(PyExc_AttributeError)) return nullptr;
    PyErr_Clear();

    if (!w->obj) {
        PyErr_SetString(PyExc_ReferenceError, "the object has been freed");
        return nullptr;
    }
    ClassInfo *ci = w->obj->get_class_info();
    if (const PropertyInfo *pi = ci->find_property(n)) {
        if (!pi->get) {
            PyErr_Format(PyExc_AttributeError, "'%s' is write-only", n);
            return nullptr;
        }
        return to_python(pi->get(w->obj));
    }
    if (const MethodInfo *mi = ci->find_method(n)) return make_bound(w->obj, mi);

    PyErr_Format(PyExc_AttributeError, "'%s' has no attribute '%s'",
                 w->obj->get_class_name(), n);
    return nullptr;
}

int wrapper_setattro(PyObject *self, PyObject *name, PyObject *value) {
    ObjectWrapper *w = (ObjectWrapper *)self;
    const char *n = PyUnicode_AsUTF8(name);
    if (!n) return -1;
    if (w->obj) {
        ClassInfo *ci = w->obj->get_class_info();
        if (const PropertyInfo *pi = ci->find_property(n)) {
            if (!pi->set) {
                PyErr_Format(PyExc_AttributeError, "'%s' is read-only", n);
                return -1;
            }
            if (!value) {
                PyErr_Format(PyExc_AttributeError, "cannot delete '%s'", n);
                return -1;
            }
            Variant v;
            if (!from_python(value, &v)) return -1;
            pi->set(w->obj, v);
            return 0;
        }
    }
    // Anything else is the script's own business.
    return PyObject_GenericSetAttr(self, name, value);
}

PyObject *wrapper_richcompare(PyObject *a, PyObject *b, int op) {
    if (op != Py_EQ && op != Py_NE) Py_RETURN_NOTIMPLEMENTED;
    Object *oa = object_from_python(a);
    Object *ob = object_from_python(b);
    const bool same = oa && oa == ob;
    if ((op == Py_EQ) == same) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

Py_hash_t wrapper_hash(PyObject *self) {
    return Py_hash_t(uintptr_t(((ObjectWrapper *)self)->obj));
}

void attach_script_if_needed(Object *obj, PyObject *self);

PyObject *wrapper_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    ClassInfo *ci = engine_class_of(type);
    if (!ci) {
        PyErr_SetString(PyExc_TypeError, "not an engine class");
        return nullptr;
    }
    if (!ci->construct) {
        PyErr_Format(PyExc_TypeError, "%s is abstract", ci->name.c_str());
        return nullptr;
    }
    Object *obj = ci->construct();
    if (!obj) {
        PyErr_Format(PyExc_RuntimeError, "could not create a %s", ci->name.c_str());
        return nullptr;
    }

    allocfunc alloc = (allocfunc)PyType_GetSlot(type, Py_tp_alloc);
    ObjectWrapper *w = (ObjectWrapper *)(alloc ? alloc(type, 0)
                                               : PyType_GenericAlloc(type, 0));
    if (!w) {
        delete obj;
        return nullptr;
    }
    w->obj = obj;
    obj->ref_retain();
    w->dict = PyDict_New();
    w->weakrefs = nullptr;
    obj->script_peer = w;
    g_wrappers[obj] = (PyObject *)w;
    attach_script_if_needed(obj, (PyObject *)w);
    (void)args;
    (void)kwargs;
    return (PyObject *)w;
}

int wrapper_init(PyObject *, PyObject *, PyObject *) { return 0; }

PyObject *wrapper_get_dict(PyObject *self, void *) {
    ObjectWrapper *w = (ObjectWrapper *)self;
    if (!w->dict) w->dict = PyDict_New();
    Py_XINCREF(w->dict);
    return w->dict;
}

PyGetSetDef k_wrapper_getset[] = {
    {"__dict__", wrapper_get_dict, nullptr, nullptr, nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr}};

PyMemberDef k_wrapper_members[] = {
    {"__dictoffset__", Py_T_PYSSIZET, offsetof(ObjectWrapper, dict), Py_READONLY, nullptr},
    {nullptr, 0, 0, 0, nullptr}};

PyObject *wrapper_class_name(PyObject *self, PyObject *) {
    ObjectWrapper *w = (ObjectWrapper *)self;
    return PyUnicode_FromString(w->obj ? w->obj->get_class_name() : "<freed>");
}

PyObject *wrapper_is_valid(PyObject *self, PyObject *) {
    return PyBool_FromLong(((ObjectWrapper *)self)->obj != nullptr);
}

PyObject *wrapper_properties(PyObject *self, PyObject *) {
    ObjectWrapper *w = (ObjectWrapper *)self;
    if (!w->obj) Py_RETURN_NONE;
    std::vector<std::string> names = w->obj->property_list();
    PyObject *list = PyList_New(Py_ssize_t(names.size()));
    for (size_t i = 0; i < names.size(); i++)
        PyList_SET_ITEM(list, Py_ssize_t(i), PyUnicode_FromString(names[i].c_str()));
    return list;
}

PyMethodDef k_wrapper_methods[] = {
    {"class_name", wrapper_class_name, METH_NOARGS, "Its engine class name."},
    {"is_valid", wrapper_is_valid, METH_NOARGS, "False once the object is freed."},
    {"properties", wrapper_properties, METH_NOARGS,
     "Every engine property, base class first."},
    {nullptr, nullptr, 0, nullptr}};

}  // namespace

// --------------------------------------------------- the script bridge

}  // namespace wr::python

namespace wr {

// A STRONG REFERENCE, AND THE CYCLE IT MAKES IS BROKEN ON PURPOSE.
//
// The wrapper keeps the engine object alive and the engine object
// keeps this script alive, so the script holding the wrapper closes a
// loop that reference counting alone will never open. Holding a
// borrowed reference instead does not work: the Python variable that
// created the node goes out of scope at the end of the line that
// added it to the tree, the wrapper is freed, and the next _process
// calls into freed memory.
//
// So the reference is owned, and DROPPED WHEN THE NODE LEAVES THE
// TREE -- which is when a node is finished with, and the one moment
// both halves agree on. A node created in a script and never added
// to a tree is therefore kept alive by its own script, which is the
// same bargain every engine of this shape makes.
PythonScript::PythonScript(PyObject *peer, const std::string &name)
    : peer_(peer), name_(name) {
    Py_XINCREF(peer_);
    has_ready_ = PyObject_HasAttrString(peer, "_ready");
    has_process_ = PyObject_HasAttrString(peer, "_process");
    has_physics_ = PyObject_HasAttrString(peer, "_physics_process");
    has_exit_ = PyObject_HasAttrString(peer, "_exit");
}

PythonScript::~PythonScript() { release_peer(); }

void PythonScript::release_peer() {
    if (!peer_) return;
    PyObject *p = peer_;
    peer_ = nullptr;
    // A node outliving the interpreter is a shutdown-order mistake
    // somewhere, but touching a finalised interpreter turns that
    // mistake into a fatal error with no stack, so it is checked.
    if (Py_IsInitialized()) Py_DECREF(p);
}

void PythonScript::on_ready() { if (has_ready_) call("_ready", -1.0f); }
void PythonScript::on_process(float dt) { if (has_process_) call("_process", dt); }
void PythonScript::on_physics(float dt) {
    if (has_physics_) call("_physics_process", dt);
}
void PythonScript::on_exit() {
    if (has_exit_) call("_exit", -1.0f);
    release_peer();
}

void PythonScript::call(const char *method, float dt) {
    if (!peer_) return;
    PyObject *fn = PyObject_GetAttrString(peer_, method);
    if (!fn) {
        PyErr_Clear();
        return;
    }
    PyObject *result = dt >= 0.0f ? PyObject_CallFunction(fn, "f", double(dt))
                                  : PyObject_CallNoArgs(fn);
    Py_DECREF(fn);
    if (!result) {
        // PRINTED AND SWALLOWED. A script that throws in _process
        // must not take the frame loop with it, and it must not print
        // the same traceback sixty times a second either -- so the
        // offending callback is switched off.
        WR_ERROR("script '%s': %s raised; it will not be called again",
                 name_.c_str(), method);
        PyErr_Print();
        if (!std::strcmp(method, "_process")) has_process_ = false;
        else if (!std::strcmp(method, "_physics_process")) has_physics_ = false;
        else if (!std::strcmp(method, "_ready")) has_ready_ = false;
        return;
    }
    Py_DECREF(result);
}

}  // namespace wr

namespace wr::python {
namespace {

void attach_script_if_needed(Object *obj, PyObject *self) {
    Node *node = obj->cast_to<Node>();
    if (!node) return;
    // Only when the Python side actually defines something to call;
    // otherwise this is a plain wrapper and the indirection is waste.
    if (!PyObject_HasAttrString(self, "_ready") &&
        !PyObject_HasAttrString(self, "_process") &&
        !PyObject_HasAttrString(self, "_physics_process"))
        return;
    node->set_script(new PythonScript(self, Py_TYPE(self)->tp_name));
}

}  // namespace

// ------------------------------------------------------------- the types

PyTypeObject *class_type(ClassInfo *ci, PyObject *module) {
    if (!ci) return nullptr;
    auto it = g_types.find(ci);
    if (it != g_types.end()) return it->second;

    // The base first, so the Python hierarchy matches the C++ one and
    // isinstance(node, mf.Node) is true for a Node3D.
    PyObject *base = nullptr;
    if (ci->base) base = (PyObject *)class_type(ci->base, module);

    static PyType_Slot slots[] = {
        {Py_tp_dealloc, (void *)wrapper_dealloc},
        {Py_tp_repr, (void *)wrapper_repr},
        {Py_tp_getattro, (void *)wrapper_getattro},
        {Py_tp_setattro, (void *)wrapper_setattro},
        {Py_tp_richcompare, (void *)wrapper_richcompare},
        {Py_tp_hash, (void *)wrapper_hash},
        {Py_tp_new, (void *)wrapper_new},
        {Py_tp_init, (void *)wrapper_init},
        {Py_tp_methods, (void *)k_wrapper_methods},
        {Py_tp_getset, (void *)k_wrapper_getset},
        {Py_tp_members, (void *)k_wrapper_members},
        {0, nullptr}};

    std::string full = "warren." + ci->name;
    char *name = strdup(full.c_str());
    PyType_Spec spec{name, sizeof(ObjectWrapper), 0,
                     Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, slots};
    PyObject *bases = base ? PyTuple_Pack(1, base) : nullptr;
    PyObject *type = PyType_FromSpecWithBases(&spec, bases);
    Py_XDECREF(bases);
    if (!type) {
        WR_ERROR("python: could not build a type for %s", ci->name.c_str());
        PyErr_Print();
        return nullptr;
    }
    g_types[ci] = (PyTypeObject *)type;
    Py_INCREF(type);
    PyModule_AddObject(module, ci->name.c_str(), type);
    return (PyTypeObject *)type;
}

bool register_object_types(PyObject *module) {
    g_module = module;
    PyType_Spec bound{"warren.EngineMethod", sizeof(BoundMethod), 0,
                      Py_TPFLAGS_DEFAULT, k_bound_slots};
    PyObject *bt = PyType_FromSpec(&bound);
    if (!bt) return false;
    g_bound_method_type = (PyTypeObject *)bt;

    // Anything freed on the engine side must not leave Python holding
    // a pointer to it.
    Object::set_peer_destructor([](Object *o) { forget_object(o); });

    refresh_object_types(module);
    return true;
}

void refresh_object_types(PyObject *module) {
    for (ClassInfo *ci : ClassDB::all()) class_type(ci, module);
}

void forget_object(Object *o) {
    auto it = g_wrappers.find(o);
    if (it == g_wrappers.end()) return;
    ObjectWrapper *w = (ObjectWrapper *)it->second;
    // The engine object is going; the Python object may outlive it,
    // and must report itself invalid rather than follow the pointer.
    w->obj = nullptr;
    g_wrappers.erase(it);
}

PyObject *object_to_python(Object *o) {
    if (!o) Py_RETURN_NONE;
    auto it = g_wrappers.find(o);
    if (it != g_wrappers.end()) {
        Py_INCREF(it->second);
        return it->second;
    }
    PyTypeObject *type = class_type(o->get_class_info(), g_module);
    if (!type) Py_RETURN_NONE;
    allocfunc alloc = (allocfunc)PyType_GetSlot(type, Py_tp_alloc);
    ObjectWrapper *w = (ObjectWrapper *)(alloc ? alloc(type, 0)
                                               : PyType_GenericAlloc(type, 0));
    if (!w) return nullptr;
    w->obj = o;
    o->ref_retain();
    w->dict = PyDict_New();
    w->weakrefs = nullptr;
    o->script_peer = w;
    g_wrappers[o] = (PyObject *)w;
    return (PyObject *)w;
}

Object *object_from_python(PyObject *o) {
    if (!o || o == Py_None) return nullptr;
    for (auto &kv : g_types)
        if (PyObject_TypeCheck(o, kv.second)) return ((ObjectWrapper *)o)->obj;
    return nullptr;
}

}  // namespace wr::python

#endif  // WARREN_PYTHON
