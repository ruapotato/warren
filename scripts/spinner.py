# Warren -- a script, attached to a node.
#
# Run with:  warren --demo portals --script scripts/spinner.py
#
# Everything here goes through the engine's own reflection: the
# classes, their properties and their methods all come from ClassDB,
# so a class a plugin registered is available on the same terms as a
# built-in one and nothing had to be bound by hand.
import math
import warren as wr


class Spinner(wr.Node3D):
    """Turns, and bobs up and down. Subclassing an engine class works:
    the C++ object is made underneath, and _process is called by the
    scene tree."""

    def _ready(self):
        self.speed = 1.4
        self.bob = 0.35
        self.base = self.position
        self.t = 0.0

    def _process(self, dt):
        self.t += dt
        self.rotate(wr.Vec3(0, 1, 0), self.speed * dt)
        self.position = self.base + wr.Vec3(0, math.sin(self.t * 2.0) * self.bob, 0)


def main():
    scene = wr.scene()
    if scene is None:
        wr.error("no scene")
        return

    wr.log(f"warren {wr.version}, {len(wr.classes())} classes")

    # The maths types behave like maths.
    a = wr.Vec3(1, 2, 3)
    b = wr.Vec3(0, 1, 0)
    assert abs((a + b).y - 3.0) < 1e-6
    assert abs(a.dot(b) - 2.0) < 1e-6
    assert abs(a.cross(b).length() - math.sqrt(10)) < 1e-4
    assert isinstance(a, wr.Vec3)

    # Engine objects are reachable and their properties are attributes.
    cam = wr.camera()
    if cam is not None:
        wr.log(f"camera at {cam.global_position}, fov {cam.fov:.0f}")

    # Make something and put it in the world.
    for i in range(3):
        node = Spinner()
        node.set_name(f"Spinner{i}")
        node.position = wr.Vec3(-2.0 + i * 2.0, 1.2, -1.0)

        mesh = wr.instantiate("MeshInstance3D")
        mesh.set_name("mesh")
        node.add_child(mesh)
        scene.add_child(node)

    wr.log(f"scene now:\n{scene.print_tree(0)}")


main()
