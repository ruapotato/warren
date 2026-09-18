#include "agent/agent.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "core/variant_json.h"
#include "app/engine.h"
#include "core/log.h"
#include "core/suggest.h"
#include "procgen/sdf.h"
#include "procgen/texture.h"
#include "render/material.h"
#include "render/mesh.h"
#include "resource/packed_scene.h"
#include "resource/resource.h"
#include "scene/node.h"
#include "scene/nodes.h"
#include "scene/scene_tree.h"
#include "script/python.h"

namespace wr {
namespace {

// --------------------------------------------------------------- misses

// THE POINT OF THIS FILE, more or less.
//
// A caller that guesses a name wrong gets told the names that are
// close to it. "colour" comes back with "color"; "set_pos" comes back
// with "set_position". The alternative is "no such property", which
// tells the caller only that its next guess should be different --
// and a model that has to guess three times to set a position is a
// model nobody wants driving an engine.
Json near_misses(const std::string &wanted, const std::vector<std::string> &have) {
    Json out = Json::array();
    for (const std::string &n : suggest(wanted, have)) out.push(Json(n));
    return out;
}

Json fail(const std::string &code, const std::string &message) {
    Json j = Json::object();
    j.set("ok", false);
    j.set("error", message);
    j.set("code", code);
    return j;
}

Json ok() {
    Json j = Json::object();
    j.set("ok", true);
    return j;
}

// ---------------------------------------------------------------- lookup

std::vector<std::string> class_names() {
    std::vector<std::string> names;
    for (ClassInfo *ci : ClassDB::all()) names.push_back(ci->name);
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> property_names(const ClassInfo *ci, bool inherited = true) {
    std::vector<std::string> out;
    for (const ClassInfo *c = ci; c; c = c->base) {
        for (const auto &kv : c->properties) out.push_back(kv.first);
        if (!inherited) break;
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<std::string> method_names(const ClassInfo *ci) {
    std::vector<std::string> out;
    for (const ClassInfo *c = ci; c; c = c->base)
        for (const auto &kv : c->methods) out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

const PropertyInfo *find_property(const ClassInfo *ci, const std::string &name) {
    for (const ClassInfo *c = ci; c; c = c->base)
        if (const PropertyInfo *p = c->find_property(name)) return p;
    return nullptr;
}

const MethodInfo *find_method(const ClassInfo *ci, const std::string &name) {
    for (const ClassInfo *c = ci; c; c = c->base)
        if (const MethodInfo *m = c->find_method(name)) return m;
    return nullptr;
}

// Every node in the tree, for suggesting a path that does exist.
void collect_paths(Node *n, std::vector<std::string> *out, size_t limit) {
    if (out->size() >= limit) return;
    out->push_back(n->path());
    for (const Ref<Node> &c : n->children()) collect_paths(c.get(), out, limit);
}

// A path resolved against the root, accepting the leading slash or
// not, and "/" or "" for the root itself.
Node *resolve(const AgentContext &ctx, const std::string &path, Json *err) {
    SceneTree *tree = ctx.scene_tree();
    Node *root = tree ? tree->root() : nullptr;
    if (!root) {
        if (err) *err = fail("no_tree", "there is no scene tree yet");
        return nullptr;
    }
    std::string p = path;
    if (p.empty() || p == "/" || p == "." || p == root->name() ||
        p == "/" + root->name())
        return root;
    if (p[0] == '/') p.erase(0, 1);
    // Paths print with the root's name on the front, so accept them
    // back in the form they were given out.
    const std::string prefix = root->name() + "/";
    if (p.rfind(prefix, 0) == 0) p.erase(0, prefix.size());
    Node *n = root->find_path(p);
    if (!n && err) {
        Json j = fail("no_such_node", "no node at \"" + path + "\"");
        // MATCH ON THE LAST SEGMENT, not the whole path.
        //
        // A caller that has the wrong path usually has the right
        // name: it knows there is a Player and guessed it lived
        // somewhere else. Comparing "/World/Player" with
        // "/root/Demo/Player" as strings finds nothing, because
        // almost every character differs -- comparing "Player" with
        // "Player" finds it at once.
        std::vector<std::string> paths;
        collect_paths(root, &paths, 4000);
        const size_t slash = path.find_last_of('/');
        const std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
        std::vector<std::string> leaves;
        for (const std::string &full : paths) {
            const size_t k = full.find_last_of('/');
            leaves.push_back(k == std::string::npos ? full : full.substr(k + 1));
        }
        Json near = near_misses(leaf, leaves);
        Json full_paths = Json::array();
        for (size_t i = 0; i < near.size(); i++)
            for (size_t k = 0; k < paths.size(); k++)
                if (leaves[k] == near[i].string()) full_paths.push(Json(paths[k]));
        if (full_paths.size()) j.set("did_you_mean", full_paths);
        *err = j;
    }
    return n;
}

Json node_brief(Node *n) {
    Json j = Json::object();
    j.set("path", n->path());
    j.set("name", n->name());
    j.set("class", n->get_class_name());
    if (n->child_count()) j.set("children", n->child_count());
    if (!n->scene_path().empty()) j.set("instance_of", n->scene_path());
    if (!n->groups().empty()) {
        Json g = Json::array();
        for (const std::string &s : n->groups()) g.push(Json(s));
        j.set("groups", g);
    }
    return j;
}

Json node_tree(Node *n, int depth) {
    Json j = node_brief(n);
    if (depth != 0 && n->child_count()) {
        Json kids = Json::array();
        for (const Ref<Node> &c : n->children())
            kids.push(node_tree(c.get(), depth - 1));
        j.set("nodes", kids);
    }
    return j;
}

std::string g_capture;

void capture_sink(LogLevel, const char *message) {
    g_capture += message;
    g_capture += "\n";
}

// BOTH HALVES OF THE ANSWER TO A MISSED PROPERTY.
//
// Either the name was wrong, in which case the near misses on this
// class are what the caller wanted; or the name was right and the
// NODE was wrong -- someone set "mesh" on the Node3D above the
// MeshInstance3D -- in which case the classes that do have it turn a
// dead end into a redirection.
void annotate_property_miss(Json *j, const ClassInfo *ci, const std::string &name) {
    Json near = near_misses(name, property_names(ci));
    if (near.size()) j->set("did_you_mean", near);
    Json classes = Json::array();
    for (ClassInfo *other : ClassDB::all())
        if (other->find_property(name) && classes.size() < 8)
            classes.push(Json(other->name));
    if (classes.size()) j->set("property_exists_on", classes);
}

// --------------------------------------------------------------- commands

struct Command {
    const char *name;
    const char *summary;
    const char *args;  // "name:type" pairs, "?" marking optional
    Json (*run)(const AgentContext &, const Json &);
    // Needs frames to have happened, not merely a tree.
    bool needs_engine = false;
};

Json cmd_help(const AgentContext &, const Json &) {
    Json j = ok();
    j.set("commands", agent_help()["commands"]);
    return j;
}

Json cmd_schema(const AgentContext &, const Json &c) {
    const std::string cls = c["class"].string();
    const bool inherited = c["inherited"].boolean();
    Json s = agent_schema(cls, inherited);
    if (!cls.empty() && s.type() == Json::Type::Null) {
        Json j = fail("no_such_class", "no class named \"" + cls + "\"");
        Json near = near_misses(cls, class_names());
        if (near.size()) j.set("did_you_mean", near);
        return j;
    }
    Json j = ok();
    if (cls.empty()) {
        j.set("classes", s["classes"]);
    } else {
        j.set("class", s);
    }
    return j;
}

Json cmd_classes(const AgentContext &, const Json &c) {
    const std::string base = c["base"].string();
    ClassInfo *want = base.empty() ? nullptr : ClassDB::get(base);
    if (!base.empty() && !want) {
        Json j = fail("no_such_class", "no class named \"" + base + "\"");
        Json near = near_misses(base, class_names());
        if (near.size()) j.set("did_you_mean", near);
        return j;
    }
    std::vector<ClassInfo *> all = ClassDB::all();
    std::sort(all.begin(), all.end(),
              [](ClassInfo *a, ClassInfo *b) { return a->name < b->name; });
    Json list = Json::array();
    for (ClassInfo *ci : all) {
        if (want && !ci->derives_from(want)) continue;
        Json e = Json::object();
        e.set("name", ci->name);
        if (ci->base) e.set("base", ci->base->name);
        e.set("instantiable", bool(ci->construct));
        list.push(e);
    }
    Json j = ok();
    j.set("classes", list);
    return j;
}

Json cmd_tree(const AgentContext &ctx, const Json &c) {
    Json err;
    Node *n = resolve(ctx, c["path"].string(), &err);
    if (!n) return err;
    const int depth = c.has("depth") ? int(c["depth"].number()) : -1;
    Json j = ok();
    j.set("root", node_tree(n, depth));
    return j;
}

Json cmd_get(const AgentContext &ctx, const Json &c) {
    Json err;
    Node *n = resolve(ctx, c["path"].string(), &err);
    if (!n) return err;
    ClassInfo *ci = n->get_class_info();
    if (!c.has("property")) {
        // No property named: hand back the lot. An agent that has
        // just found a node usually wants to see it, and making that
        // one round trip instead of twenty is the difference between
        // an interface that is pleasant to drive and one that is not.
        Json props = Json::object();
        for (const std::string &name : property_names(ci)) {
            const PropertyInfo *p = find_property(ci, name);
            if (p && p->get) props.set(name, variant_to_json(p->get(n)));
        }
        Json j = ok();
        j.set("node", node_brief(n));
        j.set("properties", props);
        return j;
    }
    const std::string name = c["property"].string();
    const PropertyInfo *p = find_property(ci, name);
    if (!p || !p->get) {
        Json j = fail("no_such_property", std::string(n->get_class_name()) +
                                              " has no property \"" + name + "\"");
        annotate_property_miss(&j, ci, name);
        return j;
    }
    Json j = ok();
    j.set("value", variant_to_json(p->get(n)));
    j.set("type", type_label(p->type, p->class_name));
    return j;
}

// Shared by `set` and by the properties block of `create`.
Json apply_property(const AgentContext &ctx, Node *n, const std::string &name, const Json &value) {
    ClassInfo *ci = n->get_class_info();
    const PropertyInfo *p = find_property(ci, name);
    if (!p) {
        Json j = fail("no_such_property", std::string(n->get_class_name()) +
                                              " has no property \"" + name + "\"");
        annotate_property_miss(&j, ci, name);
        return j;
    }
    if (!p->set)
        return fail("readonly", name + " is read-only on " + n->get_class_name());

    Variant v;
    std::string error;
    if (p->type == VType::Object) {
        // An object-typed property is written as a path to another
        // node, or as null to clear it.
        if (value.type() == Json::Type::Null) {
            v = Variant((Object *)nullptr);
        } else if (value.type() == Json::Type::String) {
            Json e2;
            Node *target = resolve(ctx, value.string(), &e2);
            if (!target) return e2;
            v = Variant((Object *)target);
        } else if (value.type() == Json::Type::Object && value.has("resource")) {
            Ref<Resource> r = ResourceLoader::load(value["resource"].string());
            if (!r)
                return fail("load_failed",
                            "could not load \"" + value["resource"].string() + "\"");
            v = Variant((Object *)r.get());
        } else {
            return fail("bad_value", name + " takes a node path or {\"resource\": path}");
        }
    } else if (const Variant current = p->get ? p->get(n) : Variant();
               !json_to_variant(value, p->type, &v, &error, &current)) {
        Json j = fail("bad_value", name + ": " + error);
        j.set("expected", type_label(p->type, p->class_name));
        return j;
    }
    p->set(n, v);
    Json j = ok();
    // The value AS STORED, not as sent. A caller that wrote a
    // rotation in degrees gets the transform it actually produced,
    // and a caller whose value was clamped or normalised finds out
    // here rather than three commands later.
    if (p->get) j.set("value", variant_to_json(p->get(n)));
    return j;
}

Json cmd_set(const AgentContext &ctx, const Json &c) {
    Json err;
    Node *n = resolve(ctx, c["path"].string(), &err);
    if (!n) return err;
    if (c.has("properties")) {
        Json results = Json::object();
        for (const auto &kv : c["properties"].fields()) {
            Json r = apply_property(ctx, n, kv.first, kv.second);
            if (!r["ok"].boolean()) {
                // Stop at the first failure rather than applying half
                // of a batch and reporting success: a half-applied
                // node is worse than a rejected one because nothing
                // says which half.
                r.set("failed_on", kv.first);
                r.set("applied", results);
                return r;
            }
            results.set(kv.first, r["value"]);
        }
        Json j = ok();
        j.set("properties", results);
        return j;
    }
    if (!c.has("property")) return fail("missing_arg", "set needs \"property\" or \"properties\"");
    return apply_property(ctx, n, c["property"].string(), c["value"]);
}

Json cmd_call(const AgentContext &ctx, const Json &c) {
    Json err;
    Node *n = resolve(ctx, c["path"].string(), &err);
    if (!n) return err;
    const std::string name = c["method"].string();
    ClassInfo *ci = n->get_class_info();
    const MethodInfo *m = find_method(ci, name);
    if (!m) {
        Json j = fail("no_such_method", std::string(n->get_class_name()) +
                                            " has no method \"" + name + "\"");
        Json near = near_misses(name, method_names(ci));
        if (near.size()) j.set("did_you_mean", near);
        // THE GODOT REFLEX, which is the commonest miss of all.
        //
        // Nearly everything here that Godot exposes as
        // set_position() / get_position() is a property, so a caller
        // working from habit -- or from a model that has read a lot
        // of Godot -- reaches for a method that was never going to
        // exist. Saying "that is a property, use set" is the whole
        // answer, and it costs one string compare.
        std::string bare = name;
        const bool was_set = bare.rfind("set_", 0) == 0;
        const bool was_get = bare.rfind("get_", 0) == 0;
        if (was_set || was_get) bare.erase(0, 4);
        Json properties = near_misses(bare, property_names(ci));
        if (properties.size()) {
            j.set("but_there_is_a_property", properties);
            j.set("use", was_get ? "the get command" : "the set command");
        }
        return j;
    }
    const Json &given = c["args"];
    Array args;
    // Arguments by name as well as by position, because reflection
    // knows the names and a caller reading the schema will use them.
    if (given.type() == Json::Type::Object) {
        for (size_t i = 0; i < m->args.size(); i++) {
            const std::string an = i < m->arg_names.size() ? m->arg_names[i] : "";
            if (an.empty() || !given.has(an)) {
                if (int(i) >= m->required()) break;
                Json j = fail("missing_arg", name + " needs \"" +
                                                 (an.empty() ? "arg" + std::to_string(i) : an) +
                                                 "\"");
                j.set("signature", agent_schema(ci->name, true));
                return j;
            }
            Variant v;
            std::string error;
            if (!json_to_variant(given[an], m->args[i], &v, &error))
                return fail("bad_value", an + ": " + error);
            args.push_back(std::move(v));
        }
    } else {
        const size_t count = given.type() == Json::Type::Array ? given.size() : 0;
        if (!m->vararg && int(count) > int(m->args.size())) {
            Json j = fail("too_many_args", name + " takes at most " +
                                               std::to_string(m->args.size()) +
                                               " arguments, got " + std::to_string(count));
            return j;
        }
        if (int(count) < m->required()) {
            Json j = fail("too_few_args", name + " needs " +
                                              std::to_string(m->required()) +
                                              " arguments, got " + std::to_string(count));
            Json sig = Json::array();
            for (size_t i = 0; i < m->args.size(); i++)
                sig.push(Json((i < m->arg_names.size() ? m->arg_names[i]
                                                       : "arg" + std::to_string(i)) +
                              ": " + type_label(m->args[i])));
            j.set("signature", sig);
            return j;
        }
        for (size_t i = 0; i < count; i++) {
            const VType want = i < m->args.size() ? m->args[i] : VType::Nil;
            Variant v;
            std::string error;
            if (want == VType::Object && given[i].type() == Json::Type::String) {
                Json e2;
                Node *t = resolve(ctx, given[i].string(), &e2);
                if (!t) return e2;
                v = Variant((Object *)t);
            } else if (!json_to_variant(given[i], want, &v, &error)) {
                return fail("bad_value", "argument " + std::to_string(i) + ": " + error);
            }
            args.push_back(std::move(v));
        }
    }
    CallError ce = CallError::Ok;
    Variant result = n->callv(name, args, &ce);
    if (ce != CallError::Ok)
        return fail("call_failed", std::string(name) + ": " + call_error_name(ce));
    Json j = ok();
    j.set("result", variant_to_json(result));
    return j;
}

Json cmd_create(const AgentContext &ctx, const Json &c) {
    const std::string cls = c["class"].string();
    ClassInfo *ci = ClassDB::get(cls);
    if (!ci) {
        Json j = fail("no_such_class", "no class named \"" + cls + "\"");
        Json near = near_misses(cls, class_names());
        if (near.size()) j.set("did_you_mean", near);
        return j;
    }
    if (!ci->construct)
        return fail("abstract", cls + " is abstract and cannot be created");
    Json err;
    Node *parent = resolve(ctx, c["parent"].string(), &err);
    if (!parent) return err;

    Object *o = ClassDB::instantiate(cls);
    Node *n = o ? o->cast_to<Node>() : nullptr;
    if (!n) {
        delete o;
        return fail("not_a_node", cls + " is not a Node and cannot be added to the tree");
    }
    if (c.has("name")) n->set_name(c["name"].string());
    parent->add_child(n);

    if (c.has("properties")) {
        for (const auto &kv : c["properties"].fields()) {
            Json r = apply_property(ctx, n, kv.first, kv.second);
            if (!r["ok"].boolean()) {
                // Undo the node rather than leave a half-configured
                // one in the tree under a name the caller will reuse.
                n->free_from_parent();
                r.set("failed_on", kv.first);
                r.set("note", "the node was not created");
                return r;
            }
        }
    }
    Json j = ok();
    j.set("node", node_brief(n));
    return j;
}

Json cmd_delete(const AgentContext &ctx, const Json &c) {
    Json err;
    Node *n = resolve(ctx, c["path"].string(), &err);
    if (!n) return err;
    if (n == ctx.scene_tree()->root()) return fail("refused", "the root node cannot be deleted");
    const std::string path = n->path();
    n->free_from_parent();
    Json j = ok();
    j.set("deleted", path);
    return j;
}

Json cmd_reparent(const AgentContext &ctx, const Json &c) {
    Json err;
    Node *n = resolve(ctx, c["path"].string(), &err);
    if (!n) return err;
    Node *to = resolve(ctx, c["to"].string(), &err);
    if (!to) return err;
    if (n == ctx.scene_tree()->root()) return fail("refused", "the root node cannot be moved");
    for (Node *p = to; p; p = p->parent())
        if (p == n)
            return fail("refused", "that would make \"" + n->path() +
                                       "\" its own ancestor");
    // A reference across the move, because remove_child drops the
    // parent's and would otherwise be the last one.
    Ref<Node> keep(n);
    // Keep the node where it is in the world. Moving a node between
    // parents in an editor never moves the thing on screen, and an
    // agent expects the same.
    Node3D *n3 = n->cast_to<Node3D>();
    Node3D *to3 = to->cast_to<Node3D>();
    const bool keep_world = n3 && to3;
    const Transform3D world = keep_world ? n3->global_transform() : Transform3D();
    n->parent()->remove_child(n);
    to->add_child(n);
    if (keep_world) n3->set_global_transform(world);
    Json j = ok();
    j.set("node", node_brief(n));
    return j;
}

Json cmd_find(const AgentContext &ctx, const Json &c) {
    Json err;
    Node *from = resolve(ctx, c["path"].string(), &err);
    if (!from) return err;
    const std::string cls = c["class"].string();
    const std::string name = c["name"].string();
    const std::string group = c["group"].string();
    if (!cls.empty() && !ClassDB::get(cls)) {
        Json j = fail("no_such_class", "no class named \"" + cls + "\"");
        Json near = near_misses(cls, class_names());
        if (near.size()) j.set("did_you_mean", near);
        return j;
    }
    Json out = Json::array();
    const size_t limit = c.has("limit") ? size_t(c["limit"].number()) : 200;
    // Iterative, because a deep tree and a recursive walk is a stack
    // overflow reported as a crash.
    std::vector<Node *> stack{from};
    while (!stack.empty() && out.size() < limit) {
        Node *n = stack.back();
        stack.pop_back();
        for (int i = n->child_count() - 1; i >= 0; i--) stack.push_back(n->child(i));
        if (!cls.empty() && !n->is_class(cls)) continue;
        // A bare substring, which is what a caller means by "door".
        if (!name.empty() && n->name().find(name) == std::string::npos) continue;
        if (!group.empty() && !n->in_group(group)) continue;
        out.push(node_brief(n));
    }
    Json j = ok();
    j.set("nodes", out);
    j.set("count", int(out.size()));
    return j;
}

Json cmd_eval(const AgentContext &, const Json &c) {
    const std::string code = c["code"].string();
    if (code.empty()) return fail("missing_arg", "eval needs \"code\"");
    // Captured, not printed: a reply that says what happened beats
    // one the caller has to go and read the log for. A sink is a bare
    // function pointer, so the buffer has to be a static -- which is
    // fine, because commands are executed one at a time.
    g_capture.clear();
    log_add_sink(capture_sink);
    std::string value;
    bool okay;
    if (c["statement"].boolean() || code.find('\n') != std::string::npos) {
        okay = Python::run_string(code, "<agent>");
    } else {
        value = Python::eval_repr(code, &okay);
        if (!okay) {
            // "x = 1" is not an expression and never will be, but it
            // is obviously what the caller meant. Try it as a
            // statement before reporting a failure -- and keep the
            // ORIGINAL error if that fails too, since the expression
            // error is the one that describes what was actually
            // wrong.
            const std::string expression_error = value;
            value.clear();
            okay = Python::run_string(code, "<agent>");
            if (!okay) value = expression_error;
        }
    }
    log_remove_sink(capture_sink);
    // The failure is already in "error"; repeating the log line the
    // interpreter wrote about it as "output" says nothing new.
    const std::string captured = okay ? g_capture : std::string();
    Json j = okay ? ok() : fail("python_error", value.empty() ? captured : value);
    if (!value.empty() && okay) j.set("value", value);
    if (!captured.empty()) j.set("output", captured);
    return j;
}

Json cmd_step(const AgentContext &ctx, const Json &c) {
    Engine *e = ctx.engine;
    const int frames = c.has("frames") ? std::max(1, int(c["frames"].number())) : 1;
    int done = 0;
    bool running = true;
    for (int i = 0; i < frames && running; i++) {
        running = e->step();
        done++;
    }
    Json j = ok();
    j.set("frames", done);
    j.set("total_frames", double(e->frames()));
    j.set("running", running);
    return j;
}

Json cmd_screenshot(const AgentContext &ctx, const Json &c) {
    Engine *e = ctx.engine;
    std::string path = c["path"].string();
    if (path.empty()) path = "agent_shot.png";
    // A frame, so the picture shows the commands that just ran rather
    // than the state before them.
    if (!c.has("step") || c["step"].boolean()) e->step();
    if (!e->save_screenshot(path)) return fail("screenshot_failed", "could not write " + path);
    Json j = ok();
    j.set("path", path);
    return j;
}

Json cmd_save_scene(const AgentContext &ctx, const Json &c) {
    const std::string path = c["path"].string();
    if (path.empty()) return fail("missing_arg", "save_scene needs \"path\"");
    Json err;
    Node *n = resolve(ctx, c["node"].string(), &err);
    if (!n) return err;
    Ref<PackedScene> ps(new PackedScene());
    ps->pack(n);
    if (!ResourceSaver::save(ps.get(), path))
        return fail("save_failed", "could not write " + path);
    Json j = ok();
    j.set("path", path);
    return j;
}

Json cmd_load_scene(const AgentContext &ctx, const Json &c) {
    const std::string path = c["path"].string();
    if (path.empty()) return fail("missing_arg", "load_scene needs \"path\"");
    Ref<Resource> r = ResourceLoader::load(path);
    PackedScene *ps = r ? r->cast_to<PackedScene>() : nullptr;
    if (!ps) return fail("load_failed", "could not load a scene from " + path);
    Node *n = ps->instantiate();
    if (!n) return fail("load_failed", path + " produced no nodes");
    if (c.has("parent")) {
        Json err;
        Node *parent = resolve(ctx, c["parent"].string(), &err);
        if (!parent) { n->free_from_parent(); return err; }
        parent->add_child(n);
    } else {
        ctx.scene_tree()->set_scene(n);
    }
    Json j = ok();
    j.set("node", node_brief(n));
    return j;
}

// ------------------------------------------------------- making things

// THE SHAPE LANGUAGE, DESCRIBED BY THE ENGINE ITSELF.
//
// An agent cannot write a shape it has not been told the grammar of,
// and a grammar in a README is a grammar that will be out of date.
// Every name here is checked by test_agent against the parser, so a
// primitive that stops parsing, or one that is added and not listed,
// fails the build rather than a user.
struct ShapeDoc {
    const char *name;
    const char *params;
    const char *note;
};

const ShapeDoc kShapes[] = {
    {"sphere", "radius=0.5", "centred on the origin, like all of them"},
    {"box", "size=[1,1,1], round=0", "round takes the corners off without growing it"},
    {"cylinder", "radius=0.5, height=1, round=0", "standing on Y"},
    {"capsule", "radius=0.25, height=1", "height includes the round ends"},
    {"cone", "radius=0.5, height=1, round=0", "tip up"},
    {"torus", "major=0.5, minor=0.15", "lying in the XZ plane"},
    {"half_space", "normal=[0,1,0], offset=0",
     "unbounded: only useful intersected with something, which is how you "
     "slice a shape flat"},
};

const ShapeDoc kOps[] = {
    {"union", "of=[...], blend=0", "everything; blend melts them together"},
    {"intersection", "of=[...], blend=0", "only where they overlap"},
    {"difference", "of=[...], blend=0", "the first one, minus all the rest"},
};

const ShapeDoc kModifiers[] = {
    {"at", "[x,y,z]", "where it goes; applied after everything else on the node"},
    {"rotate", "[x,y,z]", "degrees about each axis"},
    {"scale", "number", "uniform"},
    {"round", "number", "grow and round every edge"},
    {"shell", "number", "hollow it out, leaving walls that thick"},
    {"twist", "number", "turns per metre about Y"},
    {"bend", "number", "curvature about Z as X increases"},
    {"displace", "number, or {amplitude, scale, octaves, seed}",
     "fractal noise on the surface -- what turns a sphere into a rock"},
    {"elongate", "[x,y,z]", "stretch by sliding the halves apart, keeping the ends round"},
    {"mirror", "[x,y,z]", "nonzero axes are mirrored, so only half need be built"},
    {"repeat", "[x,y,z], count=[x,y,z]",
     "copies on a lattice; a count of 0 on an axis repeats for ever"},
    {"material", "number", "palette index for this part and everything under it"},
};

Json shape_doc_list(const ShapeDoc *docs, size_t n) {
    Json out = Json::array();
    for (size_t i = 0; i < n; i++) {
        Json j = Json::object();
        j.set("name", docs[i].name);
        j.set("params", docs[i].params);
        j.set("note", docs[i].note);
        out.push(j);
    }
    return out;
}

Json cmd_shapes(const AgentContext &, const Json &) {
    Json j = ok();
    j.set("about",
          "A shape is a signed distance field described as JSON, contoured "
          "into a mesh. One object per node: \"shape\" for a primitive or "
          "\"op\" with \"of\" for a combination, plus any modifiers on the "
          "same object. Booleans always work here -- they are min and max on "
          "the field, not surgery on triangles.");
    j.set("example",
          R"({"op":"difference","of":[{"shape":"box","size":[2,1,2],"round":0.05},)"
          R"({"shape":"cylinder","radius":0.3,"height":3,"at":[0.6,0,0.6]}]})");
    j.set("primitives", shape_doc_list(kShapes, sizeof(kShapes) / sizeof(kShapes[0])));
    j.set("operations", shape_doc_list(kOps, sizeof(kOps) / sizeof(kOps[0])));
    j.set("modifiers",
          shape_doc_list(kModifiers, sizeof(kModifiers) / sizeof(kModifiers[0])));
    return j;
}

Json cmd_make_mesh(const AgentContext &ctx, const Json &c) {
    if (!c.has("shape") && !c.has("sdf"))
        return fail("missing_arg",
                    "make_mesh needs \"shape\": the shape to build. Send the "
                    "\"shapes\" command for the grammar.");
    std::string error;
    gen::Sdf shape =
        gen::Sdf::from_json(c.has("shape") ? c["shape"] : c["sdf"], &error);
    if (!error.empty()) {
        Json j = fail("bad_shape", error);
        j.set("see", "the shapes command");
        return j;
    }

    gen::Sdf::MeshOptions options;
    if (c.has("cell_size")) options.cell_size = float(c["cell_size"].number());
    if (c.has("detail")) options.target_cells = int(c["detail"].number());
    if (c.has("smooth")) options.smooth_normals = c["smooth"].boolean();

    Ref<Mesh> mesh = shape.to_mesh(options, &error);
    if (!mesh) return fail("mesh_failed", error);

    Json j = ok();
    j.set("vertices", int(mesh->vertex_count()));
    j.set("triangles", int(mesh->triangle_count()));
    const AABB b = mesh->bounds();
    Json size = Json::object();
    size.set("x", double(b.max.x - b.min.x)).set("y", double(b.max.y - b.min.y));
    size.set("z", double(b.max.z - b.min.z));
    j.set("size", size);

    if (c.has("save")) {
        const std::string path = c["save"].string();
        if (!ResourceSaver::save(mesh.get(), path))
            return fail("save_failed", "could not write " + path);
        j.set("saved", path);
    }
    // Put it in the world, which is the point: an agent that builds
    // something wants to look at it, and looking at it means a node
    // and then a screenshot.
    if (c.has("parent")) {
        Json err;
        Node *parent = resolve(ctx, c["parent"].string(), &err);
        if (!parent) return err;
        MeshInstance3D *mi = new MeshInstance3D();
        mi->set_name(c.has("name") ? c["name"].string() : "Shape");
        mi->mesh = mesh;
        parent->add_child(mi);
        if (c.has("at")) {
            Json r = apply_property(ctx, mi, "position", c["at"]);
            if (!r["ok"].boolean()) { mi->free_from_parent(); return r; }
        }
        j.set("node", node_brief(mi));
    }
    return j;
}

Json cmd_surfaces(const AgentContext &, const Json &) {
    Json j = ok();
    const Json g = gen::texture_grammar();
    for (const auto &kv : g.fields()) j.set(kv.first, kv.second);
    return j;
}

Json cmd_make_material(const AgentContext &ctx, const Json &c) {
    if (!c.has("surface"))
        return fail("missing_arg",
                    "make_material needs \"surface\": the surface to build. Send "
                    "the \"surfaces\" command for the grammar.");
    const Json &spec = c["surface"];
    const uint32_t size =
        uint32_t(std::clamp(c.has("size") ? int(c["size"].number()) : 256, 16, 2048));
    std::string error;
    Json written = Json::array();

    // A PICTURE OF IT, whether or not there is a device.
    //
    // Saving the maps as PNGs is how an agent looks at what it made
    // without putting it in the world first, and it works headless,
    // which a GPU upload does not.
    if (c.has("save")) {
        const gen::SurfaceImages images = gen::render_surface(spec, size, &error);
        if (!error.empty()) {
            Json j = fail("bad_surface", error);
            j.set("see", "the surfaces command");
            return j;
        }
        const std::string stem = c["save"].string();
        struct { const char *suffix; const gen::Image *image; } maps[] = {
            {"_albedo.png", &images.albedo},
            {"_normal.png", &images.normal},
            {"_orm.png", &images.orm},
            {"_height.png", &images.height},
        };
        for (const auto &m : maps) {
            const std::string path = stem + m.suffix;
            if (!m.image->write_png(path))
                return fail("save_failed", "could not write " + path);
            written.push(Json(path));
        }
        if (!ctx.engine) {
            // No device to upload to, but the maps exist and that is
            // the whole job when the caller only wanted files.
            Json j = ok();
            j.set("saved", written);
            j.set("size", int(size));
            return j;
        }
    }

    if (!ctx.engine)
        return fail("no_engine",
                    "uploading a material needs a running engine; pass \"save\" to "
                    "write the maps as PNGs instead");

    Ref<Material> material =
        gen::make_material(ctx.engine->device(), spec, size, &error);
    if (!material) {
        Json j = fail("bad_surface", error);
        j.set("see", "the surfaces command");
        return j;
    }
    if (c.has("as")) {
        if (!ResourceSaver::save(material.get(), c["as"].string()))
            return fail("save_failed", "could not write " + c["as"].string());
    }

    Json j = ok();
    j.set("size", int(size));
    if (written.size()) j.set("saved", written);
    // Straight onto something, because that is what it is for.
    if (c.has("on")) {
        Json err;
        Node *n = resolve(ctx, c["on"].string(), &err);
        if (!n) return err;
        MeshInstance3D *mi = n->cast_to<MeshInstance3D>();
        if (!mi)
            return fail("not_a_mesh", c["on"].string() + " is a " +
                                          n->get_class_name() +
                                          ", which has no material to set");
        // A slot per submesh; naming one replaces just that one, and
        // naming none replaces them all, which is what somebody
        // texturing a whole prop means.
        const int slot = c.has("slot") ? int(c["slot"].number()) : -1;
        if (slot >= 0) {
            if (slot >= int(mi->materials.size()))
                mi->materials.resize(size_t(slot) + 1);
            mi->materials[size_t(slot)] = material;
        } else if (mi->materials.empty()) {
            mi->materials.push_back(material);
        } else {
            for (Ref<Material> &m : mi->materials) m = material;
        }
        j.set("applied_to", mi->path());
    }
    return j;
}

Json cmd_stats(const AgentContext &ctx, const Json &) {
    Engine *e = ctx.engine;
    Engine::FrameTimes ft = e->frame_times(0);
    Json j = ok();
    j.set("frames", double(e->frames()));
    Json t = Json::object();
    t.set("mean_ms", ft.mean_ms).set("p50_ms", ft.p50_ms).set("p95_ms", ft.p95_ms);
    t.set("p99_ms", ft.p99_ms).set("max_ms", ft.max_ms);
    if (ft.mean_ms > 0) t.set("fps", 1000.0 / ft.mean_ms);
    j.set("frame_time", t);
    j.set("status", e->status_line());
    return j;
}

const Command kCommands[] = {
    {"help", "every command, its arguments and what it returns", "", cmd_help},
    {"schema", "the engine API: classes, properties with types and ranges, methods with argument names",
     "class:string?, inherited:bool?", cmd_schema},
    {"classes", "just the class names and their bases, optionally only those under one base",
     "base:string?", cmd_classes},
    {"tree", "the scene tree from a node down", "path:string?, depth:int?", cmd_tree},
    {"get", "one property, or every property when none is named",
     "path:string, property:string?", cmd_get},
    {"set", "write one property, or several at once",
     "path:string, property:string?, value:any?, properties:object?", cmd_set},
    {"call", "call a method; args may be an array or an object keyed by argument name",
     "path:string, method:string, args:array|object?", cmd_call},
    {"create", "make a node and add it to the tree",
     "class:string, parent:string, name:string?, properties:object?", cmd_create},
    {"delete", "remove a node and everything under it", "path:string", cmd_delete},
    {"reparent", "move a node, keeping its world transform", "path:string, to:string", cmd_reparent},
    {"find", "search the tree by class, by name substring, or by group",
     "path:string?, class:string?, name:string?, group:string?, limit:int?", cmd_find},
    {"eval", "run Python in the engine's own interpreter and return the value and any output",
     "code:string, statement:bool?", cmd_eval},
    {"step", "advance the simulation by whole frames", "frames:int?", cmd_step, true},
    {"screenshot", "render and write a PNG, so the caller can look at what it built",
     "path:string?, step:bool?", cmd_screenshot, true},
    {"save_scene", "pack a node and its children into a scene file",
     "path:string, node:string?", cmd_save_scene},
    {"load_scene", "load a scene file as the current scene, or under a parent",
     "path:string, parent:string?", cmd_load_scene},
    {"shapes", "the procedural shape language: every primitive, operation and modifier",
     "", cmd_shapes},
    {"make_mesh", "build a mesh from a shape and, optionally, put it in the world",
     "shape:object, parent:string?, name:string?, at:vec3?, cell_size:float?, "
     "detail:int?, smooth:bool?, save:string?", cmd_make_mesh},
    {"surfaces", "the procedural surface language: patterns, blends and what a "
     "material is made of", "", cmd_surfaces},
    {"make_material", "build albedo, normal and roughness maps from one surface "
     "description",
     "surface:object, on:string?, slot:int?, size:int?, save:string?, as:string?",
     cmd_make_material},
    {"stats", "frame times and what the engine is doing", "", cmd_stats, true},
};

}  // namespace

Json agent_help() {
    Json list = Json::array();
    for (const Command &c : kCommands) {
        Json j = Json::object();
        j.set("cmd", c.name);
        j.set("summary", c.summary);
        if (c.args[0]) j.set("args", c.args);
        list.push(j);
    }
    Json j = Json::object();
    j.set("commands", list);
    j.set("protocol",
          "one JSON object per line in, one per line out; every reply has "
          "\"ok\", and a failure has \"error\", \"code\" and often "
          "\"did_you_mean\". An \"id\" on a command is echoed on its reply.");
    return j;
}

SceneTree *AgentContext::scene_tree() const {
    return tree ? tree : (engine ? engine->tree() : nullptr);
}

Json agent_execute(const AgentContext &ctx, const Json &command) {
    const std::string name = command["cmd"].string();
    Json reply;
    if (name.empty()) {
        reply = fail("missing_cmd", "every command needs a \"cmd\" field");
        Json names = Json::array();
        for (const Command &c : kCommands) names.push(Json(c.name));
        reply.set("commands", names);
    } else if (name == "quit" || name == "exit") {
        reply = ok();
        reply.set("quit", true);
    } else {
        const Command *found = nullptr;
        for (const Command &c : kCommands)
            if (name == c.name) found = &c;
        if (!found) {
            std::vector<std::string> names;
            for (const Command &c : kCommands) names.push_back(c.name);
            names.push_back("quit");
            reply = fail("no_such_command", "no command named \"" + name + "\"");
            Json near = near_misses(name, names);
            if (near.size()) reply.set("did_you_mean", near);
        } else if (found->needs_engine && !ctx.engine) {
            reply = fail("no_engine", std::string(found->name) +
                                          " needs a running engine, and this one is "
                                          "driving a scene tree on its own");
        } else {
            reply = found->run(ctx, command);
        }
    }
    // Echoed so a caller that pipelines commands can match replies to
    // them without counting lines.
    if (command.has("id")) reply.set("id", command["id"]);
    return reply;
}

int agent_serve(const AgentContext &ctx) {
    log_reserve_stdout();
    // A line-oriented protocol, so the far end can be anything that
    // can write a line -- a Python script, a shell pipeline, a model
    // with a tool. Unbuffered, because a caller waiting on a reply
    // that is sitting in a buffer looks exactly like a hang.
    std::cout << agent_help().to_string() << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        // Blank lines and comments, so a session can be a file.
        size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        std::string error;
        Json cmd = Json::parse(line, &error);
        Json reply;
        if (!error.empty()) {
            reply = fail("bad_json", error);
        } else if (cmd.type() != Json::Type::Object) {
            reply = fail("bad_json", "a command must be a JSON object");
        } else {
            reply = agent_execute(ctx, cmd);
        }
        std::cout << reply.to_string() << std::endl;
        if (reply["quit"].boolean()) break;
    }
    return 0;
}

}  // namespace wr
