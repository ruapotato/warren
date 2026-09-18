// Warren -- driving the engine from a program.
//
// The agent interface exists so that something that is not a person
// can use this engine: a script, a build step, a model. What makes
// that workable is not the command list, which is short and obvious.
// It is the two properties measured here.
//
// The FIRST is that the schema is complete. An agent reads it once
// and has no other source of truth: if a property is settable but
// missing from the schema, nothing will ever set it, and no error
// will ever be raised. So the test checks the schema against the
// registry it was generated from rather than against a list written
// by hand, because a list written by hand is the thing being
// replaced.
//
// The SECOND is that a wrong guess is answered rather than refused.
// A model that misspells a property should get the spelling back. It
// is measured the way it will be met: with the mistakes a caller
// actually makes -- British spellings, the Godot name for the same
// thing, the right name on the wrong node.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "agent/agent.h"
#include "core/json.h"
#include "procgen/sdf.h"
#include "procgen/texture.h"
#include "core/log.h"
#include "resource/resource.h"
#include "scene/controls.h"
#include "scene/nodes.h"
#include "scene/scene_tree.h"

using namespace wr;

namespace {

int g_fail = 0, g_checks = 0;
void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        g_fail++;
    }
}

// The reply to one command, so a test reads like a session.
Json run(const AgentContext &ctx, const std::string &line) {
    std::string error;
    Json cmd = Json::parse(line, &error);
    if (!error.empty()) {
        std::printf("  FAIL  the test's own JSON is bad: %s\n", error.c_str());
        g_fail++;
        return Json();
    }
    return agent_execute(ctx, cmd);
}

bool said(const Json &reply, const char *key, const char *value) {
    const Json &v = reply[key];
    if (v.type() == Json::Type::String) return v.string() == value;
    for (size_t i = 0; i < v.size(); i++)
        if (v[i].type() == Json::Type::String && v[i].string() == value) return true;
    return false;
}

}  // namespace

int main() {
    log_set_level(LogLevel::Error);
    ClassDB::register_all();
    std::printf("agent\n");

    // ------------------------------------------------- the schema
    //
    // Generated with no engine at all, which is the point: a tool
    // that wants to know what this engine can do should not have to
    // open a window to find out.
    {
        Json schema = agent_schema();
        const Json &classes = schema["classes"];
        check(classes.size() == ClassDB::all().size(),
              "the schema names every registered class");

        // Every property and method of every class, compared against
        // the registry. This is the check that keeps the schema
        // honest as classes are added.
        size_t props = 0, methods = 0, missing = 0, untyped = 0;
        for (size_t i = 0; i < classes.size(); i++) {
            const Json &c = classes[i];
            ClassInfo *ci = ClassDB::get(c["name"].string());
            if (!ci) { missing++; continue; }
            props += c["properties"].size();
            methods += c["methods"].size();
            if (c["properties"].size() != ci->properties.size()) missing++;
            if (c["methods"].size() != ci->methods.size()) missing++;
            for (size_t k = 0; k < c["properties"].size(); k++) {
                const Json &p = c["properties"][k];
                if (p["type"].string().empty()) untyped++;
                if (!ci->find_property(p["name"].string())) missing++;
            }
            for (size_t k = 0; k < c["methods"].size(); k++) {
                const Json &m = c["methods"][k];
                if (m["returns"].string().empty()) untyped++;
                if (!ci->find_method(m["name"].string())) missing++;
                // An argument with no name is one a caller has to
                // guess at, which is the failure this whole interface
                // is trying to avoid.
                for (size_t a = 0; a < m["args"].size(); a++)
                    if (m["args"][a]["name"].string().empty()) untyped++;
            }
        }
        check(missing == 0, "every class's members match the registry exactly");
        check(untyped == 0, "and nothing is left without a type or a name");
        std::printf("  schema: %zu classes, %zu properties, %zu methods\n",
                    classes.size(), props, methods);

        // A range in the hint has to come out as numbers, or a caller
        // has to parse English to know what is legal.
        Json light = agent_schema("AudioPlayer", false);
        bool ranged = false;
        for (size_t i = 0; i < light["properties"].size(); i++) {
            const Json &p = light["properties"][i];
            if (p["name"].string() == "volume")
                ranged = p.has("min") && p.has("max") && p["max"].number() == 4.0;
        }
        check(ranged, "a range hint is reported as min and max");

        // The round trip a caller will actually do: read the schema
        // as text, parse it back, get the same thing.
        std::string text = agent_schema("Node3D", true).to_string(2);
        std::string error;
        Json again = Json::parse(text, &error);
        check(error.empty() && again["name"].string() == "Node3D",
              "the schema survives being written and read back");
        // Inherited folds in the base, which is what someone about to
        // set a property wants -- they do not care where it was declared.
        check(agent_schema("Node3D", true)["properties"].size() >
                  agent_schema("Node3D", false)["properties"].size(),
              "inherited members are folded in when asked for");
    }

    // ---------------------------------------- a tree, and no engine
    SceneTree tree;
    Node *root = tree.root();
    AgentContext ctx(&tree);

    {
        Json r = run(ctx, R"({"cmd":"step"})");
        check(!r["ok"].boolean() && r["code"].string() == "no_engine",
              "a command that needs frames says so rather than crashing");
        r = run(ctx, R"({"cmd":"tree"})");
        check(r["ok"].boolean(), "and one that does not, works");
    }

    // ------------------------------------------------- building
    {
        Json r = run(ctx, R"({"cmd":"create","class":"Node3D","parent":"/","name":"World"})");
        check(r["ok"].boolean() && r["node"]["path"].string() == "/root/World",
              "a node is created and reported by its path");

        r = run(ctx, R"({"cmd":"create","class":"OmniLight3D","parent":"/root/World",
                         "name":"Lamp","properties":{"position":{"x":1,"y":2,"z":3},
                         "colour":"#ff8000","energy":25,"range":9}})");
        check(r["ok"].boolean(), "and created with its properties in one command");

        r = run(ctx, R"({"cmd":"get","path":"/root/World/Lamp","property":"position"})");
        check(r["ok"].boolean() && r["value"]["y"].number() == 2.0,
              "the properties took effect");
        check(r["type"].string() == "vec3", "and the reply says what type it is");

        // THE THREE SPELLINGS OF A VECTOR. All of them are what
        // somebody will send, and refusing two of them buys nothing.
        run(ctx, R"({"cmd":"set","path":"/root/World/Lamp","property":"position","value":[4,5,6]})");
        r = run(ctx, R"({"cmd":"get","path":"/root/World/Lamp","property":"position"})");
        check(r["value"]["x"].number() == 4.0, "a vector may be written as an array");
        run(ctx, R"({"cmd":"set","path":"/root/World/Lamp","property":"position","value":{"y":9}})");
        r = run(ctx, R"({"cmd":"get","path":"/root/World/Lamp","property":"position"})");
        check(r["value"]["y"].number() == 9.0 && r["value"]["x"].number() == 4.0,
              "and a partial object moves only the components it names");

        // A transform written the way a person describes one.
        r = run(ctx, R"({"cmd":"set","path":"/root/World/Lamp","property":"transform",
                         "value":{"position":[0,1,0],"rotation":{"y":90},"scale":2}})");
        check(r["ok"].boolean(), "a transform takes position, rotation and scale");
        Json named = run(ctx, R"({"cmd":"set","path":"/root/World/Lamp","property":"transform",
                                  "value":{"rotation":{"yaw":90}}})");
        check(named["value"]["rotation_degrees"]["yaw"].number() > 89.9,
              "a rotation may also be written with its axes named");
        r = run(ctx, R"({"cmd":"set","path":"/root/World/Lamp","property":"transform",
                         "value":{"position":[0,1,0],"rotation":{"y":90},"scale":2}})");
        // Named axes on the way out, because the engine's own euler
        // packing puts yaw in .x and a caller reading x/y/z back
        // would write a pitch next time.
        check(r["value"]["rotation_degrees"]["yaw"].number() > 89.9 &&
                  r["value"]["rotation_degrees"]["yaw"].number() < 90.1,
              "and reads back as the rotation that was asked for");
        check(r["value"]["rotation_degrees"]["pitch"].number() < 0.1,
              "with the axes named rather than packed into x, y and z");
        check(r["value"]["scale"].number() > 1.99 && r["value"]["scale"].number() < 2.01,
              "with the scale that was asked for");

        // A colour is linear inside and sRGB in the file and on the
        // wire, so "#ff8000" must not come back as 1.0, 0.5, 0.
        r = run(ctx, R"({"cmd":"get","path":"/root/World/Lamp","property":"colour"})");
        const double g = r["value"]["g"].number();
        check(g > 0.2 && g < 0.25, "a hex colour is decoded from sRGB, not taken raw");
    }

    // ------------------------------------------- calling and finding
    {
        run(ctx, R"({"cmd":"create","class":"Node3D","parent":"/root/World","name":"Pillar"})");
        Json r = run(ctx, R"({"cmd":"call","path":"/root/World/Pillar",
                              "method":"add_to_group","args":["scenery"]})");
        check(r["ok"].boolean(), "a method is called with positional arguments");

        // By NAME, which is the form a caller reading the schema will
        // use, and which only works because reflection kept them.
        r = run(ctx, R"({"cmd":"call","path":"/root/World/Pillar",
                         "method":"add_to_group","args":{"group":"solid"}})");
        check(r["ok"].boolean(), "and with arguments keyed by name");

        r = run(ctx, R"({"cmd":"find","group":"scenery"})");
        check(r["ok"].boolean() && r["count"].number() == 1.0, "a group can be searched");
        r = run(ctx, R"({"cmd":"find","class":"Light3D"})");
        check(r["count"].number() == 1.0, "a search by class matches subclasses");
        r = run(ctx, R"({"cmd":"find","name":"ill"})");
        check(r["count"].number() == 1.0, "and a search by name is a substring");
    }

    // ------------------------------------- moving things about
    {
        run(ctx, R"({"cmd":"create","class":"Node3D","parent":"/root","name":"Elsewhere",
                     "properties":{"position":[100,0,0]}})");
        run(ctx, R"({"cmd":"set","path":"/root/World","property":"position","value":[10,0,0]})");
        Json r = run(ctx, R"({"cmd":"get","path":"/root/World/Pillar","property":"global_position"})");
        check(r["value"]["x"].number() == 10.0, "a child moves with its parent");

        r = run(ctx, R"({"cmd":"reparent","path":"/root/World/Pillar","to":"/root/Elsewhere"})");
        check(r["ok"].boolean(), "a node can be reparented");
        r = run(ctx, R"({"cmd":"get","path":"/root/Elsewhere/Pillar","property":"global_position"})");
        check(r["value"]["x"].number() > 9.99 && r["value"]["x"].number() < 10.01,
              "and does not move in the world when it is");

        r = run(ctx, R"({"cmd":"reparent","path":"/root/Elsewhere","to":"/root/Elsewhere/Pillar"})");
        check(!r["ok"].boolean(), "a node cannot be reparented under itself");
        r = run(ctx, R"({"cmd":"delete","path":"/root/Elsewhere/Pillar"})");
        check(r["ok"].boolean() && !root->find_path("Elsewhere/Pillar"),
              "and deleting one really removes it");
    }

    // ---------------------------------- WHAT A WRONG GUESS GETS BACK
    //
    // Each of these is a mistake somebody will make on the way in.
    // The measure is not that they fail -- they must -- but that the
    // reply contains the right answer.
    {
        Json r = run(ctx, R"({"cmd":"get","path":"/root/World/Lamp","property":"colour"})");
        check(r["ok"].boolean(), "colour is spelled the engine's way");

        // The American spelling, which is the one Godot uses.
        r = run(ctx, R"({"cmd":"set","path":"/root/World/Lamp","property":"color","value":"#ffffff"})");
        check(!r["ok"].boolean() && said(r, "did_you_mean", "colour"),
              "the other spelling of colour is suggested");

        r = run(ctx, R"({"cmd":"set","path":"/root/World/Lamp","property":"enrgy","value":1})");
        check(!r["ok"].boolean() && said(r, "did_you_mean", "energy"),
              "a typo in a property name is suggested");

        // Godot spells this as a method; here it is a property, and
        // an agent working from habit needs telling which.
        r = run(ctx, R"({"cmd":"call","path":"/root/World/Lamp","method":"set_position","args":[[0,0,0]]})");
        check(!r["ok"].boolean() && said(r, "but_there_is_a_property", "position"),
              "a Godot-style setter is answered with the property it should be");
        r = run(ctx, R"({"cmd":"call","path":"/root/World/Lamp","method":"add_to_grp","args":["x"]})");
        check(!r["ok"].boolean() && said(r, "did_you_mean", "add_to_group"),
              "and a genuine method typo is suggested");

        r = run(ctx, R"({"cmd":"create","class":"OmniLight","parent":"/"})");
        check(!r["ok"].boolean() && said(r, "did_you_mean", "OmniLight3D"),
              "a class name missing its suffix is suggested");

        // The right name on the wrong node -- the mistake that a
        // "did you mean" cannot help with, because the name is
        // spelled correctly. Saying where it DOES exist can.
        r = run(ctx, R"({"cmd":"set","path":"/root/World","property":"energy","value":3})");
        check(!r["ok"].boolean() && said(r, "property_exists_on", "OmniLight3D"),
              "a property set on the wrong class says which class has it");

        // The right name in the wrong place.
        r = run(ctx, R"({"cmd":"get","path":"/root/Lamp","property":"energy"})");
        check(!r["ok"].boolean() && said(r, "did_you_mean", "/root/World/Lamp"),
              "a wrong path is answered with the right one");

        r = run(ctx, R"({"cmd":"creat","class":"Node"})");
        check(!r["ok"].boolean() && said(r, "did_you_mean", "create"),
              "and a misspelled command is suggested too");

        // Wrong shapes, which must say what the right shape is.
        r = run(ctx, R"({"cmd":"set","path":"/root/World/Lamp","property":"energy","value":"bright"})");
        check(!r["ok"].boolean() && r["expected"].string() == "float",
              "a value of the wrong type says which type was wanted");
        r = run(ctx, R"({"cmd":"create","class":"Node3D","parent":"/root/World",
                         "name":"Broken","properties":{"position":[1,2],"visible":false}})");
        check(!r["ok"].boolean() && !root->find_path("World/Broken"),
              "a node whose properties do not apply is not left half-built");
        r = run(ctx, R"({"cmd":"create","class":"Light3D","parent":"/"})");
        check(!r["ok"].boolean() && r["code"].string() == "abstract",
              "an abstract class is refused with the reason");
    }

    // -------------------------------------- a batch that half applies
    {
        run(ctx, R"({"cmd":"set","path":"/root/World/Lamp","property":"energy","value":1})");
        Json r = run(ctx, R"({"cmd":"set","path":"/root/World/Lamp","properties":
                              {"energy":7,"rnge":3}})");
        check(!r["ok"].boolean() && r["failed_on"].string() == "rnge",
              "a batched set names the property that failed");
        // The ones before it DID apply, and the reply says which, so
        // the caller knows the state rather than having to re-read it.
        check(r["applied"]["energy"].number() == 7.0,
              "and reports what had already been applied");
    }

    // -------------------------------------------- saving and loading
    {
        std::error_code ec;
        const std::string dir = "warren_agent_test";
        std::filesystem::create_directories(dir, ec);
        ResourceLoader::set_base_directory(dir);

        Json r = run(ctx, R"({"cmd":"save_scene","path":"built.mfs","node":"/root/World"})");
        check(r["ok"].boolean(), "a subtree the agent built can be saved");
        r = run(ctx, R"({"cmd":"load_scene","path":"built.mfs","parent":"/root"})");
        check(r["ok"].boolean(), "and loaded back");
        Node *back = root->find_path(r["node"]["name"].string());
        check(back && back->find_child("Lamp"), "with its children");
        if (back) {
            OmniLight3D *lamp = back->find_child("Lamp")
                                    ? back->find_child("Lamp")->cast_to<OmniLight3D>()
                                    : nullptr;
            check(lamp && lamp->energy == 7.0f,
                  "and the property values the agent set");
        }

        ResourceLoader::forget_all();
        std::filesystem::remove_all(dir, ec);
        ResourceLoader::set_base_directory(".");
    }

    // ------------------------------------------ the protocol itself
    {
        Json r = run(ctx, R"({"cmd":"tree","id":"abc"})");
        check(r["id"].string() == "abc", "an id is echoed so replies can be matched");
        r = run(ctx, R"({"nope":1})");
        check(!r["ok"].boolean() && r["commands"].size() > 0,
              "a command with no cmd is answered with the list of commands");

        // help is generated from the dispatch table, so it cannot
        // describe a command that is not there or miss one that is.
        Json help = agent_help();
        size_t unknown = 0;
        for (size_t i = 0; i < help["commands"].size(); i++) {
            Json probe = Json::object();
            probe.set("cmd", help["commands"][i]["cmd"].string());
            Json reply = agent_execute(ctx, probe);
            if (reply["code"].string() == "no_such_command") unknown++;
        }
        check(unknown == 0, "every command help lists is a command that exists");
    }

    // ------------------------------------ THE DOCUMENTATION
    //
    // Every example in docs/agent.md, run against the engine.
    //
    // Worth doing because the whole argument for this interface is
    // that it cannot drift from the engine -- and a reference that
    // says a class has a property it does not have is exactly the
    // drift the design was supposed to make impossible. It caught
    // one on the first run: an example showing `energy` and
    // `colour` on OmniLight3D, where they are declared on Light3D
    // and a non-inherited dump does not list them.
    //
    // A block containing an arrow is illustrating a REPLY and is
    // skipped; everything else is a command and must work.
    {
#ifdef WARREN_SOURCE_DIR
        size_t ran = 0, broken = 0;
        // The README makes the same claims in shorter form, and has
        // the same way of going stale.
        for (const char *doc : {"/docs/agent.md", "/README.md"}) {
            const std::string path = std::string(WARREN_SOURCE_DIR) + doc;
            FILE *f = std::fopen(path.c_str(), "rb");
            check(f != nullptr, "the documentation is where the test expects it");
            if (!f) continue;
            std::string text;
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
            std::fclose(f);

            size_t at = 0;
            while ((at = text.find("```json", at)) != std::string::npos) {
                const size_t start = text.find('\n', at);
                const size_t end = text.find("```", start);
                if (start == std::string::npos || end == std::string::npos) break;
                const std::string block = text.substr(start + 1, end - start - 1);
                at = end + 3;
                // A reply, not a command.
                if (block.find("\xe2\x86\x92") != std::string::npos) continue;

                // One JSON value per block, possibly several in a row,
                // possibly spread over several lines.
                size_t pos = 0;
                while (pos < block.size()) {
                    const size_t open = block.find('{', pos);
                    if (open == std::string::npos) break;
                    int depth = 0;
                    size_t close = open;
                    bool in_string = false;
                    for (; close < block.size(); close++) {
                        const char ch = block[close];
                        if (in_string) {
                            if (ch == '\\') close++;
                            else if (ch == '"') in_string = false;
                            continue;
                        }
                        if (ch == '"') in_string = true;
                        else if (ch == '{') depth++;
                        else if (ch == '}' && --depth == 0) break;
                    }
                    if (close >= block.size()) break;
                    const std::string one = block.substr(open, close - open + 1);
                    pos = close + 1;

                    std::string error;
                    const Json cmd = Json::parse(one, &error);
                    if (!error.empty()) {
                        std::printf("  FAIL  an example in docs/agent.md is not "
                                    "valid JSON: %s\n", error.c_str());
                        broken++;
                        continue;
                    }
                    // Some blocks are a shape or a surface on their
                    // own, shown as an argument rather than a
                    // command. Those are checked by building them.
                    if (!cmd.has("cmd")) {
                        if (cmd.has("shape") || cmd.has("op")) {
                            gen::Sdf::from_json(cmd, &error);
                            if (!error.empty()) {
                                std::printf("  FAIL  a documented shape does not "
                                            "parse: %s\n", error.c_str());
                                broken++;
                            }
                            ran++;
                        } else if (cmd.has("pattern")) {
                            gen::render_height(cmd, 8, &error);
                            if (!error.empty()) {
                                std::printf("  FAIL  a documented surface does not "
                                            "parse: %s\n", error.c_str());
                                broken++;
                            }
                            ran++;
                        }
                        continue;
                    }
                    const Json reply = agent_execute(ctx, cmd);
                    ran++;
                    // Commands that need frames, and ones whose
                    // paths belong to a demo scene this test does
                    // not have, are not failures of the document.
                    const std::string code = reply["code"].string();
                    if (reply["ok"].boolean() || code == "no_engine" ||
                        code == "no_such_node")
                        continue;
                    std::printf("  FAIL  a documented command fails: %s\n  ->  %s\n",
                                one.c_str(), reply.to_string().c_str());
                    broken++;
                }
            }
        }
        check(broken == 0, "every example in the documentation still works");
        check(ran >= 12, "and the test found them to check");
        std::printf("  checked %zu documented examples\n", ran);
#endif
    }

    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
