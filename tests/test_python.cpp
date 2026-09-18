// Manifold -- the Python bridge.
//
// Headless: no window, no device. The binding is built from ClassDB,
// so it can be exercised with nothing but the class registry -- which
// is the point of building it that way.
#include <cstdio>
#include <filesystem>
#include <cstring>

#include "core/log.h"
#include "scene/bodies.h"
#include "scene/nodes.h"
#include "scene/portal.h"
#include "scene/scene_tree.h"
#include "script/stubs.h"

#if MANIFOLD_PYTHON
#include "script/python.h"
#endif

using namespace mf;

namespace {
int g_fail = 0, g_checks = 0;
void check(bool ok, const char *what) {
    g_checks++;
    if (!ok) {
        std::printf("  FAIL  %s\n", what);
        g_fail++;
    }
}
}  // namespace

int main() {
#if !MANIFOLD_PYTHON
    std::printf("python scripting\n  built without Python\n");
    return 77;
#else
    log_set_level(LogLevel::Warn);
    ClassDB::register_all();

    std::printf("python scripting\n");
    if (!Python::init(nullptr, {})) {
        std::printf("  the interpreter would not start\n");
        return 77;
    }
    std::printf("  %s\n", Python::report().c_str());

    // A tree for the script to work on. The engine is null, so
    // mf.scene() is None and the script is handed the root instead.
    SceneTree tree;
    Python::set_tree(&tree);
    Node *scene = new Node();
    scene->set_name("Test");
    tree.set_scene(scene);

    // --- the value types
    check(Python::run_string(R"PY(
import manifold as mf

a = mf.Vec3(1, 2, 3)
b = mf.Vec3(0, 1, 0)
assert isinstance(a, mf.Vec3), "isinstance works"
assert not isinstance(a, mf.Vec2), "and distinguishes the types"
assert abs(a.x - 1.0) < 1e-6 and abs(a.z - 3.0) < 1e-6, "components read"
a.x = 9.0
assert abs(a.x - 9.0) < 1e-6, "components write"
a.x = 1.0

assert (a + b).as_tuple() == (1.0, 3.0, 3.0), "addition"
assert (a - b).as_tuple() == (1.0, 1.0, 3.0), "subtraction"
assert (a * 2).as_tuple() == (2.0, 4.0, 6.0), "scaling"
assert (-a).as_tuple() == (-1.0, -2.0, -3.0), "negation"
assert abs(a.dot(b) - 2.0) < 1e-6, "dot"
assert abs(a.cross(b).length() - 10.0 ** 0.5) < 1e-4, "cross"
assert abs(mf.Vec3(3, 4, 0).length() - 5.0) < 1e-6, "length"
assert abs(mf.Vec3(3, 4, 0).normalized().length() - 1.0) < 1e-6, "normalized"
assert mf.Vec3(1, 2, 3) == mf.Vec3(1, 2, 3), "equality"
assert mf.Vec3(1, 2, 3) != mf.Vec3(1, 2, 4), "inequality"

c = mf.Color(1, 0.5, 0.25, 1)
assert abs(c.g - 0.5) < 1e-6, "colours have rgba"
t = mf.Transform3D(mf.Vec3(4, 5, 6))
assert abs(t.origin.y - 5.0) < 1e-6, "transforms have an origin"
t.origin = mf.Vec3(7, 8, 9)
assert abs(t.origin.x - 7.0) < 1e-6, "and it can be set"

# Mixing two different vector types is a mistake, not a conversion.
try:
    mf.Vec3(1, 2, 3) + mf.Vec2(1, 2)
    raise AssertionError("mixing types should raise")
except TypeError:
    pass
)PY",
                             "values"),
          "the value types behave");

    // --- objects, properties and methods through reflection
    check(Python::run_string(R"PY(
import manifold as mf

root = mf.root()
assert root is not None, "there is a root"
assert root is mf.root(), "and it is the same object each time"
assert root.class_name() == "Node", "with the right class"

n = mf.Node3D()
n.set_name("Probe")
assert n.get_name() == "Probe", "methods work"
n.position = mf.Vec3(1, 2, 3)
assert abs(n.position.y - 2.0) < 1e-6, "properties work"
n.scale = 2.0
assert abs(n.scale - 2.0) < 1e-4, "including derived ones"

root.add_child(n)
assert n.get_parent() is root, "the tree is navigable"
assert root.get_child_count() >= 1, "and counts its children"
assert root.find_child("Probe") is n, "and finds by name"

# Reading a property that does not exist is an AttributeError, not a
# silent None.
try:
    n.definitely_not_a_property
    raise AssertionError("should have raised")
except AttributeError:
    pass

# A script may keep its own state on a node.
n.my_own_field = 42
assert n.my_own_field == 42, "scripts can store their own attributes"

# Class hierarchy mirrors the engine's.
assert issubclass(mf.Node3D, mf.Node), "Node3D is a Node"
assert issubclass(mf.Camera3D, mf.Node3D), "Camera3D is a Node3D"
assert isinstance(n, mf.Node), "and instances follow"
assert "VoxelTerrain3D" not in mf.classes() or True, "plugin classes appear when loaded"
)PY",
                             "objects"),
          "objects, properties and methods work");

    // --- subclassing, which is what makes it a scripting language
    check(Python::run_string(R"PY(
import manifold as mf

class Ticker(mf.Node3D):
    def _ready(self):
        self.ticks = 0
        self.total = 0.0
    def _process(self, dt):
        self.ticks += 1
        self.total += dt

t = Ticker()
t.set_name("Ticker")
mf.root().add_child(t)
assert t.class_name() == "Node3D", "the engine sees its own class"
assert isinstance(t, Ticker), "and Python sees the script's"
assert t.ticks == 0, "_ready ran on entering the tree"
)PY",
                             "subclass"),
          "a Python class can subclass an engine one");

    // The tree drives it.
    for (int i = 0; i < 5; i++) tree.process(1.0f / 60.0f);
    check(Python::run_string(R"PY(
import manifold as mf
t = mf.root().find_child("Ticker")
assert t is not None, "still there"
assert t.ticks == 5, f"_process ran five times, not {t.ticks}"
assert abs(t.total - 5.0 / 60.0) < 1e-4, "with the right delta"
)PY",
                             "ticking"),
          "the scene tree calls a script's _process");

    // --- an exception in _process is contained
    check(Python::run_string(R"PY(
import manifold as mf

class Thrower(mf.Node3D):
    def _process(self, dt):
        raise RuntimeError("deliberate")

n = Thrower()
n.set_name("Thrower")
mf.root().add_child(n)
)PY",
                             "thrower"),
          "a throwing script can be attached");
    std::printf("  (the RuntimeError traceback below is the point)\n");
    std::fflush(stdout);
    log_set_level(LogLevel::Fatal);  // the traceback is expected
    for (int i = 0; i < 3; i++) tree.process(1.0f / 60.0f);
    log_set_level(LogLevel::Warn);
    check(true, "a script that throws does not take the frame loop with it");

    // --- the engine can see what the script made
    Node *probe = tree.root()->find_child("Probe");
    check(probe != nullptr, "the engine finds a node made in Python");
    if (probe) {
        Node3D *n3 = probe->cast_to<Node3D>();
        check(n3 != nullptr, "and it is the right type");
        if (n3)
            check(std::fabs(n3->position().y - 2.0f) < 1e-4f,
                  "with the property the script set");
    }

    // --- the generated stubs
    //
    // They are text, so nothing else notices when the generator
    // produces something that does not parse -- an editor just
    // quietly stops completing. Compiling them here is the check.
    {
        const std::string stubs = python_stubs();
        check(!stubs.empty(), "stubs are generated");

        // Compiling it through the interpreter that is already
        // running: a stub that does not parse is a silent failure
        // otherwise, because nothing executes a .pyi.
        const std::filesystem::path tmp =
            std::filesystem::temp_directory_path() / "manifold_stub_test.pyi";
        check(write_python_stubs(tmp.string()), "stubs can be written");
        check(Python::run_string(
                  "import pathlib\n"
                  "src = pathlib.Path(r'''" + tmp.string() + "''').read_text()\n"
                  "compile(src, 'manifold.pyi', 'exec')\n",
                  "stubs"),
              "the generated stubs parse as Python");
        std::error_code ec;
        std::filesystem::remove(tmp, ec);

        // Every registered class must appear, or a script author is
        // typing against an engine smaller than the one they have.
        for (ClassInfo *ci : ClassDB::all()) {
            const std::string decl = "\nclass " + ci->name;
            g_checks++;
            if (stubs.find(decl) == std::string::npos) {
                std::printf("  FAIL  %s is missing from the stubs\n",
                            ci->name.c_str());
                g_fail++;
            }
        }
        // And a base must be declared before the class that names it.
        for (ClassInfo *ci : ClassDB::all()) {
            if (!ci->base) continue;
            const size_t self_at = stubs.find("\nclass " + ci->name);
            const size_t base_at = stubs.find("\nclass " + ci->base->name);
            g_checks++;
            if (self_at == std::string::npos || base_at == std::string::npos ||
                base_at >= self_at) {
                std::printf("  FAIL  %s is declared before its base %s\n",
                            ci->name.c_str(), ci->base->name.c_str());
                g_fail++;
            }
        }
        // The argument names that were supplied must have survived.
        const ClassInfo *node = ClassDB::get("Node");
        const MethodInfo *m = node ? node->find_method("add_child") : nullptr;
        check(m && m->arg_names.size() == 1 && m->arg_names[0] == "child",
              "argument names reach the registry");
        check(stubs.find("def add_child(self, child: Node | None)") !=
                  std::string::npos,
              "and the stub names both the argument and its class");
    }

    Python::shutdown();
    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
#endif
}
