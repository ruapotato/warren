// Warren -- Object, Ref, and the class registry.
//
// The registry is the reason this file exists. Every engine class
// declares its methods and properties once, in C++, and from that one
// declaration comes: the Python bindings, the property inspector, the
// scene serialiser and the error message when a script calls something
// that is not there. Writing bindings by hand for a scene tree is how
// engines end up with a scripting layer that lags the engine by a
// release; generating them from the same table the engine itself uses
// means they cannot.
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "variant.h"

namespace wr {

class Object;

// ---------------------------------------------------------------- Ref<T>

// Intrusive reference counting on every Object, not just resources.
// A Node is held by its parent, a Resource by whoever loaded it and a
// Variant by whatever it was put in, and all three are the same
// mechanism -- so a Python script holding a node that its parent then
// frees gets a live object, not a crash.
template <class T>
class Ref {
public:
    Ref() = default;
    Ref(std::nullptr_t) {}
    Ref(T *p) : ptr_(p) { retain(); }
    Ref(const Ref &o) : ptr_(o.ptr_) { retain(); }
    template <class U>
    Ref(const Ref<U> &o) : ptr_(static_cast<T *>(o.get())) { retain(); }
    Ref(Ref &&o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    ~Ref() { release(); }

    Ref &operator=(const Ref &o) {
        if (ptr_ != o.ptr_) { release(); ptr_ = o.ptr_; retain(); }
        return *this;
    }
    Ref &operator=(Ref &&o) noexcept {
        if (this != &o) { release(); ptr_ = o.ptr_; o.ptr_ = nullptr; }
        return *this;
    }
    Ref &operator=(T *p) {
        if (ptr_ != p) { release(); ptr_ = p; retain(); }
        return *this;
    }

    T *get() const { return ptr_; }
    T *operator->() const { return ptr_; }
    T &operator*() const { return *ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    bool operator==(const Ref &o) const { return ptr_ == o.ptr_; }
    bool operator!=(const Ref &o) const { return ptr_ != o.ptr_; }
    bool is_valid() const { return ptr_ != nullptr; }
    void reset() { release(); ptr_ = nullptr; }

private:
    void retain();
    void release();
    T *ptr_ = nullptr;
};

// ----------------------------------------------------------- reflection

struct PropertyInfo {
    std::string name;
    VType type = VType::Nil;
    // For an Object-typed property, which class -- empty otherwise.
    std::string class_name;
    // NOT PART OF THE OBJECT'S STATE, only a view of it.
    //
    // A Node3D has `transform`, `position`, `basis`, `rotation`,
    // `euler`, `scale`, `global_transform` and `global_position`, and
    // they are all the same four-by-three matrix seen from different
    // angles. An inspector should show every one of them; a scene
    // file must store exactly one, because writing them all means
    // the last one read wins and a child's position comes back with
    // its parent's added in. Marked with ClassBuilder::transient().
    bool transient = false;
    std::function<Variant(const Object *)> get;
    std::function<void(Object *, const Variant &)> set;
    std::string hint;  // free text for an editor: "range:0,1", "file", ...
};

struct MethodInfo {
    std::string name;
    VType ret = VType::Nil;
    std::vector<VType> args;
    // Optional, and supplied by .args(...) at the call site, because
    // C++ does not keep parameter names anywhere reflection can read
    // them. Where they are present they become the names in the
    // generated stubs and the keywords a script may call with.
    std::vector<std::string> arg_names;
    // For Object-typed arguments and returns, which class; empty
    // where the type is not an object. Parallel to `args`.
    std::string ret_class;
    std::vector<std::string> arg_classes;
    std::vector<Variant> defaults;  // tail-aligned
    // Vararg methods take whatever they are given.
    bool vararg = false;
    std::function<Variant(Object *, const Variant *, int)> call;
    int required() const { return int(args.size()) - int(defaults.size()); }
};

struct SignalInfo {
    std::string name;
    std::vector<VType> args;
};

struct ClassInfo {
    std::string name;
    ClassInfo *base = nullptr;
    std::function<Object *()> construct;  // null for abstract classes
    std::unordered_map<std::string, MethodInfo> methods;
    std::unordered_map<std::string, PropertyInfo> properties;
    std::unordered_map<std::string, SignalInfo> signals;
    // Declaration order, for an inspector that should not be a hash set.
    std::vector<std::string> property_order;
    std::vector<std::string> method_order;

    const MethodInfo *find_method(const std::string &n) const;
    const PropertyInfo *find_property(const std::string &n) const;
    const SignalInfo *find_signal(const std::string &n) const;
    bool derives_from(const ClassInfo *other) const;
};

class ClassDB {
public:
    static ClassInfo *get(const std::string &name);
    static ClassInfo *create(const std::string &name, ClassInfo *base);
    static Object *instantiate(const std::string &name);
    static std::vector<ClassInfo *> all();
    // Run every registration function that has been queued. Called once
    // at start-up; classes register themselves through static
    // initialisers, which cannot depend on each other's order, so the
    // work is deferred to here where the order is known.
    static void register_all();
    static void add_registrar(void (*fn)());
};

// ----------------------------------------------------------------- Object

enum class CallError {
    Ok = 0,
    NoMethod,
    TooFewArgs,
    TooManyArgs,
    BadArg,
    Failed,
};
const char *call_error_name(CallError e);

class Object {
public:
    Object() = default;
    virtual ~Object();
    Object(const Object &) = delete;
    Object &operator=(const Object &) = delete;

    virtual const char *get_class_name() const { return "Object"; }
    virtual ClassInfo *get_class_info() const { return class_info_static(); }
    static ClassInfo *class_info_static();

    bool is_class(const std::string &name) const;
    template <class T>
    T *cast_to() {
        return is_class(T::class_name_static()) ? static_cast<T *>(this) : nullptr;
    }
    template <class T>
    const T *cast_to() const {
        return is_class(T::class_name_static()) ? static_cast<const T *>(this) : nullptr;
    }

    // --- reflection -------------------------------------------------
    Variant call(const std::string &method, const Variant *args, int argc,
                 CallError *err = nullptr);
    Variant callv(const std::string &method, const Array &args,
                  CallError *err = nullptr) {
        return call(method, args.data(), int(args.size()), err);
    }
    bool has_method(const std::string &method) const;
    bool set(const std::string &prop, const Variant &value);
    Variant get(const std::string &prop) const;
    bool has_property(const std::string &prop) const;
    std::vector<std::string> property_list() const;

    // --- signals ------------------------------------------------------
    using Callback = std::function<void(const Variant *, int)>;
    // Returns a token the caller can disconnect with. A connection made
    // to a method on another Object is dropped automatically when that
    // object dies, which is the bug this indirection exists to prevent.
    uint64_t connect(const std::string &signal, Object *target, Callback cb);
    void disconnect(uint64_t token);
    void emit(const std::string &signal, const Variant *args = nullptr,
              int argc = 0);
    void emitv(const std::string &signal, const Array &args) {
        emit(signal, args.data(), int(args.size()));
    }

    // --- lifetime -------------------------------------------------------
    void ref_retain() const { ++refcount_; }
    // Returns true when this call destroyed it.
    bool ref_release() const;
    int ref_count() const { return refcount_; }

    // The Python object standing in for this one, if a script has ever
    // asked for it. Owned by the script layer; cleared on destruction so
    // the peer can be told.
    void *script_peer = nullptr;
    // Called on destruction so the script layer can invalidate the peer
    // rather than leave Python holding a dead pointer.
    static void set_peer_destructor(void (*fn)(Object *));

    std::string to_string() const;

protected:
    struct Connection {
        uint64_t token;
        std::string signal;
        Object *target;
        Callback cb;
    };
    std::vector<Connection> connections_;

private:
    mutable int refcount_ = 0;
};

template <class T>
void Ref<T>::retain() {
    if (ptr_) static_cast<const Object *>(ptr_)->ref_retain();
}
template <class T>
void Ref<T>::release() {
    if (ptr_) static_cast<const Object *>(ptr_)->ref_release();
}

// --------------------------------------------------------- the WR_CLASS macro

#define WR_CLASS(Cls, Base)                                                  \
public:                                                                      \
    using Self = Cls;                                                        \
    using Super = Base;                                                      \
    static const char *class_name_static() { return #Cls; }                  \
    const char *get_class_name() const override { return #Cls; }             \
    static ::wr::ClassInfo *class_info_static() {                            \
        static ::wr::ClassInfo *ci =                                         \
            ::wr::ClassDB::create(#Cls, Base::class_info_static());          \
        return ci;                                                           \
    }                                                                        \
    ::wr::ClassInfo *get_class_info() const override {                       \
        return class_info_static();                                          \
    }                                                                        \
                                                                             \
private:

// Put one of these at file scope to have `fn` run during
// ClassDB::register_all().
#define WR_REGISTER(fn)                                                      \
    namespace {                                                              \
    struct MfRegistrar_##fn {                                                \
        MfRegistrar_##fn() { ::wr::ClassDB::add_registrar(&fn); }            \
    };                                                                       \
    static MfRegistrar_##fn s_mf_registrar_##fn;                             \
    }

}  // namespace wr

#include "bind.h"
