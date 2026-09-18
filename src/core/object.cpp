#include "object.h"

#include <algorithm>

#include "log.h"

namespace mf {

// ------------------------------------------------------------- ClassInfo

const MethodInfo *ClassInfo::find_method(const std::string &n) const {
    for (const ClassInfo *c = this; c; c = c->base) {
        auto it = c->methods.find(n);
        if (it != c->methods.end()) return &it->second;
    }
    return nullptr;
}

const PropertyInfo *ClassInfo::find_property(const std::string &n) const {
    for (const ClassInfo *c = this; c; c = c->base) {
        auto it = c->properties.find(n);
        if (it != c->properties.end()) return &it->second;
    }
    return nullptr;
}

const SignalInfo *ClassInfo::find_signal(const std::string &n) const {
    for (const ClassInfo *c = this; c; c = c->base) {
        auto it = c->signals.find(n);
        if (it != c->signals.end()) return &it->second;
    }
    return nullptr;
}

bool ClassInfo::derives_from(const ClassInfo *other) const {
    for (const ClassInfo *c = this; c; c = c->base)
        if (c == other) return true;
    return false;
}

// --------------------------------------------------------------- ClassDB

namespace {

// Deliberately function-local: these are touched from static
// initialisers, and a namespace-scope container might not be built yet.
std::unordered_map<std::string, ClassInfo *> &registry() {
    static std::unordered_map<std::string, ClassInfo *> r;
    return r;
}
std::vector<void (*)()> &registrars() {
    static std::vector<void (*)()> r;
    return r;
}
bool g_registered = false;

}  // namespace

ClassInfo *ClassDB::get(const std::string &name) {
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : it->second;
}

ClassInfo *ClassDB::create(const std::string &name, ClassInfo *base) {
    auto it = registry().find(name);
    if (it != registry().end()) return it->second;
    ClassInfo *ci = new ClassInfo();
    ci->name = name;
    ci->base = base;
    registry()[name] = ci;
    return ci;
}

Object *ClassDB::instantiate(const std::string &name) {
    ClassInfo *ci = get(name);
    if (!ci) {
        MF_ERROR("ClassDB: no class named '%s'", name.c_str());
        return nullptr;
    }
    if (!ci->construct) {
        MF_ERROR("ClassDB: '%s' is abstract and cannot be instantiated",
                 name.c_str());
        return nullptr;
    }
    return ci->construct();
}

std::vector<ClassInfo *> ClassDB::all() {
    std::vector<ClassInfo *> v;
    v.reserve(registry().size());
    for (auto &kv : registry()) v.push_back(kv.second);
    std::sort(v.begin(), v.end(),
              [](ClassInfo *a, ClassInfo *b) { return a->name < b->name; });
    return v;
}

void ClassDB::add_registrar(void (*fn)()) {
    registrars().push_back(fn);
    // A class registered after start-up -- a plugin, a hot reload -- is
    // bound straight away rather than waiting for a call that will not
    // come again.
    if (g_registered && fn) fn();
}

void ClassDB::register_all() {
    if (g_registered) return;
    g_registered = true;
    for (auto fn : registrars())
        if (fn) fn();
    MF_INFO("ClassDB: %zu classes registered", registry().size());
}

// ---------------------------------------------------------------- Object

const char *call_error_name(CallError e) {
    switch (e) {
        case CallError::Ok: return "ok";
        case CallError::NoMethod: return "no such method";
        case CallError::TooFewArgs: return "too few arguments";
        case CallError::TooManyArgs: return "too many arguments";
        case CallError::BadArg: return "wrong argument type";
        case CallError::Failed: return "call failed";
    }
    return "?";
}

namespace {
void (*g_peer_destructor)(Object *) = nullptr;
uint64_t g_next_token = 1;
}  // namespace

void Object::set_peer_destructor(void (*fn)(Object *)) { g_peer_destructor = fn; }

ClassInfo *Object::class_info_static() {
    static ClassInfo *ci = ClassDB::create("Object", nullptr);
    return ci;
}

Object::~Object() {
    if (script_peer && g_peer_destructor) g_peer_destructor(this);
    script_peer = nullptr;
}

bool Object::ref_release() const {
    if (--refcount_ <= 0) {
        delete this;
        return true;
    }
    return false;
}

bool Object::is_class(const std::string &name) const {
    for (const ClassInfo *c = get_class_info(); c; c = c->base)
        if (c->name == name) return true;
    return false;
}

std::string Object::to_string() const {
    char b[128];
    std::snprintf(b, sizeof(b), "<%s #%p>", get_class_name(), (const void *)this);
    return b;
}

namespace detail {

bool marshal_args(const MethodInfo &mi, const Variant *in, int argc,
                  std::vector<Variant> &storage, const Variant **out,
                  CallError *err) {
    if (mi.vararg) {
        *out = in;
        return true;
    }
    const int want = int(mi.args.size());
    const int need = mi.required();
    if (argc > want) {
        if (err) *err = CallError::TooManyArgs;
        return false;
    }
    if (argc < need) {
        if (err) *err = CallError::TooFewArgs;
        return false;
    }
    if (argc == want) {
        *out = in;
        return true;
    }
    // Trailing defaults are tail-aligned against the parameter list.
    storage.assign(in, in + argc);
    for (int i = argc; i < want; i++)
        storage.push_back(mi.defaults[size_t(i - need)]);
    *out = storage.data();
    return true;
}

}  // namespace detail

Variant Object::call(const std::string &method, const Variant *args, int argc,
                     CallError *err) {
    if (err) *err = CallError::Ok;
    const MethodInfo *mi = get_class_info()->find_method(method);
    if (!mi) {
        if (err) *err = CallError::NoMethod;
        MF_ERROR("%s has no method '%s'", get_class_name(), method.c_str());
        return Variant();
    }
    std::vector<Variant> storage;
    const Variant *final_args = nullptr;
    CallError e = CallError::Ok;
    if (!detail::marshal_args(*mi, args, argc, storage, &final_args, &e)) {
        if (err) *err = e;
        MF_ERROR("%s.%s: %s (given %d, wants %d..%zu)", get_class_name(),
                 method.c_str(), call_error_name(e), argc, mi->required(),
                 mi->args.size());
        return Variant();
    }
    return mi->call(this, final_args, argc);
}

bool Object::has_method(const std::string &method) const {
    return get_class_info()->find_method(method) != nullptr;
}

bool Object::set(const std::string &prop, const Variant &value) {
    const PropertyInfo *pi = get_class_info()->find_property(prop);
    if (!pi || !pi->set) {
        MF_ERROR("%s has no writable property '%s'", get_class_name(),
                 prop.c_str());
        return false;
    }
    pi->set(this, value);
    return true;
}

Variant Object::get(const std::string &prop) const {
    const PropertyInfo *pi = get_class_info()->find_property(prop);
    if (!pi || !pi->get) {
        MF_ERROR("%s has no readable property '%s'", get_class_name(),
                 prop.c_str());
        return Variant();
    }
    return pi->get(this);
}

bool Object::has_property(const std::string &prop) const {
    return get_class_info()->find_property(prop) != nullptr;
}

std::vector<std::string> Object::property_list() const {
    std::vector<std::string> out;
    // Base class first, so an inspector reads from general to specific.
    std::vector<const ClassInfo *> chain;
    for (const ClassInfo *c = get_class_info(); c; c = c->base) chain.push_back(c);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        for (const auto &n : (*it)->property_order) out.push_back(n);
    return out;
}

uint64_t Object::connect(const std::string &signal, Object *target, Callback cb) {
    if (!get_class_info()->find_signal(signal))
        MF_WARN("%s: connecting to undeclared signal '%s'", get_class_name(),
                signal.c_str());
    Connection c;
    c.token = g_next_token++;
    c.signal = signal;
    c.target = target;
    c.cb = std::move(cb);
    connections_.push_back(std::move(c));
    return connections_.back().token;
}

void Object::disconnect(uint64_t token) {
    connections_.erase(
        std::remove_if(connections_.begin(), connections_.end(),
                       [token](const Connection &c) { return c.token == token; }),
        connections_.end());
}

void Object::emit(const std::string &signal, const Variant *args, int argc) {
    // A handler may connect or disconnect while running, so the list is
    // copied first. Emitting into a vector being mutated underneath is
    // the classic way a signal system corrupts itself.
    std::vector<Connection> snapshot;
    snapshot.reserve(connections_.size());
    for (const auto &c : connections_)
        if (c.signal == signal) snapshot.push_back(c);
    for (const auto &c : snapshot)
        if (c.cb) c.cb(args, argc);
}

Object *variant_object_checked(const Variant &v, const char *want_class) {
    Object *o = v.to_object();
    if (!o) return nullptr;
    if (want_class && *want_class && !o->is_class(want_class)) {
        MF_ERROR("expected a %s, got a %s", want_class, o->get_class_name());
        return nullptr;
    }
    return o;
}

}  // namespace mf
