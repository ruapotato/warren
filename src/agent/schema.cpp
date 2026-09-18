#include <algorithm>

#include "agent/agent.h"
#include "core/variant_json.h"
#include "core/object.h"

namespace wr {
namespace {

Json property_json(const PropertyInfo &p) {
    Json j = Json::object();
    j.set("name", p.name);
    j.set("type", type_label(p.type, p.class_name));
    if (!p.hint.empty()) {
        j.set("hint", p.hint);
        // A hint is free text for the inspector; the two shapes that
        // constrain a value are worth pulling out so a caller does
        // not have to parse English to know the legal range.
        if (p.hint.rfind("range:", 0) == 0) {
            double lo = 0, hi = 0;
            if (std::sscanf(p.hint.c_str() + 6, "%lf,%lf", &lo, &hi) == 2) {
                j.set("min", lo);
                j.set("max", hi);
            }
        } else if (p.hint.rfind("enum:", 0) == 0) {
            Json values = Json::array();
            std::string acc;
            for (const char *c = p.hint.c_str() + 5;; c++) {
                if (*c == ',' || *c == 0) {
                    if (!acc.empty()) values.push(Json(acc));
                    acc.clear();
                    if (*c == 0) break;
                } else {
                    acc.push_back(*c);
                }
            }
            j.set("values", values);
        }
    }
    if (!p.set) j.set("readonly", true);
    // Says "this is a view of other state, do not save it" -- which
    // also tells a caller that writing it may move several of the
    // others, and that two of these can disagree about who won.
    if (p.transient) j.set("derived", true);
    return j;
}

Json method_json(const MethodInfo &m) {
    Json j = Json::object();
    j.set("name", m.name);
    j.set("returns", type_label(m.ret, m.ret_class));
    Json args = Json::array();
    const int required = m.required();
    for (size_t i = 0; i < m.args.size(); i++) {
        Json a = Json::object();
        a.set("name", i < m.arg_names.size() && !m.arg_names[i].empty()
                          ? m.arg_names[i]
                          : "arg" + std::to_string(i));
        a.set("type", type_label(m.args[i],
                                 i < m.arg_classes.size() ? m.arg_classes[i] : ""));
        if (int(i) >= required) {
            a.set("optional", true);
            const size_t d = i - size_t(required);
            if (d < m.defaults.size()) a.set("default", variant_to_json(m.defaults[d]));
        }
        args.push(a);
    }
    j.set("args", args);
    if (m.vararg) j.set("vararg", true);
    return j;
}

Json signal_json(const SignalInfo &s) {
    Json j = Json::object();
    j.set("name", s.name);
    Json args = Json::array();
    for (VType t : s.args) args.push(Json(type_label(t)));
    j.set("args", args);
    return j;
}

// Declaration order where there is one, alphabetical for the rest --
// a hash-map order would make two dumps of the same engine differ,
// and a schema that changes between runs cannot be diffed or cached.
std::vector<std::string> ordered(const std::vector<std::string> &declared,
                                 const std::unordered_map<std::string, MethodInfo> &m) {
    std::vector<std::string> out = declared;
    std::vector<std::string> rest;
    for (const auto &kv : m)
        if (std::find(out.begin(), out.end(), kv.first) == out.end())
            rest.push_back(kv.first);
    std::sort(rest.begin(), rest.end());
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
}

void collect(const ClassInfo *ci, bool inherited, Json *props, Json *methods,
             Json *signals) {
    // Base first, so a derived class's own members read last and the
    // order matches how someone would describe the class.
    if (inherited && ci->base) collect(ci->base, true, props, methods, signals);
    std::vector<std::string> pnames = ci->property_order;
    for (const auto &kv : ci->properties)
        if (std::find(pnames.begin(), pnames.end(), kv.first) == pnames.end())
            pnames.push_back(kv.first);
    for (const std::string &n : pnames) {
        const PropertyInfo *p = ci->find_property(n);
        if (p) props->push(property_json(*p));
    }
    for (const std::string &n : ordered(ci->method_order, ci->methods)) {
        auto it = ci->methods.find(n);
        if (it != ci->methods.end()) methods->push(method_json(it->second));
    }
    std::vector<std::string> snames;
    for (const auto &kv : ci->signals) snames.push_back(kv.first);
    std::sort(snames.begin(), snames.end());
    for (const std::string &n : snames) signals->push(signal_json(ci->signals.at(n)));
}

Json class_json(const ClassInfo *ci, bool inherited) {
    Json j = Json::object();
    j.set("name", ci->name);
    if (ci->base) j.set("base", ci->base->name);
    // An abstract class cannot be the target of `create`, and saying
    // so here saves a failed attempt.
    j.set("instantiable", bool(ci->construct));
    Json props = Json::array(), methods = Json::array(), signals = Json::array();
    collect(ci, inherited, &props, &methods, &signals);
    j.set("properties", props);
    j.set("methods", methods);
    if (signals.size()) j.set("signals", signals);
    return j;
}

}  // namespace

Json agent_schema(const std::string &class_name, bool inherited) {
    if (!class_name.empty()) {
        ClassInfo *ci = ClassDB::get(class_name);
        if (!ci) return Json();
        return class_json(ci, inherited);
    }
    std::vector<ClassInfo *> all = ClassDB::all();
    std::sort(all.begin(), all.end(),
              [](ClassInfo *a, ClassInfo *b) { return a->name < b->name; });
    Json classes = Json::array();
    for (ClassInfo *ci : all) classes.push(class_json(ci, inherited));
    Json j = Json::object();
    j.set("engine", "warren");
    j.set("classes", classes);
    return j;
}

}  // namespace wr
