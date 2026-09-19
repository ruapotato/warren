#include "pyvalue.h"

#if WARREN_PYTHON

#include <cstring>
#include <map>
#include <string>
#include <unordered_map>

#include "core/log.h"

namespace wr::python {
namespace {

PyTypeObject *g_value_types[int(VType::Count)] = {};
// Engine object -> its Python wrapper. Weak: the wrapper holds a
// reference to the object, not the other way round, and the entry is
// removed when either dies.
std::unordered_map<Object *, PyObject *> g_wrappers;
std::unordered_map<ClassInfo *, PyTypeObject *> g_class_types;

// ------------------------------------------------------------ the value

const char *component_names(VType t) {
    switch (t) {
        case VType::Vec2: return "xy";
        case VType::Vec3: return "xyz";
        case VType::Vec4: return "xyzw";
        case VType::Color: return "rgba";
        case VType::Quat: return "xyzw";
        default: return "";
    }
}

int component_index(VType t, const char *name) {
    if (!name || name[1] != 0) return -1;
    const char *set = component_names(t);
    for (int i = 0; set[i]; i++)
        if (set[i] == name[0]) return i;
    return -1;
}

float get_component(const Variant &v, int i) {
    switch (v.type()) {
        case VType::Vec2: return v.to_vec2()[i];
        case VType::Vec3: return v.to_vec3()[i];
        case VType::Vec4: return v.to_vec4()[i];
        case VType::Color: return v.to_color()[i];
        case VType::Quat: {
            Quat q = v.to_quat();
            return (&q.x)[i];
        }
        default: return 0.0f;
    }
}

void set_component(Variant &v, int i, float f) {
    switch (v.type()) {
        case VType::Vec2: {
            Vec2 a = v.to_vec2();
            a[i] = f;
            v = a;
            break;
        }
        case VType::Vec3: {
            Vec3 a = v.to_vec3();
            a[i] = f;
            v = a;
            break;
        }
        case VType::Vec4: {
            Vec4 a = v.to_vec4();
            a[i] = f;
            v = a;
            break;
        }
        case VType::Color: {
            Color a = v.to_color();
            a[i] = f;
            v = a;
            break;
        }
        case VType::Quat: {
            Quat a = v.to_quat();
            (&a.x)[i] = f;
            v = a;
            break;
        }
        default: break;
    }
}

PyObject *make_value(const Variant &v) {
    PyTypeObject *type = value_type(v.type());
    if (!type) Py_RETURN_NONE;
    ValueObject *self = PyObject_New(ValueObject, type);
    if (!self) return nullptr;
    // PyObject_New does not run the C++ constructor.
    new (&self->value) Variant(v);
    return (PyObject *)self;
}

void value_dealloc(PyObject *self) {
    ValueObject *v = (ValueObject *)self;
    v->value.~Variant();
    PyTypeObject *t = Py_TYPE(self);
    PyObject_Free(self);
    Py_DECREF(t);
}

PyObject *value_repr(PyObject *self) {
    ValueObject *v = (ValueObject *)self;
    return PyUnicode_FromFormat("%s%s", Py_TYPE(self)->tp_name + 9,
                                v->value.to_string().c_str());
}

PyObject *value_str(PyObject *self) {
    ValueObject *v = (ValueObject *)self;
    return PyUnicode_FromString(v->value.to_string().c_str());
}

PyObject *value_getattro(PyObject *self, PyObject *name) {
    ValueObject *v = (ValueObject *)self;
    const char *n = PyUnicode_AsUTF8(name);
    if (!n) return nullptr;
    const int c = component_index(v->value.type(), n);
    if (c >= 0) return PyFloat_FromDouble(double(get_component(v->value, c)));

    // The compound types' parts.
    if (v->value.type() == VType::Transform) {
        Transform3D t = v->value.to_transform();
        if (!std::strcmp(n, "origin")) return to_python(Variant(t.origin));
        if (!std::strcmp(n, "basis")) return to_python(Variant(t.basis));
    } else if (v->value.type() == VType::Plane) {
        Plane p = v->value.to_plane();
        if (!std::strcmp(n, "normal")) return to_python(Variant(p.normal));
        if (!std::strcmp(n, "d")) return PyFloat_FromDouble(double(p.d));
    } else if (v->value.type() == VType::AABB) {
        AABB b = v->value.to_aabb();
        if (!std::strcmp(n, "min")) return to_python(Variant(b.min));
        if (!std::strcmp(n, "max")) return to_python(Variant(b.max));
        if (!std::strcmp(n, "size")) return to_python(Variant(b.size()));
        if (!std::strcmp(n, "center")) return to_python(Variant(b.center()));
    } else if (v->value.type() == VType::Rect2) {
        Rect2 r = v->value.to_rect2();
        if (!std::strcmp(n, "position")) return to_python(Variant(r.position));
        if (!std::strcmp(n, "size")) return to_python(Variant(r.size));
    }
    return PyObject_GenericGetAttr(self, name);
}

int value_setattro(PyObject *self, PyObject *name, PyObject *val) {
    ValueObject *v = (ValueObject *)self;
    const char *n = PyUnicode_AsUTF8(name);
    if (!n) return -1;
    const int c = component_index(v->value.type(), n);
    if (c >= 0) {
        if (!val) {
            PyErr_SetString(PyExc_AttributeError, "cannot delete a component");
            return -1;
        }
        double d = PyFloat_AsDouble(val);
        if (PyErr_Occurred()) return -1;
        set_component(v->value, c, float(d));
        return 0;
    }
    if (v->value.type() == VType::Transform && val) {
        Transform3D t = v->value.to_transform();
        Variant other;
        if (!std::strcmp(n, "origin") && from_python(val, &other)) {
            t.origin = other.to_vec3();
            v->value = t;
            return 0;
        }
        if (!std::strcmp(n, "basis") && from_python(val, &other)) {
            t.basis = other.to_basis();
            v->value = t;
            return 0;
        }
    }
    return PyObject_GenericSetAttr(self, name, val);
}

// --- arithmetic. Only for the types where it means something.

bool numeric(VType t) {
    return t == VType::Vec2 || t == VType::Vec3 || t == VType::Vec4 ||
           t == VType::Color;
}

bool as_vec4(PyObject *o, Vec4 *out, VType *type) {
    if (is_value(o)) {
        const Variant &v = value_of(o);
        if (!numeric(v.type())) return false;
        *type = v.type();
        switch (v.type()) {
            case VType::Vec2: *out = Vec4(v.to_vec2().x, v.to_vec2().y, 0, 0); break;
            case VType::Vec3: *out = Vec4(v.to_vec3(), 0); break;
            case VType::Vec4: *out = v.to_vec4(); break;
            default: *out = v.to_color().rgba(); break;
        }
        return true;
    }
    if (PyFloat_Check(o) || PyLong_Check(o)) {
        double d = PyFloat_AsDouble(o);
        if (PyErr_Occurred()) return false;
        *out = Vec4(float(d), float(d), float(d), float(d));
        *type = VType::Nil;  // a scalar takes the other operand's type
        return true;
    }
    return false;
}

PyObject *pack(VType t, const Vec4 &v) {
    switch (t) {
        case VType::Vec2: return to_python(Variant(Vec2(v.x, v.y)));
        case VType::Vec3: return to_python(Variant(v.xyz()));
        case VType::Vec4: return to_python(Variant(v));
        case VType::Color: return to_python(Variant(Color(v.x, v.y, v.z, v.w)));
        default: Py_RETURN_NOTIMPLEMENTED;
    }
}

template <int Op>
PyObject *binary(PyObject *a, PyObject *b) {
    Vec4 va, vb;
    VType ta = VType::Nil, tb = VType::Nil;
    if (!as_vec4(a, &va, &ta) || !as_vec4(b, &vb, &tb)) {
        PyErr_Clear();
        Py_RETURN_NOTIMPLEMENTED;
    }
    VType t = ta != VType::Nil ? ta : tb;
    if (t == VType::Nil) Py_RETURN_NOTIMPLEMENTED;
    // Two different vector types are a mistake, not a conversion.
    if (ta != VType::Nil && tb != VType::Nil && ta != tb) {
        PyErr_Format(PyExc_TypeError, "cannot combine %s with %s", vtype_name(ta),
                     vtype_name(tb));
        return nullptr;
    }
    Vec4 r;
    switch (Op) {
        case 0: r = va + vb; break;
        case 1: r = va - vb; break;
        case 2: r = Vec4(va.x * vb.x, va.y * vb.y, va.z * vb.z, va.w * vb.w); break;
        case 3:
            if (std::fabs(vb.x) < 1e-20f || std::fabs(vb.y) < 1e-20f) {
                PyErr_SetString(PyExc_ZeroDivisionError, "vector division by zero");
                return nullptr;
            }
            r = Vec4(va.x / vb.x, va.y / vb.y, va.z / vb.z,
                     std::fabs(vb.w) > 1e-20f ? va.w / vb.w : 0.0f);
            break;
        default: Py_RETURN_NOTIMPLEMENTED;
    }
    return pack(t, r);
}

PyObject *value_negative(PyObject *a) {
    Vec4 v;
    VType t = VType::Nil;
    if (!as_vec4(a, &v, &t) || t == VType::Nil) Py_RETURN_NOTIMPLEMENTED;
    return pack(t, Vec4(-v.x, -v.y, -v.z, -v.w));
}

PyObject *value_richcompare(PyObject *a, PyObject *b, int op) {
    if (op != Py_EQ && op != Py_NE) Py_RETURN_NOTIMPLEMENTED;
    if (!is_value(a) || !is_value(b)) {
        if (op == Py_EQ) Py_RETURN_FALSE;
        Py_RETURN_TRUE;
    }
    const bool equal = value_of(a) == value_of(b);
    if ((op == Py_EQ) == equal) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

// --- methods shared by the vector types

PyObject *value_length(PyObject *self, PyObject *) {
    Vec4 v;
    VType t;
    if (!as_vec4(self, &v, &t)) Py_RETURN_NONE;
    switch (t) {
        case VType::Vec2: return PyFloat_FromDouble(Vec2(v.x, v.y).length());
        case VType::Vec4: return PyFloat_FromDouble(std::sqrt(dot(v, v)));
        default: return PyFloat_FromDouble(v.xyz().length());
    }
}

PyObject *value_normalized(PyObject *self, PyObject *) {
    Vec4 v;
    VType t;
    if (!as_vec4(self, &v, &t)) Py_RETURN_NONE;
    if (t == VType::Vec2) {
        Vec2 r = Vec2(v.x, v.y).normalized();
        return to_python(Variant(r));
    }
    return pack(t, Vec4(v.xyz().normalized(), v.w));
}

PyObject *value_dot(PyObject *self, PyObject *other) {
    Vec4 a, b;
    VType ta, tb;
    if (!as_vec4(self, &a, &ta) || !as_vec4(other, &b, &tb)) Py_RETURN_NONE;
    return PyFloat_FromDouble(dot(a.xyz(), b.xyz()));
}

PyObject *value_cross(PyObject *self, PyObject *other) {
    Vec4 a, b;
    VType ta, tb;
    if (!as_vec4(self, &a, &ta) || !as_vec4(other, &b, &tb)) Py_RETURN_NONE;
    return to_python(Variant(cross(a.xyz(), b.xyz())));
}

PyObject *value_as_tuple(PyObject *self, PyObject *) {
    ValueObject *v = (ValueObject *)self;
    const char *set = component_names(v->value.type());
    const Py_ssize_t n = Py_ssize_t(std::strlen(set));
    if (!n) Py_RETURN_NONE;
    PyObject *t = PyTuple_New(n);
    for (Py_ssize_t i = 0; i < n; i++)
        PyTuple_SET_ITEM(t, i,
                         PyFloat_FromDouble(double(get_component(v->value, int(i)))));
    return t;
}

// APPLYING A TRANSFORM OR A BASIS TO A POINT, from a script.
//
// A script could read a Transform3D's origin and basis and do
// nothing with either -- there was no way to put a point through
// one. That matters the moment a game carries anything alongside a
// body that also has to go through a portal: a third-person
// camera, a smoothed boom, an aim direction. Without this the
// camera stays on the far side of the level and slides across to
// catch up, which is the opposite of what a portal is for.
//
// `xform` moves a point (rotation, scale and translation);
// `xform_dir` turns a direction (no translation), which is what a
// look vector wants.
PyObject *value_xform(PyObject *self, PyObject *other) {
    ValueObject *v = (ValueObject *)self;
    Variant p;
    if (!from_python(other, &p)) Py_RETURN_NONE;
    const Vec3 point = p.to_vec3();
    if (v->value.type() == VType::Transform)
        return to_python(Variant(v->value.to_transform().xform(point)));
    if (v->value.type() == VType::Basis)
        return to_python(Variant(v->value.to_basis().xform(point)));
    Py_RETURN_NONE;
}

PyObject *value_xform_dir(PyObject *self, PyObject *other) {
    ValueObject *v = (ValueObject *)self;
    Variant p;
    if (!from_python(other, &p)) Py_RETURN_NONE;
    const Vec3 dir = p.to_vec3();
    if (v->value.type() == VType::Transform)
        return to_python(Variant(v->value.to_transform().basis.xform(dir)));
    if (v->value.type() == VType::Basis)
        return to_python(Variant(v->value.to_basis().xform(dir)));
    Py_RETURN_NONE;
}

// ------------------------------------------- the rotation constructors
//
// A ROTATION HAS TO BE CONSTRUCTIBLE FROM A SCRIPT. Every value
// type is built from its components -- Vec3(x, y, z) -- and for
// a quaternion that is useless: nobody writes down the four
// numbers. The two ways anybody actually names a rotation are
// three angles and an axis with an angle, and without them a
// script can read an orientation and can never make one, which
// is the difference between being able to ask a body which way
// it is pointing and being able to tell it.

PyObject *quat_from_euler(PyObject *, PyObject *args) {
    double yaw = 0, pitch = 0, roll = 0;
    if (!PyArg_ParseTuple(args, "d|dd", &yaw, &pitch, &roll)) return nullptr;
    return to_python(Variant(Quat::from_euler_yxz(float(yaw), float(pitch),
                                                  float(roll))));
}

PyObject *quat_from_axis_angle(PyObject *, PyObject *args) {
    PyObject *axis = nullptr;
    double angle = 0;
    if (!PyArg_ParseTuple(args, "Od", &axis, &angle)) return nullptr;
    Variant a;
    if (!from_python(axis, &a)) return nullptr;
    return to_python(Variant(Quat::from_axis_angle(a.to_vec3(), float(angle))));
}

PyObject *quat_between(PyObject *, PyObject *args) {
    PyObject *from = nullptr, *to = nullptr;
    if (!PyArg_ParseTuple(args, "OO", &from, &to)) return nullptr;
    Variant a, b;
    if (!from_python(from, &a) || !from_python(to, &b)) return nullptr;
    return to_python(Variant(Quat::between(a.to_vec3(), b.to_vec3())));
}

PyObject *basis_from_axis_angle(PyObject *, PyObject *args) {
    PyObject *axis = nullptr;
    double angle = 0;
    if (!PyArg_ParseTuple(args, "Od", &axis, &angle)) return nullptr;
    Variant a;
    if (!from_python(axis, &a)) return nullptr;
    return to_python(Variant(Basis::from_axis_angle(a.to_vec3(), float(angle))));
}

PyMethodDef k_quat_statics[] = {
    {"from_euler_yxz", quat_from_euler, METH_VARARGS,
     "A rotation from yaw, pitch and roll, applied in that order."},
    {"from_axis_angle", quat_from_axis_angle, METH_VARARGS,
     "A rotation of `angle` radians about `axis`."},
    {"between", quat_between, METH_VARARGS,
     "The shortest rotation taking one direction to another."},
    {nullptr, nullptr, 0, nullptr}};

PyMethodDef k_basis_statics[] = {
    {"from_axis_angle", basis_from_axis_angle, METH_VARARGS,
     "A rotation of `angle` radians about `axis`."},
    {nullptr, nullptr, 0, nullptr}};

PyMethodDef k_value_methods[] = {
    {"xform", value_xform, METH_O,
     "A point through this transform or basis."},
    {"xform_dir", value_xform_dir, METH_O,
     "A direction through it -- rotation and scale, no translation."},
    {"length", value_length, METH_NOARGS, "Its magnitude."},
    {"normalized", value_normalized, METH_NOARGS, "A unit vector in the same direction."},
    {"dot", value_dot, METH_O, "The dot product with another."},
    {"cross", value_cross, METH_O, "The cross product with another."},
    {"as_tuple", value_as_tuple, METH_NOARGS, "Its components as a tuple."},
    {nullptr, nullptr, 0, nullptr}};

// The constructor: mf.Vec3(1, 2, 3), mf.Vec3(), mf.Vec3(other).
PyObject *value_new(PyTypeObject *type, PyObject *args, PyObject *) {
    VType vt = VType::Nil;
    for (int i = 0; i < int(VType::Count); i++)
        if (g_value_types[i] == type) vt = VType(i);
    if (vt == VType::Nil) {
        PyErr_SetString(PyExc_TypeError, "unknown Warren value type");
        return nullptr;
    }
    const Py_ssize_t n = args ? PyTuple_GET_SIZE(args) : 0;
    Variant v;
    auto arg = [&](Py_ssize_t i) {
        return i < n ? PyFloat_AsDouble(PyTuple_GET_ITEM(args, i)) : 0.0;
    };
    switch (vt) {
        case VType::Vec2:
            v = Vec2(float(arg(0)), float(arg(1)));
            break;
        case VType::Vec3:
            v = Vec3(float(arg(0)), float(arg(1)), float(arg(2)));
            break;
        case VType::Vec4:
            v = Vec4(float(arg(0)), float(arg(1)), float(arg(2)), float(arg(3)));
            break;
        case VType::Color:
            v = Color(float(arg(0)), float(arg(1)), float(arg(2)),
                      n > 3 ? float(arg(3)) : 1.0f);
            break;
        case VType::Quat:
            v = n ? Quat(float(arg(0)), float(arg(1)), float(arg(2)), float(arg(3)))
                  : Quat();
            break;
        case VType::Transform: {
            Transform3D t;
            if (n >= 1) {
                Variant a;
                if (from_python(PyTuple_GET_ITEM(args, 0), &a)) {
                    if (a.type() == VType::Basis) t.basis = a.to_basis();
                    else t.origin = a.to_vec3();
                }
            }
            if (n >= 2) {
                Variant b;
                if (from_python(PyTuple_GET_ITEM(args, 1), &b)) t.origin = b.to_vec3();
            }
            v = t;
            break;
        }
        case VType::Plane: {
            Variant nrm;
            if (n >= 1 && from_python(PyTuple_GET_ITEM(args, 0), &nrm))
                v = Plane(nrm.to_vec3(), float(arg(1)));
            else
                v = Plane();
            break;
        }
        case VType::AABB: {
            Variant lo, hi;
            if (n >= 2 && from_python(PyTuple_GET_ITEM(args, 0), &lo) &&
                from_python(PyTuple_GET_ITEM(args, 1), &hi))
                v = AABB(lo.to_vec3(), hi.to_vec3());
            else
                v = AABB();
            break;
        }
        case VType::Rect2:
            v = Rect2(float(arg(0)), float(arg(1)), float(arg(2)), float(arg(3)));
            break;
        case VType::Basis:
            v = Basis();
            break;
        case VType::Projection:
            v = Projection::identity();
            break;
        default:
            PyErr_SetString(PyExc_TypeError, "that type cannot be constructed");
            return nullptr;
    }
    if (PyErr_Occurred()) return nullptr;
    return make_value(v);
}

PyType_Slot k_value_slots[] = {
    {Py_tp_dealloc, (void *)value_dealloc},
    {Py_tp_repr, (void *)value_repr},
    {Py_tp_str, (void *)value_str},
    {Py_tp_getattro, (void *)value_getattro},
    {Py_tp_setattro, (void *)value_setattro},
    {Py_tp_methods, (void *)k_value_methods},
    {Py_tp_new, (void *)value_new},
    {Py_tp_richcompare, (void *)value_richcompare},
    {Py_nb_add, (void *)binary<0>},
    {Py_nb_subtract, (void *)binary<1>},
    {Py_nb_multiply, (void *)binary<2>},
    {Py_nb_true_divide, (void *)binary<3>},
    {Py_nb_negative, (void *)value_negative},
    {0, nullptr}};

struct ValueTypeDesc {
    VType type;
    const char *name;
    // Constructors that live on the type rather than on an
    // instance. Null for the types that are just components.
    PyMethodDef *statics;
};

const ValueTypeDesc k_value_type_list[] = {
    {VType::Vec2, "Vec2", nullptr},
    {VType::Vec3, "Vec3", nullptr},
    {VType::Vec4, "Vec4", nullptr},
    {VType::Color, "Color", nullptr},
    {VType::Quat, "Quat", k_quat_statics},
    {VType::Basis, "Basis", k_basis_statics},
    {VType::Transform, "Transform3D", nullptr},
    {VType::Plane, "Plane", nullptr},
    {VType::AABB, "AABB", nullptr},
    {VType::Rect2, "Rect2", nullptr},
    {VType::Projection, "Projection", nullptr},
};

}  // namespace

PyTypeObject *value_type(VType t) {
    return int(t) < int(VType::Count) ? g_value_types[int(t)] : nullptr;
}

bool is_value(PyObject *o) {
    if (!o) return false;
    for (PyTypeObject *t : g_value_types)
        if (t && Py_IS_TYPE(o, t)) return true;
    return false;
}

Variant &value_of(PyObject *o) { return ((ValueObject *)o)->value; }

bool register_value_types(PyObject *module) {
    for (const ValueTypeDesc &d : k_value_type_list) {
        std::string full = std::string("warren.") + d.name;
        // Leaked on purpose: a PyType_Spec's name must outlive the
        // type, and the type outlives the process.
        char *name = strdup(full.c_str());
        PyType_Spec spec{name, sizeof(ValueObject), 0,
                         Py_TPFLAGS_DEFAULT, k_value_slots};
        PyObject *type = PyType_FromSpec(&spec);
        if (!type) return false;
        g_value_types[int(d.type)] = (PyTypeObject *)type;
        for (PyMethodDef *m = d.statics; m && m->ml_name; m++) {
            PyObject *fn = PyCFunction_New(m, nullptr);
            if (!fn) return false;
            PyObject *st = PyStaticMethod_New(fn);
            Py_DECREF(fn);
            if (!st) return false;
            int rc = PyObject_SetAttrString(type, m->ml_name, st);
            Py_DECREF(st);
            if (rc < 0) return false;
        }
        Py_INCREF(type);
        if (PyModule_AddObject(module, d.name, type) < 0) return false;
    }
    return true;
}

// ---------------------------------------------------------- conversion

PyObject *to_python(const Variant &v) {
    switch (v.type()) {
        case VType::Nil: Py_RETURN_NONE;
        case VType::Bool: return PyBool_FromLong(v.to_bool() ? 1 : 0);
        case VType::Int: return PyLong_FromLongLong((long long)v.to_int());
        case VType::Float: return PyFloat_FromDouble(v.to_float());
        case VType::String: return PyUnicode_FromString(v.to_string().c_str());
        case VType::Object: return object_to_python(v.to_object());
        case VType::Array: {
            const Array *a = v.array_ptr();
            PyObject *list = PyList_New(a ? Py_ssize_t(a->size()) : 0);
            if (!list || !a) return list;
            for (size_t i = 0; i < a->size(); i++)
                PyList_SET_ITEM(list, Py_ssize_t(i), to_python((*a)[i]));
            return list;
        }
        case VType::Dict: {
            const Dict *d = v.dict_ptr();
            PyObject *dict = PyDict_New();
            if (!dict || !d) return dict;
            for (const auto &kv : *d) {
                PyObject *val = to_python(kv.second);
                PyDict_SetItemString(dict, kv.first.c_str(), val);
                Py_XDECREF(val);
            }
            return dict;
        }
        default: return make_value(v);
    }
}

bool from_python(PyObject *o, Variant *out) {
    if (!o || o == Py_None) {
        *out = Variant();
        return true;
    }
    if (PyBool_Check(o)) {
        *out = Variant(o == Py_True);
        return true;
    }
    if (PyLong_Check(o)) {
        *out = Variant(int64_t(PyLong_AsLongLong(o)));
        return !PyErr_Occurred();
    }
    if (PyFloat_Check(o)) {
        *out = Variant(PyFloat_AsDouble(o));
        return !PyErr_Occurred();
    }
    if (PyUnicode_Check(o)) {
        const char *s = PyUnicode_AsUTF8(o);
        *out = Variant(s ? s : "");
        return s != nullptr;
    }
    if (is_value(o)) {
        *out = value_of(o);
        return true;
    }
    if (Object *obj = object_from_python(o)) {
        *out = Variant(obj);
        return true;
    }
    if (PyList_Check(o) || PyTuple_Check(o)) {
        PyObject *seq = PySequence_Fast(o, "expected a sequence");
        if (!seq) return false;
        Array a;
        const Py_ssize_t n = PySequence_Fast_GET_SIZE(seq);
        a.reserve(size_t(n));
        for (Py_ssize_t i = 0; i < n; i++) {
            Variant item;
            if (!from_python(PySequence_Fast_GET_ITEM(seq, i), &item)) {
                Py_DECREF(seq);
                return false;
            }
            a.push_back(item);
        }
        Py_DECREF(seq);
        *out = Variant(std::move(a));
        return true;
    }
    if (PyDict_Check(o)) {
        Dict d;
        PyObject *key = nullptr, *val = nullptr;
        Py_ssize_t pos = 0;
        while (PyDict_Next(o, &pos, &key, &val)) {
            const char *k = PyUnicode_AsUTF8(key);
            if (!k) return false;
            Variant item;
            if (!from_python(val, &item)) return false;
            d[k] = item;
        }
        *out = Variant(std::move(d));
        return true;
    }
    PyErr_Format(PyExc_TypeError, "cannot pass a %s to the engine",
                 Py_TYPE(o)->tp_name);
    return false;
}

Variant from_python_or_nil(PyObject *o) {
    Variant v;
    if (!from_python(o, &v)) {
        PyErr_Clear();
        return Variant();
    }
    return v;
}

}  // namespace wr::python

#endif  // WARREN_PYTHON
