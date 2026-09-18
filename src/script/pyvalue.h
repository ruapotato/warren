// Manifold -- Variant to Python and back.
#pragma once

#if MANIFOLD_PYTHON

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <string>

#include "core/object.h"
#include "scene/node.h"
#include "core/variant.h"

namespace mf {
class Node;

// A Python object standing in for a node's script. Defined here so
// both halves of the binding can make one.
class PythonScript : public ScriptInstance {
public:
    PythonScript(PyObject *peer, const std::string &name);
    ~PythonScript() override;
    const char *script_name() const override { return name_.c_str(); }
    void on_ready() override;
    void on_process(float dt) override;
    void on_physics(float dt) override;
    void on_exit() override;

private:
    void call(const char *method, float dt);
    void release_peer();
    PyObject *peer_ = nullptr;   // OWNED; see the note in pyobject.cpp
    std::string name_;
    bool has_ready_ = false, has_process_ = false, has_physics_ = false,
         has_exit_ = false;
};
}  // namespace mf

namespace mf::python {

// ONE C TYPE, MANY PYTHON TYPES.
//
// Vec3, Color, Transform3D and the rest all hold a Variant and behave
// the same way, so they share one C struct and one set of slots --
// but each is a distinct Python type created from the same spec, so
// `isinstance(v, mf.Vec3)` answers correctly and a TypeError names
// the type the caller actually passed.
struct ValueObject {
    PyObject_HEAD
    Variant value;
};

// The Python type for a given Variant type, or null for the ones that
// map to Python's own (int, float, str, bool, None).
PyTypeObject *value_type(VType t);
bool is_value(PyObject *o);
Variant &value_of(PyObject *o);

// Build a Python object from a Variant, using Python's own types
// where they fit and a Value where they do not.
PyObject *to_python(const Variant &v);
// And back. Raises and returns false on a type that cannot cross.
bool from_python(PyObject *o, Variant *out);
// The same, but never raises: anything unconvertible becomes nil.
Variant from_python_or_nil(PyObject *o);

// Wrap an engine Object. The same C++ pointer always yields the same
// Python object, so `node.get_parent() is root` is true and a script
// can keep state on a node by setting an attribute on it.
PyObject *object_to_python(Object *o);
Object *object_from_python(PyObject *o);

// Called once at module init.
bool register_value_types(PyObject *module);
// Build (or fetch) the Python type mirroring a ClassDB class.
PyTypeObject *class_type(ClassInfo *ci, PyObject *module);
bool register_object_types(PyObject *module);
void refresh_object_types(PyObject *module);
// Drop the wrapper for an object that has been destroyed.
void forget_object(Object *o);

}  // namespace mf::python

#endif  // MANIFOLD_PYTHON
