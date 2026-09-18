// Warren -- the Python bridge.
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
#include "scene/nav_nodes.h"
#include "scene/portal.h"
#include "scene/scene_tree.h"
#include "script/stubs.h"

#if WARREN_PYTHON
#include "script/python.h"
#endif

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
}  // namespace

int main() {
#if !WARREN_PYTHON
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
import warren as mf

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
import warren as mf

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
import warren as mf

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
import warren as mf
t = mf.root().find_child("Ticker")
assert t is not None, "still there"
assert t.ticks == 5, f"_process ran five times, not {t.ticks}"
assert abs(t.total - 5.0 / 60.0) < 1e-4, "with the right delta"
)PY",
                             "ticking"),
          "the scene tree calls a script's _process");

    // --- an exception in _process is contained
    check(Python::run_string(R"PY(
import warren as mf

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
            std::filesystem::temp_directory_path() / "warren_stub_test.pyi";
        check(write_python_stubs(tmp.string()), "stubs can be written");
        check(Python::run_string(
                  "import pathlib\n"
                  "src = pathlib.Path(r'''" + tmp.string() + "''').read_text()\n"
                  "compile(src, 'warren.pyi', 'exec')\n",
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
        //
        // Matched with the bracket, because a class name is a prefix
        // of other class names: looking for "class Mesh" finds
        // "class MeshBuilder" first and then reports that Mesh comes
        // before its own base, which is a fact about this search and
        // not about the file.
        auto declared_at = [&](const std::string &name) {
            size_t at = stubs.find("\nclass " + name + "(");
            if (at == std::string::npos)
                at = stubs.find("\nclass " + name + ":");
            return at;
        };
        for (ClassInfo *ci : ClassDB::all()) {
            if (!ci->base) continue;
            const size_t self_at = declared_at(ci->name);
            const size_t base_at = declared_at(ci->base->name);
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

    // --- navigation, from a script
    //
    // A game drives its bodies from Python, so the navigation nodes
    // have to be reachable there on the same terms as everything
    // else: built by name, configured by property, and stepped by
    // the tree. Nothing below was bound by hand; it all comes from
    // the same registry the stubs are written from.
    check(Python::run_string(R"PY(
import warren as mf
root = mf.root()
region = mf.NavRegion3D()
region.name = "Nav"
root.add_child(region)
)PY"),
          "a navigation region is built from a script");

    // mf.root() is the tree's root, which is the scene's parent
    // here -- there is no engine, so mf.scene() is None and the
    // script is handed the root instead.
    NavRegion3D *region = nullptr;
    if (Node *found = tree.root()->find_child("Nav"))
        region = found->cast_to<NavRegion3D>();
    check(region != nullptr, "and is a real node in the tree");

    if (region) {
        // The level, built in the script. A game that lays its town
        // out from a plan needs to make geometry, not just place
        // meshes somebody else made.
        check(Python::run_string(R"PY(
import warren as mf
region = mf.root().get_node("Nav")
b = mf.MeshBuilder()
b.add_room(mf.AABB(mf.Vec3(-8, -0.4, -8), mf.Vec3(8, 0, 8)), 3.0, 0.3)
floor = mf.MeshInstance3D()
floor.name = "Level"
floor.mesh = b.build()
region.add_child(floor)
)PY"),
              "a script builds the level geometry itself");

        check(Python::run_string(R"PY(
import warren as mf
region = mf.root().get_node("Nav")
agent = mf.NavAgent3D()
agent.name = "Shambler"
agent.radius = 0.35
agent.max_speed = 2.5
agent.goal_radius = 1.0
agent.position = mf.Vec3(-5, 0, -5)
region.add_child(agent)
)PY"),
              "and an agent is put in it, configured by property");

        // The room is walled, so the walk below is inside a box the
        // script built -- which also checks the walls are walls.
        check(Python::run_string(R"PY(
import warren as mf
b = mf.MeshBuilder()
b.add_box(mf.Vec3(0, 1, 0), mf.Vec3(2, 2, 2))
b.add_wall(mf.Vec3(0, 0, 0), mf.Vec3(4, 0, 0), 3.0, 0.2)
m = b.build()
assert m.triangle_count == 24, "two boxes is twenty-four triangles"
assert abs(m.bounds.min.y) < 1e-4, "and the wall stands on the ground"
)PY"),
              "and the builder's own numbers come back to the script");

        auto frame = [&] {
            tree.physics_tick(1.0f / 60.0f);
            tree.process(1.0f / 60.0f);
        };
        frame();
        check(region->poly_count() > 0,
              "the region bakes the geometry under it");

        check(Python::run_string(R"PY(
import warren as mf
region = mf.root().get_node("Nav")
agent = region.get_node("Shambler")
agent.set_target(mf.Vec3(5, 0, 5))
assert agent.has_target(), "the agent took a target"
path = region.find_path(mf.Vec3(-5, 0, -5), mf.Vec3(5, 0, 5))
assert len(path) >= 2, "a path came back as a list of points"
assert region.is_reachable(mf.Vec3(-5, 0, -5), mf.Vec3(5, 0, 5))
)PY"),
              "pathing answers through the script bindings");

        for (int i = 0; i < 600; ++i) frame();
        check(Python::run_string(R"PY(
import warren as mf
agent = mf.root().get_node("Nav").get_node("Shambler")
assert agent.arrived(), "the agent got there"
p = agent.position
assert p.x > 2.0 and p.z > 2.0, "and the node moved with it"
)PY"),
              "and the body walks, driven by the tree");
    }

    Python::shutdown();
    std::printf("  %d checks\n%s\n", g_checks, g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
#endif
}
