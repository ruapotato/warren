// Warren -- the interface for something that is not a person.
//
// The editor is for a human: it draws the scene tree, it has an
// inspector, you click a thing and drag a slider. This is the same
// engine with the same reflection underneath, addressed the way a
// program addresses it -- one JSON object in, one JSON object out.
//
// The bet is that these are not two features. Everything the
// inspector can show, ClassDB already knows; everything an agent
// needs to know about this engine, ClassDB already knows. So the
// agent interface is not a layer written alongside the engine that
// will drift out of date with it. It is a fifth reader of the same
// table that feeds the Python bindings, the stubs, the inspector, the
// scene files and the replication -- which means a property added to
// a node this afternoon is promptable this afternoon, with no
// documentation written and nothing regenerated.
//
// Three things make it usable by a model rather than merely by a
// program:
//
//   `schema` dumps the whole API as JSON. An agent reads it once and
//   knows every class, every property with its type and range, every
//   method with its argument NAMES. No documentation to go stale, and
//   nothing to guess.
//
//   Errors answer the question that was asked. A wrong property name
//   comes back with the near misses; a wrong class comes back with
//   the classes that do have the property you tried to set. A model
//   that guesses gets told the right answer instead of "failed", and
//   is right on the second try instead of the fifth.
//
//   Liberal parsing. A vector may be written {"x":1,"y":2,"z":3} or
//   [1,2,3]; a transform may be written as position/rotation/scale
//   even though that is not how one is stored. Being strict here buys
//   nothing and costs a retry.
#pragma once

#include <string>

#include "core/json.h"

namespace wr {

class Engine;
class SceneTree;

// WHAT THE COMMANDS ARE ALLOWED TO TOUCH.
//
// A running engine is not required to answer most of this. Reading a
// scene, creating nodes, setting properties and saving the result
// need a tree and nothing else -- only stepping, screenshots and
// frame statistics need frames to have happened. Keeping the two
// apart means a build tool, a test or a headless asset pipeline can
// use the same commands without opening a window, and the commands
// that genuinely need one say so instead of crashing.
struct AgentContext {
    Engine *engine = nullptr;
    SceneTree *tree = nullptr;  // defaults to the engine's

    AgentContext() = default;
    AgentContext(Engine *e) : engine(e) {}
    AgentContext(SceneTree *t) : tree(t) {}
    SceneTree *scene_tree() const;
};

// The whole engine API as one JSON document. `class_name` empty is
// everything; naming a class gives that class alone. `inherited`
// folds in the base classes' members rather than only naming the
// base, which is what an agent wants when it is about to set a
// property and does not care where it was declared.
Json agent_schema(const std::string &class_name = "", bool inherited = false);

// One command. Never throws; a failure is a reply with "ok": false,
// because the caller is a program and an exception is not an answer.
Json agent_execute(const AgentContext &ctx, const Json &command);

// Read newline-delimited JSON commands from stdin and write
// newline-delimited replies to stdout, stepping the engine between
// them. Returns the process exit code.
int agent_serve(const AgentContext &ctx);

// Every command, its arguments and what it returns -- generated from
// the same table the dispatcher uses, so it cannot describe a command
// that is not there.
Json agent_help();

}  // namespace wr
