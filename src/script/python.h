// Warren -- Python.
//
// The engine's reflection is the binding. Every class registered with
// ClassDB appears in Python automatically, with its properties as
// attributes and its methods as methods, and a class added by a
// plugin appears the moment that plugin loads. Nothing is written
// twice, and the bindings cannot lag the engine because there are no
// bindings -- there is a bridge.
//
//     import warren as mf
//
//     class Spinner(mf.Node3D):
//         def _ready(self):
//             self.speed = 2.0
//         def _process(self, dt):
//             self.rotate(mf.Vec3(0, 1, 0), self.speed * dt)
//
// THREADS. Python runs on the main thread and nowhere else. A job may
// not touch a script, and a script may not be called from one; the
// engine never does either. That is a deliberate limit rather than an
// oversight: the alternative is holding the GIL across the frame loop
// or a lock around every property access, and neither is worth what
// it buys.
#pragma once

#include <string>
#include <vector>

#include "core/object.h"
#include "scene/node.h"

namespace wr {

class Engine;
class SceneTree;

class Python {
public:
    // Brings up the interpreter and builds the `warren` module from
    // whatever ClassDB currently holds. Safe to call once.
    // `engine` may be null: the bridge needs a scene tree, not an
    // engine, and one can be supplied on its own with set_tree().
    static bool init(Engine *engine, const std::vector<std::string> &search_paths = {});
    static void set_tree(SceneTree *tree);
    static void shutdown();
    static bool running();

    // Re-scan ClassDB and add any classes that have appeared since --
    // after a plugin loads, for instance.
    static void refresh_classes();

    // Run a file or a string. False on an exception, which is printed.
    static bool run_file(const std::string &path);
    static bool run_string(const std::string &source, const char *name = "<string>");

    // Import a module and, if it defines a class with the same name
    // as the file, attach an instance of it to `node` as its script.
    // This is the Godot-shaped path: one file, one class, attached.
    static bool attach_script(Node *node, const std::string &module_or_path);

    // For a console. `ok`, where given, says whether the string is a
    // repr or an exception message -- a console can print both the
    // same way, but a caller that has to act on the answer cannot
    // tell them apart by looking.
    static std::string eval_repr(const std::string &expression, bool *ok = nullptr);

    // Where scripts are looked for. Added to sys.path.
    static void add_search_path(const std::string &path);

    static std::string version();
    static std::string report();
};

}  // namespace wr
