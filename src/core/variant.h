// Warren -- one value, any type.
//
// The bridge between C++ and Python, and the currency of the reflection
// system. Deliberately not clever: a tag, a 48-byte buffer big enough
// for the largest plain math type, a string and a shared pointer for the
// two containers. A Variant is never in a hot loop -- the renderer and
// the physics speak in real types -- so the cost of it being fat is not
// a cost anyone pays.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "math/projection.h"

namespace wr {

class Object;
class Variant;

using Array = std::vector<Variant>;
using Dict = std::map<std::string, Variant>;

enum class VType : uint8_t {
    Nil = 0,
    Bool,
    Int,
    Float,
    String,
    Vec2,
    Vec3,
    Vec4,
    Color,
    Quat,
    Basis,
    Transform,
    Plane,
    AABB,
    Rect2,
    Projection,
    Object,
    Array,
    Dict,
    Count
};

const char *vtype_name(VType t);

class Variant {
public:
    Variant() = default;
    ~Variant() { clear(); }
    Variant(const Variant &o) { copy_from(o); }
    Variant(Variant &&o) noexcept { move_from(std::move(o)); }
    Variant &operator=(const Variant &o) {
        if (this != &o) { clear(); copy_from(o); }
        return *this;
    }
    Variant &operator=(Variant &&o) noexcept {
        if (this != &o) { clear(); move_from(std::move(o)); }
        return *this;
    }

    // --- making one -----------------------------------------------------
    Variant(bool v) : t_(VType::Bool) { as<bool>() = v; }
    Variant(int v) : t_(VType::Int) { as<int64_t>() = v; }
    Variant(int64_t v) : t_(VType::Int) { as<int64_t>() = v; }
    Variant(uint32_t v) : t_(VType::Int) { as<int64_t>() = int64_t(v); }
    Variant(float v) : t_(VType::Float) { as<double>() = v; }
    Variant(double v) : t_(VType::Float) { as<double>() = v; }
    Variant(const char *v) : t_(VType::String), str_(v ? v : "") {}
    Variant(std::string v) : t_(VType::String), str_(std::move(v)) {}
    Variant(const wr::Vec2 &v) : t_(VType::Vec2) { as<wr::Vec2>() = v; }
    Variant(const wr::Vec3 &v) : t_(VType::Vec3) { as<wr::Vec3>() = v; }
    Variant(const wr::Vec4 &v) : t_(VType::Vec4) { as<wr::Vec4>() = v; }
    Variant(const wr::Color &v) : t_(VType::Color) { as<wr::Color>() = v; }
    Variant(const wr::Quat &v) : t_(VType::Quat) { as<wr::Quat>() = v; }
    Variant(const wr::Basis &v) : t_(VType::Basis) { as<wr::Basis>() = v; }
    Variant(const wr::Transform3D &v) : t_(VType::Transform) { as<wr::Transform3D>() = v; }
    Variant(const wr::Plane &v) : t_(VType::Plane) { as<wr::Plane>() = v; }
    Variant(const wr::AABB &v) : t_(VType::AABB) { as<wr::AABB>() = v; }
    Variant(const wr::Rect2 &v) : t_(VType::Rect2) { as<wr::Rect2>() = v; }
    Variant(const wr::Projection &v) : t_(VType::Projection) { as<wr::Projection>() = v; }
    Variant(wr::Object *o);
    Variant(const Array &a) : t_(VType::Array), heap_(std::make_shared<Array>(a)) {}
    Variant(Array &&a) : t_(VType::Array), heap_(std::make_shared<Array>(std::move(a))) {}
    Variant(const Dict &d) : t_(VType::Dict), heap_(std::make_shared<Dict>(d)) {}
    Variant(Dict &&d) : t_(VType::Dict), heap_(std::make_shared<Dict>(std::move(d))) {}

    // --- what is it -----------------------------------------------------
    VType type() const { return t_; }
    const char *type_name() const { return vtype_name(t_); }
    static Variant lerp(const Variant &a, const Variant &b, float t);
    bool is_nil() const { return t_ == VType::Nil; }
    bool is_num() const { return t_ == VType::Int || t_ == VType::Float; }
    void clear();

    // --- getting it back ------------------------------------------------
    //
    // These CONVERT where a conversion is obvious -- an Int read as a
    // float, a Vec3 read as a Vec4 -- and return a default where it is
    // not. Scripting is full of a 2 that wanted to be a 2.0.
    bool to_bool() const;
    int64_t to_int() const;
    double to_float() const;
    std::string to_string() const;  // always works; this is the printer
    wr::Vec2 to_vec2() const;
    wr::Vec3 to_vec3() const;
    wr::Vec4 to_vec4() const;
    wr::Color to_color() const;
    wr::Quat to_quat() const;
    wr::Basis to_basis() const;
    wr::Transform3D to_transform() const;
    wr::Plane to_plane() const;
    wr::AABB to_aabb() const;
    wr::Rect2 to_rect2() const;
    wr::Projection to_projection() const;
    wr::Object *to_object() const;

    // Containers, by reference, creating on demand so that a caller can
    // build one up without a dance.
    Array &array();
    const Array *array_ptr() const;
    Dict &dict();
    const Dict *dict_ptr() const;

    bool operator==(const Variant &o) const;
    bool operator!=(const Variant &o) const { return !(*this == o); }

private:
    template <class T>
    T &as() {
        static_assert(sizeof(T) <= 64, "Variant buffer too small for this type");
        static_assert(std::is_trivially_copyable_v<T>, "Variant holds plain types only");
        return *reinterpret_cast<T *>(buf_);
    }
    template <class T>
    const T &as() const {
        return *reinterpret_cast<const T *>(buf_);
    }
    void copy_from(const Variant &o);
    void move_from(Variant &&o) noexcept;

    VType t_ = VType::Nil;
    // Big enough for a Projection (64) which is the largest plain type.
    alignas(16) unsigned char buf_[64] = {};
    std::string str_;
    std::shared_ptr<void> heap_;
};

}  // namespace wr
