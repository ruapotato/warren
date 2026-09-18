// Warren -- turning C++ member functions into callable reflection.
//
// `ClassBuilder<Camera3D>().method("set_projection", &Camera3D::set_projection)`
// and the method is callable from Python, listable in an inspector and
// nameable in a saved scene. The template machinery below is the whole
// of the cost, paid once.
#pragma once

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// A HEADER THAT ONLY COMPILES SECOND IS NOT A HEADER. This used to
// rely on object.h having been included before it, which held right
// up until a translation unit wanted the reflection macros without
// the scene tree in front of them -- and then failed with forty
// lines of template error naming none of the cause.
#include "core/math/mathdefs.h"
#include "core/object.h"
#include "core/variant.h"

namespace wr {

template <class>
inline constexpr bool always_false_v = false;

template <class T>
struct is_ref_ptr : std::false_type {};
template <class T>
struct is_ref_ptr<Ref<T>> : std::true_type {
    using inner = T;
};

// ------------------------------------------------- Variant <-> C++ types

template <class T>
VType vtype_of() {
    using U = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (std::is_same_v<U, void>) return VType::Nil;
    else if constexpr (std::is_same_v<U, bool>) return VType::Bool;
    else if constexpr (std::is_enum_v<U>) return VType::Int;
    else if constexpr (std::is_integral_v<U>) return VType::Int;
    else if constexpr (std::is_floating_point_v<U>) return VType::Float;
    else if constexpr (std::is_same_v<U, std::string>) return VType::String;
    else if constexpr (std::is_same_v<U, const char *>) return VType::String;
    else if constexpr (std::is_same_v<U, Vec2>) return VType::Vec2;
    else if constexpr (std::is_same_v<U, Vec3>) return VType::Vec3;
    else if constexpr (std::is_same_v<U, Vec4>) return VType::Vec4;
    else if constexpr (std::is_same_v<U, Color>) return VType::Color;
    else if constexpr (std::is_same_v<U, Quat>) return VType::Quat;
    else if constexpr (std::is_same_v<U, Basis>) return VType::Basis;
    else if constexpr (std::is_same_v<U, Transform3D>) return VType::Transform;
    else if constexpr (std::is_same_v<U, Plane>) return VType::Plane;
    else if constexpr (std::is_same_v<U, AABB>) return VType::AABB;
    else if constexpr (std::is_same_v<U, Rect2>) return VType::Rect2;
    else if constexpr (std::is_same_v<U, Projection>) return VType::Projection;
    else if constexpr (std::is_same_v<U, Variant>) return VType::Nil;
    else if constexpr (std::is_same_v<U, Array>) return VType::Array;
    else if constexpr (std::is_same_v<U, Dict>) return VType::Dict;
    else if constexpr (is_ref_ptr<U>::value) return VType::Object;
    else if constexpr (std::is_pointer_v<U>) return VType::Object;
    else {
        static_assert(always_false_v<U>, "type cannot cross the Variant boundary");
        return VType::Nil;
    }
}

// WHICH Object SUBCLASS, for the ones that are objects.
//
// VType::Object says "some engine object" and loses the rest of what
// the C++ signature already knew. Nothing at run time needs the
// difference -- the call site checks the cast -- but a generated
// signature reading `-> Node | None` instead of `-> Object | None`
// is the difference between stubs worth having and stubs worth
// ignoring. Null for everything that is not an object.
template <class T>
const char *vclass_of() {
    using U = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (is_ref_ptr<U>::value)
        return is_ref_ptr<U>::inner::class_name_static();
    else if constexpr (std::is_pointer_v<U>) {
        using Inner = std::remove_cv_t<std::remove_pointer_t<U>>;
        if constexpr (std::is_base_of_v<Object, Inner>)
            return Inner::class_name_static();
        else
            return nullptr;
    } else {
        return nullptr;
    }
}

// A class name, or "" -- the form the registry stores.
template <class T>
std::string vclass_name() {
    const char *n = vclass_of<T>();
    return n ? n : std::string();
}

Object *variant_object_checked(const Variant &v, const char *want_class);

// RETURNS BY VALUE, ALWAYS.
//
// A bound method usually takes `const Vec3 &`, so T comes in as a
// reference type -- and returning `T` would then return a reference to
// the temporary this function just made. The value lives until the end
// of the full expression at the call site, which is exactly long
// enough to bind to the parameter, so stripping the reference here is
// both correct and free.
template <class T>
auto variant_to(const Variant &v) -> std::remove_cv_t<std::remove_reference_t<T>> {
    using U = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (std::is_same_v<U, bool>) return v.to_bool();
    else if constexpr (std::is_enum_v<U>) return U(v.to_int());
    else if constexpr (std::is_integral_v<U>) return U(v.to_int());
    else if constexpr (std::is_floating_point_v<U>) return U(v.to_float());
    else if constexpr (std::is_same_v<U, std::string>) return v.to_string();
    else if constexpr (std::is_same_v<U, Vec2>) return v.to_vec2();
    else if constexpr (std::is_same_v<U, Vec3>) return v.to_vec3();
    else if constexpr (std::is_same_v<U, Vec4>) return v.to_vec4();
    else if constexpr (std::is_same_v<U, Color>) return v.to_color();
    else if constexpr (std::is_same_v<U, Quat>) return v.to_quat();
    else if constexpr (std::is_same_v<U, Basis>) return v.to_basis();
    else if constexpr (std::is_same_v<U, Transform3D>) return v.to_transform();
    else if constexpr (std::is_same_v<U, Plane>) return v.to_plane();
    else if constexpr (std::is_same_v<U, AABB>) return v.to_aabb();
    else if constexpr (std::is_same_v<U, Rect2>) return v.to_rect2();
    else if constexpr (std::is_same_v<U, Projection>) return v.to_projection();
    else if constexpr (std::is_same_v<U, Variant>) return v;
    else if constexpr (std::is_same_v<U, Array>) {
        const Array *a = v.array_ptr();
        return a ? *a : Array();
    } else if constexpr (std::is_same_v<U, Dict>) {
        const Dict *d = v.dict_ptr();
        return d ? *d : Dict();
    } else if constexpr (is_ref_ptr<U>::value) {
        using Inner = typename is_ref_ptr<U>::inner;
        return U(static_cast<Inner *>(
            variant_object_checked(v, Inner::class_name_static())));
    } else if constexpr (std::is_pointer_v<U>) {
        using Inner = std::remove_pointer_t<U>;
        // Only an Object subclass can cross this boundary. A raw
        // pointer to anything else is almost always an OUT PARAMETER,
        // which reflection has no way to return -- bind a method that
        // returns the value instead.
        static_assert(std::is_base_of_v<Object, std::remove_cv_t<Inner>>,
                      "only Object* can cross the Variant boundary; a plain "
                      "pointer is usually an out parameter, which reflection "
                      "cannot express -- return the value instead");
        return static_cast<U>(
            variant_object_checked(v, Inner::class_name_static()));
    } else {
        static_assert(always_false_v<U>, "type cannot cross the Variant boundary");
    }
}

template <class T>
Variant variant_from(T &&value) {
    using U = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (std::is_same_v<U, Variant>) return std::forward<T>(value);
    else if constexpr (std::is_enum_v<U>) return Variant(int64_t(value));
    // Every integer width funnels to one Variant case. Without this a
    // method returning size_t or unsigned short fails to bind, with a
    // template error long enough to hide what is wrong with it.
    else if constexpr (std::is_integral_v<U> && !std::is_same_v<U, bool>)
        return Variant(int64_t(value));
    else if constexpr (is_ref_ptr<U>::value)
        return Variant(static_cast<Object *>(value.get()));
    else if constexpr (std::is_pointer_v<U>) {
        static_assert(
            std::is_base_of_v<Object, std::remove_cv_t<std::remove_pointer_t<U>>>,
            "only Object* can be returned through a Variant");
        return Variant(static_cast<Object *>(value));
    }
    else return Variant(std::forward<T>(value));
}

// ------------------------------------------------------------ the builder

namespace detail {

template <class C, class R, class... A, size_t... I>
Variant invoke(R (C::*fn)(A...), Object *self, const Variant *args,
               std::index_sequence<I...>) {
    C *c = static_cast<C *>(self);
    if constexpr (std::is_void_v<R>) {
        (c->*fn)(variant_to<A>(args[I])...);
        return Variant();
    } else {
        return variant_from((c->*fn)(variant_to<A>(args[I])...));
    }
}

template <class C, class R, class... A, size_t... I>
Variant invoke_const(R (C::*fn)(A...) const, const Object *self,
                     const Variant *args, std::index_sequence<I...>) {
    const C *c = static_cast<const C *>(self);
    if constexpr (std::is_void_v<R>) {
        (c->*fn)(variant_to<A>(args[I])...);
        return Variant();
    } else {
        return variant_from((c->*fn)(variant_to<A>(args[I])...));
    }
}

// Fill in trailing defaults and complain about anything still missing.
// `storage` must outlive the call.
bool marshal_args(const MethodInfo &mi, const Variant *in, int argc,
                  std::vector<Variant> &storage, const Variant **out,
                  CallError *err);

}  // namespace detail

template <class C>
class ClassBuilder {
public:
    // `constructible` false marks an abstract class: it appears in the
    // registry and its methods are inherited, but nothing can make one.
    explicit ClassBuilder(bool constructible = true) {
        ci_ = C::class_info_static();
        // `if constexpr`, not `if`. A runtime branch still compiles
        // the lambda, and `new C()` on a class with a pure virtual
        // is a hard error however unreachable it is -- so an
        // abstract class could be registered only by never
        // mentioning it here at all.
        if constexpr (!std::is_abstract_v<C>) {
            if (constructible)
                ci_->construct = []() -> Object * { return new C(); };
        } else {
            (void)constructible;
        }
    }

    ClassInfo *info() const { return ci_; }

    // NAMES FOR THE ARGUMENTS OF THE METHOD JUST BOUND.
    //
    // C++ throws parameter names away, so reflection cannot recover
    // them and a generated signature reads `f(a0, a1)`. Chaining
    // .args("from", "to") after a .method() supplies them, which is
    // what the stub generator prints and what a keyword call matches.
    // Optional everywhere: a method without them still works, it just
    // reads worse.
    // The property just declared is a VIEW of the object's state
    // rather than part of it: shown in an inspector, skipped by
    // anything that saves. See PropertyInfo::transient.
    ClassBuilder &transient() {
        if (!last_property_.empty()) {
            auto it = ci_->properties.find(last_property_);
            if (it != ci_->properties.end()) it->second.transient = true;
        }
        return *this;
    }

    template <class... S>
    ClassBuilder &args(S... names) {
        static_assert(sizeof...(S) > 0, "args() with no names does nothing");
        if (!last_method_.empty()) {
            auto it = ci_->methods.find(last_method_);
            if (it != ci_->methods.end()) {
                std::vector<std::string> given = {std::string(names)...};
                // A mismatch is a mistake at the call site that would
                // otherwise produce a signature quietly missing a
                // parameter, so it is truncated or padded rather than
                // shifting the names onto the wrong arguments.
                given.resize(it->second.args.size());
                for (size_t i = 0; i < given.size(); i++)
                    if (given[i].empty()) given[i] = "a" + std::to_string(i);
                it->second.arg_names = std::move(given);
            }
        }
        return *this;
    }

    template <class R, class... A>
    ClassBuilder &method(const char *name, R (C::*fn)(A...),
                         std::vector<Variant> defaults = {}) {
        MethodInfo mi;
        mi.name = name;
        mi.ret = vtype_of<R>();
        mi.ret_class = vclass_name<R>();
        mi.args = {vtype_of<A>()...};
        mi.arg_classes = {vclass_name<A>()...};
        mi.defaults = std::move(defaults);
        mi.call = [fn](Object *self, const Variant *args, int argc) -> Variant {
            (void)argc;
            return detail::invoke(fn, self, args, std::index_sequence_for<A...>{});
        };
        add(std::move(mi));
        return *this;
    }

    template <class R, class... A>
    ClassBuilder &method(const char *name, R (C::*fn)(A...) const,
                         std::vector<Variant> defaults = {}) {
        MethodInfo mi;
        mi.name = name;
        mi.ret = vtype_of<R>();
        mi.ret_class = vclass_name<R>();
        mi.args = {vtype_of<A>()...};
        mi.arg_classes = {vclass_name<A>()...};
        mi.defaults = std::move(defaults);
        mi.call = [fn](Object *self, const Variant *args, int argc) -> Variant {
            (void)argc;
            return detail::invoke_const(fn, self, args,
                                        std::index_sequence_for<A...>{});
        };
        add(std::move(mi));
        return *this;
    }

    // Something that wants the argument list itself.
    ClassBuilder &vararg(const char *name,
                         Variant (C::*fn)(const Variant *, int)) {
        MethodInfo mi;
        mi.name = name;
        mi.vararg = true;
        mi.call = [fn](Object *self, const Variant *args, int argc) -> Variant {
            return (static_cast<C *>(self)->*fn)(args, argc);
        };
        add(std::move(mi));
        return *this;
    }

    // A property backed by a getter and a setter.
    template <class G, class S>
    ClassBuilder &prop(const char *name, G (C::*getter)() const,
                       void (C::*setter)(S), const char *hint = "") {
        PropertyInfo pi;
        pi.name = name;
        pi.type = vtype_of<G>();
        pi.class_name = vclass_name<G>();
        pi.hint = hint;
        pi.get = [getter](const Object *o) {
            return variant_from((static_cast<const C *>(o)->*getter)());
        };
        pi.set = [setter](Object *o, const Variant &v) {
            (static_cast<C *>(o)->*setter)(variant_to<S>(v));
        };
        add(std::move(pi));
        return *this;
    }

    // A read-only property.
    template <class G>
    ClassBuilder &prop_ro(const char *name, G (C::*getter)() const,
                          const char *hint = "") {
        PropertyInfo pi;
        pi.name = name;
        pi.type = vtype_of<G>();
        pi.class_name = vclass_name<G>();
        pi.hint = hint;
        pi.get = [getter](const Object *o) {
            return variant_from((static_cast<const C *>(o)->*getter)());
        };
        add(std::move(pi));
        return *this;
    }

    // A plain data member, exposed directly. Most of a node's knobs are
    // this and there is no reason to write two accessors for them.
    template <class T>
    ClassBuilder &field(const char *name, T C::*member, const char *hint = "") {
        PropertyInfo pi;
        pi.name = name;
        pi.type = vtype_of<T>();
        pi.class_name = vclass_name<T>();
        pi.hint = hint;
        pi.get = [member](const Object *o) {
            return variant_from(static_cast<const C *>(o)->*member);
        };
        pi.set = [member](Object *o, const Variant &v) {
            static_cast<C *>(o)->*member = variant_to<T>(v);
        };
        add(std::move(pi));
        return *this;
    }

    ClassBuilder &signal(const char *name, std::vector<VType> args = {}) {
        SignalInfo si;
        si.name = name;
        si.args = std::move(args);
        ci_->signals[si.name] = std::move(si);
        return *this;
    }

private:
    void add(MethodInfo &&mi) {
        std::string n = mi.name;
        if (!ci_->methods.count(n)) ci_->method_order.push_back(n);
        last_method_ = n;
        ci_->methods[n] = std::move(mi);
    }
    void add(PropertyInfo &&pi) {
        std::string n = pi.name;
        if (!ci_->properties.count(n)) ci_->property_order.push_back(n);
        last_property_ = n;
        ci_->properties[n] = std::move(pi);
    }
    ClassInfo *ci_ = nullptr;
    std::string last_method_;
    std::string last_property_;
};

}  // namespace wr
