# Manifold -- a script, attached to a node.
#
# Run with:  manifold --demo portals --script scripts/spinner.py
#
# Everything here goes through the engine's own reflection: the
# classes, their properties and their methods all come from ClassDB,
# so a class a plugin registered is available on the same terms as a
# built-in one and nothing had to be bound by hand.
import math
import manifold as mf


class Spinner(mf.Node3D):
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
        self.rotate(mf.Vec3(0, 1, 0), self.speed * dt)
        self.position = self.base + mf.Vec3(0, math.sin(self.t * 2.0) * self.bob, 0)


def main():
    scene = mf.scene()
    if scene is None:
        mf.error("no scene")
        return

    mf.log(f"manifold {mf.version}, {len(mf.classes())} classes")

    # The maths types behave like maths.
    a = mf.Vec3(1, 2, 3)
    b = mf.Vec3(0, 1, 0)
    assert abs((a + b).y - 3.0) < 1e-6
    assert abs(a.dot(b) - 2.0) < 1e-6
    assert abs(a.cross(b).length() - math.sqrt(10)) < 1e-4
    assert isinstance(a, mf.Vec3)

    # Engine objects are reachable and their properties are attributes.
    cam = mf.camera()
    if cam is not None:
        mf.log(f"camera at {cam.global_position}, fov {cam.fov:.0f}")

    # Make something and put it in the world.
    for i in range(3):
        node = Spinner()
        node.set_name(f"Spinner{i}")
        node.position = mf.Vec3(-2.0 + i * 2.0, 1.2, -1.0)

        mesh = mf.instantiate("MeshInstance3D")
        mesh.set_name("mesh")
        node.add_child(mesh)
        scene.add_child(node)

    mf.log(f"scene now:\n{scene.print_tree(0)}")


main()
