"""ROTGRAVE -- what a shot looks like.

A gun with no muzzle flash and no tracer is a gun that makes a
noise and changes a number somewhere. The first time this was
played the only sign a shot had landed was the target
disappearing, which is the same feedback a bug would give.

Three things, all cheap, all pooled:

  the flash, a bright card at the muzzle for two frames;
  the tracer, a thin bright line from the muzzle to wherever the
  round stopped, for about a tenth of a second;
  and the corpse, which is the one that matters most -- a body
  that vanishes on the last hit gives no confirmation, and a body
  that falls over does.

Pooled because a firefight is ten of each a second and a node made
and freed per shot is churn for nothing.
"""

import math
import random

import warren as wr


def _unlit(colour):
    m = wr.Material()
    m.albedo = colour
    m.emissive = colour
    m.emissive_strength = 6.0
    m.unlit = True
    m.cast_shadows = False
    return m


class Effects:
    FLASHES = 6
    TRACERS = 10
    FLASH_TIME = 0.035
    TRACER_TIME = 0.06

    def __init__(self, parent, rng=None):
        self.rng = rng or random.Random(11)
        self.root = wr.Node3D()
        self.root.name = "Effects"
        parent.add_child(self.root)

        flash_mesh = wr.MeshBuilder()
        flash_mesh.colour = wr.Color(1.0, 0.86, 0.55, 1.0)
        flash_mesh.add_box(wr.Vec3(), wr.Vec3(0.34, 0.34, 0.34))
        flash_mesh.add_box(wr.Vec3(0, 0, -0.22), wr.Vec3(0.13, 0.13, 0.5))
        fm = flash_mesh.build()
        self.flash_mat = _unlit(wr.Color(1.0, 0.85, 0.5, 1.0))

        tracer_mesh = wr.MeshBuilder()
        tracer_mesh.add_box(wr.Vec3(0, 0, -0.5), wr.Vec3(0.035, 0.035, 1.0))
        tm = tracer_mesh.build()
        self.tracer_mat = _unlit(wr.Color(1.0, 0.78, 0.42, 1.0))

        self._flashes = self._pool("flash", self.FLASHES, fm, self.flash_mat)
        self._tracers = self._pool("tracer", self.TRACERS, tm,
                                   self.tracer_mat)
        self._nf = self._nt = 0
        self._live = []       # (node, until, kind)
        self._corpses = []    # (node, until)

    def _pool(self, name, n, mesh, mat):
        out = []
        for i in range(n):
            mi = wr.MeshInstance3D()
            mi.name = f"{name}{i}"
            mi.mesh = mesh
            mi.set_material(0, mat)
            mi.cast_shadows = False
            mi.visible = False
            self.root.add_child(mi)
            out.append(mi)
        return out

    # ------------------------------------------------------- shots

    def shot(self, now, muzzle, direction, hit_at=None):
        """A flash at the muzzle and a line to where it stopped."""
        f = self._flashes[self._nf]
        self._nf = (self._nf + 1) % len(self._flashes)
        f.position = muzzle
        f.euler = wr.Vec3(math.atan2(-direction.x, -direction.z),
                          math.asin(max(-1.0, min(1.0, direction.y))),
                          self.rng.uniform(0.0, 3.14))
        f.scale = self.rng.uniform(0.8, 1.25)
        f.visible = True
        self._live.append((f, now + self.FLASH_TIME))

        if hit_at is None:
            return
        d = hit_at - muzzle
        length = d.length()
        if length < 0.5:
            return
        t = self._tracers[self._nt]
        self._nt = (self._nt + 1) % len(self._tracers)
        t.position = muzzle
        t.euler = wr.Vec3(math.atan2(-d.x, -d.z),
                          math.asin(max(-1.0, min(1.0, d.y / length))), 0.0)
        # The tracer mesh is one metre long down -Z, so the scale is
        # the distance. Uniform scale only, which is why it is thin
        # to begin with rather than scaled thin here.
        t.scale = length
        t.visible = True
        self._live.append((t, now + self.TRACER_TIME))

    # ------------------------------------------------------ corpses

    def corpse(self, now, node, at, facing, seconds=6.0):
        """Lay a body down where it fell.

        Re-parented out of the agent that is about to be freed, and
        tipped over. A kill that simply deletes the body leaves the
        player unsure whether they hit it, and leaves the street
        looking untouched after thirty of them.
        """
        node.get_parent().remove_child(node)
        self.root.add_child(node)
        node.position = at
        node.euler = wr.Vec3(facing, -1.4, self.rng.uniform(-0.3, 0.3))
        self._corpses.append((node, now + seconds))
        return node

    def update(self, now):
        if self._live:
            still = []
            for node, until in self._live:
                if now >= until:
                    node.visible = False
                else:
                    still.append((node, until))
            self._live = still
        if self._corpses:
            still = []
            for node, until in self._corpses:
                if now >= until:
                    node.queue_free()
                else:
                    still.append((node, until))
            self._corpses = still
