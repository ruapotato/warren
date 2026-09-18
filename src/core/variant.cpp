#include "variant.h"

#include <cstdio>

#include "object.h"

namespace wr {

const char *vtype_name(VType t) {
    switch (t) {
        case VType::Nil: return "nil";
        case VType::Bool: return "bool";
        case VType::Int: return "int";
        case VType::Float: return "float";
        case VType::String: return "String";
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
        case VType::Object: return "Object";
        case VType::Array: return "Array";
        case VType::Dict: return "Dict";
        default: return "?";
    }
}

Variant::Variant(wr::Object *o) : t_(VType::Object) {
    as<wr::Object *>() = o;
    if (o) o->ref_retain();
}

void Variant::clear() {
    if (t_ == VType::Object) {
        wr::Object *o = as<wr::Object *>();
        if (o) o->ref_release();
    }
    t_ = VType::Nil;
    str_.clear();
    heap_.reset();
}

void Variant::copy_from(const Variant &o) {
    t_ = o.t_;
    __builtin_memcpy(buf_, o.buf_, sizeof(buf_));
    str_ = o.str_;
    heap_ = o.heap_;
    if (t_ == VType::Object) {
        wr::Object *p = as<wr::Object *>();
        if (p) p->ref_retain();
    }
}

void Variant::move_from(Variant &&o) noexcept {
    t_ = o.t_;
    __builtin_memcpy(buf_, o.buf_, sizeof(buf_));
    str_ = std::move(o.str_);
    heap_ = std::move(o.heap_);
    // The reference moves with it; the source must not release it.
    o.t_ = VType::Nil;
    o.str_.clear();
    o.heap_.reset();
}

// -------------------------------------------------------------- readers

bool Variant::to_bool() const {
    switch (t_) {
        case VType::Bool: return as<bool>();
        case VType::Int: return as<int64_t>() != 0;
        case VType::Float: return as<double>() != 0.0;
        case VType::String: return !str_.empty() && str_ != "false" && str_ != "0";
        case VType::Nil: return false;
        case VType::Object: return as<wr::Object *>() != nullptr;
        case VType::Array: {
            const Array *a = array_ptr();
            return a && !a->empty();
        }
        case VType::Dict: {
            const Dict *d = dict_ptr();
            return d && !d->empty();
        }
        default: return true;
    }
}

int64_t Variant::to_int() const {
    switch (t_) {
        case VType::Bool: return as<bool>() ? 1 : 0;
        case VType::Int: return as<int64_t>();
        case VType::Float: return int64_t(as<double>());
        case VType::String: {
            char *end = nullptr;
            long long v = std::strtoll(str_.c_str(), &end, 10);
            return end == str_.c_str() ? 0 : int64_t(v);
        }
        default: return 0;
    }
}

double Variant::to_float() const {
    switch (t_) {
        case VType::Bool: return as<bool>() ? 1.0 : 0.0;
        case VType::Int: return double(as<int64_t>());
        case VType::Float: return as<double>();
        case VType::String: {
            char *end = nullptr;
            double v = std::strtod(str_.c_str(), &end);
            return end == str_.c_str() ? 0.0 : v;
        }
        default: return 0.0;
    }
}

Vec2 Variant::to_vec2() const {
    switch (t_) {
        case VType::Vec2: return as<Vec2>();
        case VType::Vec3: return as<Vec3>().xy();
        case VType::Vec4: return as<Vec4>().xy();
        case VType::Int:
        case VType::Float: return Vec2(float(to_float()));
        default: return {};
    }
}

Vec3 Variant::to_vec3() const {
    switch (t_) {
        case VType::Vec3: return as<Vec3>();
        case VType::Vec2: return {as<Vec2>().x, as<Vec2>().y, 0.0f};
        case VType::Vec4: return as<Vec4>().xyz();
        case VType::Color: return as<Color>().rgb();
        case VType::Int:
        case VType::Float: return Vec3(float(to_float()));
        default: return {};
    }
}

Vec4 Variant::to_vec4() const {
    switch (t_) {
        case VType::Vec4: return as<Vec4>();
        case VType::Vec3: return {as<Vec3>(), 0.0f};
        case VType::Color: return as<Color>().rgba();
        case VType::Plane: return as<Plane>().as_vec4();
        default: return {};
    }
}

Color Variant::to_color() const {
    switch (t_) {
        case VType::Color: return as<Color>();
        case VType::Vec3: {
            Vec3 v = as<Vec3>();
            return {v.x, v.y, v.z, 1.0f};
        }
        case VType::Vec4: {
            Vec4 v = as<Vec4>();
            return {v.x, v.y, v.z, v.w};
        }
        case VType::Int: return Color::hex(uint32_t(as<int64_t>()));
        default: return Color::white();
    }
}

Quat Variant::to_quat() const {
    switch (t_) {
        case VType::Quat: return as<Quat>();
        case VType::Basis: return as<Basis>().to_quat();
        case VType::Transform: return as<Transform3D>().basis.to_quat();
        default: return {};
    }
}

Basis Variant::to_basis() const {
    switch (t_) {
        case VType::Basis: return as<Basis>();
        case VType::Quat: return as<Quat>().to_basis();
        case VType::Transform: return as<Transform3D>().basis;
        default: return {};
    }
}

Transform3D Variant::to_transform() const {
    switch (t_) {
        case VType::Transform: return as<Transform3D>();
        case VType::Basis: return {as<Basis>(), Vec3()};
        case VType::Quat: return {as<Quat>().to_basis(), Vec3()};
        case VType::Vec3: return Transform3D(as<Vec3>());
        default: return {};
    }
}

Plane Variant::to_plane() const {
    switch (t_) {
        case VType::Plane: return as<Plane>();
        case VType::Vec4: {
            Vec4 v = as<Vec4>();
            return {Vec3(v.x, v.y, v.z), -v.w};
        }
        default: return {};
    }
}

AABB Variant::to_aabb() const { return t_ == VType::AABB ? as<AABB>() : AABB(); }
Rect2 Variant::to_rect2() const { return t_ == VType::Rect2 ? as<Rect2>() : Rect2(); }
Projection Variant::to_projection() const {
    switch (t_) {
        case VType::Projection: return as<Projection>();
        case VType::Transform: return wr::to_projection(as<Transform3D>());
        default: return Projection::identity();
    }
}

wr::Object *Variant::to_object() const {
    return t_ == VType::Object ? as<wr::Object *>() : nullptr;
}

Array &Variant::array() {
    if (t_ != VType::Array || !heap_) {
        clear();
        t_ = VType::Array;
        heap_ = std::make_shared<Array>();
    }
    return *static_cast<Array *>(heap_.get());
}

const Array *Variant::array_ptr() const {
    return (t_ == VType::Array && heap_) ? static_cast<const Array *>(heap_.get())
                                         : nullptr;
}

Dict &Variant::dict() {
    if (t_ != VType::Dict || !heap_) {
        clear();
        t_ = VType::Dict;
        heap_ = std::make_shared<Dict>();
    }
    return *static_cast<Dict *>(heap_.get());
}

const Dict *Variant::dict_ptr() const {
    return (t_ == VType::Dict && heap_) ? static_cast<const Dict *>(heap_.get())
                                        : nullptr;
}

// ------------------------------------------------------------- printing

static std::string f2s(float v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%g", double(v));
    return b;
}

std::string Variant::to_string() const {
    char b[256];
    switch (t_) {
        case VType::Nil: return "null";
        case VType::Bool: return as<bool>() ? "true" : "false";
        case VType::Int:
            std::snprintf(b, sizeof(b), "%lld", (long long)as<int64_t>());
            return b;
        case VType::Float:
            std::snprintf(b, sizeof(b), "%g", as<double>());
            return b;
        case VType::String: return str_;
        case VType::Vec2: {
            Vec2 v = as<Vec2>();
            return "(" + f2s(v.x) + ", " + f2s(v.y) + ")";
        }
        case VType::Vec3: {
            Vec3 v = as<Vec3>();
            return "(" + f2s(v.x) + ", " + f2s(v.y) + ", " + f2s(v.z) + ")";
        }
        case VType::Vec4: {
            Vec4 v = as<Vec4>();
            return "(" + f2s(v.x) + ", " + f2s(v.y) + ", " + f2s(v.z) + ", " +
                   f2s(v.w) + ")";
        }
        case VType::Color: {
            Color c = as<Color>();
            return "Color(" + f2s(c.r) + ", " + f2s(c.g) + ", " + f2s(c.b) + ", " +
                   f2s(c.a) + ")";
        }
        case VType::Quat: {
            Quat q = as<Quat>();
            return "Quat(" + f2s(q.x) + ", " + f2s(q.y) + ", " + f2s(q.z) + ", " +
                   f2s(q.w) + ")";
        }
        case VType::Basis: {
            Basis m = as<Basis>();
            return "Basis[" + Variant(m.col[0]).to_string() + ", " +
                   Variant(m.col[1]).to_string() + ", " +
                   Variant(m.col[2]).to_string() + "]";
        }
        case VType::Transform: {
            Transform3D t = as<Transform3D>();
            return "Transform3D(" + Variant(t.basis).to_string() + ", " +
                   Variant(t.origin).to_string() + ")";
        }
        case VType::Plane: {
            Plane p = as<Plane>();
            return "Plane(" + Variant(p.normal).to_string() + ", " + f2s(p.d) + ")";
        }
        case VType::AABB: {
            AABB a = as<AABB>();
            return "AABB(" + Variant(a.min).to_string() + " .. " +
                   Variant(a.max).to_string() + ")";
        }
        case VType::Rect2: {
            Rect2 r = as<Rect2>();
            return "Rect2(" + Variant(r.position).to_string() + ", " +
                   Variant(r.size).to_string() + ")";
        }
        case VType::Projection: {
            const Projection &p = as<Projection>();
            std::string s = "Projection[";
            for (int r = 0; r < 4; r++) {
                s += "[";
                for (int c = 0; c < 4; c++) {
                    s += f2s(p.m[c][r]);
                    if (c < 3) s += ", ";
                }
                s += r < 3 ? "], " : "]";
            }
            return s + "]";
        }
        case VType::Object: {
            wr::Object *o = as<wr::Object *>();
            return o ? o->to_string() : "<null Object>";
        }
        case VType::Array: {
            const Array *a = array_ptr();
            if (!a) return "[]";
            std::string s = "[";
            for (size_t i = 0; i < a->size(); i++) {
                if (i) s += ", ";
                s += (*a)[i].to_string();
            }
            return s + "]";
        }
        case VType::Dict: {
            const Dict *d = dict_ptr();
            if (!d) return "{}";
            std::string s = "{";
            bool first = true;
            for (const auto &kv : *d) {
                if (!first) s += ", ";
                first = false;
                s += kv.first + ": " + kv.second.to_string();
            }
            return s + "}";
        }
        default: return "?";
    }
}

bool Variant::operator==(const Variant &o) const {
    if (t_ != o.t_) {
        // A 2 and a 2.0 are the same number; nothing else converts.
        if (is_num() && o.is_num()) return to_float() == o.to_float();
        return false;
    }
    switch (t_) {
        case VType::Nil: return true;
        case VType::Bool: return as<bool>() == o.as<bool>();
        case VType::Int: return as<int64_t>() == o.as<int64_t>();
        case VType::Float: return as<double>() == o.as<double>();
        case VType::String: return str_ == o.str_;
        case VType::Vec2: return as<Vec2>() == o.as<Vec2>();
        case VType::Vec3: return as<Vec3>() == o.as<Vec3>();
        case VType::Vec4: return as<Vec4>() == o.as<Vec4>();
        case VType::Object: return as<wr::Object *>() == o.as<wr::Object *>();
        case VType::Projection: return as<Projection>() == o.as<Projection>();
        case VType::Array: {
            const Array *a = array_ptr();
            const Array *b = o.array_ptr();
            if (a == b) return true;
            if (!a || !b || a->size() != b->size()) return false;
            for (size_t i = 0; i < a->size(); i++)
                if (!((*a)[i] == (*b)[i])) return false;
            return true;
        }
        case VType::Dict: return dict_ptr() == o.dict_ptr();
        default:
            return __builtin_memcmp(buf_, o.buf_, sizeof(buf_)) == 0;
    }
}

}  // namespace wr

// BLENDING TWO VALUES OF WHATEVER TYPE THEY TURN OUT TO BE.
//
// Interpolation belongs with the type rather than with whatever is
// doing the interpolating. A replicator that switches on VType to
// smooth a position is one that stops working the day a class
// replicates a rotation, and a tween, an animation track and an undo
// preview all want the same answer.
//
// Rotations take the shortest arc. Anything with no sensible midpoint
// -- a string, an object, an array -- holds the old value until the
// end and then takes the new one, which is at least never a value
// that was true at neither end.
namespace wr {

Variant Variant::lerp(const Variant &a, const Variant &b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    if (a.type() != b.type()) return t < 0.5f ? a : b;
    switch (a.type()) {
        case VType::Float:
            return Variant(double(a.to_float() + (b.to_float() - a.to_float()) * t));
        case VType::Int: {
            const double v = double(a.to_int()) +
                             (double(b.to_int()) - double(a.to_int())) * double(t);
            return Variant(int64_t(v + (v < 0 ? -0.5 : 0.5)));
        }
        case VType::Vec2: return Variant(a.to_vec2() + (b.to_vec2() - a.to_vec2()) * t);
        case VType::Vec3: return Variant(a.to_vec3() + (b.to_vec3() - a.to_vec3()) * t);
        case VType::Vec4: return Variant(a.to_vec4() + (b.to_vec4() - a.to_vec4()) * t);
        case VType::Color: {
            const Color x = a.to_color(), y = b.to_color();
            return Variant(Color(x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t,
                                 x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t));
        }
        case VType::Quat: return Variant(slerp(a.to_quat(), b.to_quat(), t));
        case VType::Transform: {
            const Transform3D x = a.to_transform(), y = b.to_transform();
            Transform3D out;
            out.origin = x.origin + (y.origin - x.origin) * t;
            out.basis = Basis(slerp(x.basis.to_quat(), y.basis.to_quat(), t));
            return Variant(out);
        }
        case VType::Plane: {
            const Plane x = a.to_plane(), y = b.to_plane();
            return Variant(Plane((x.normal + (y.normal - x.normal) * t).normalized(),
                                 x.d + (y.d - x.d) * t));
        }
        default:
            return t < 0.5f ? a : b;
    }
}

}  // namespace wr
