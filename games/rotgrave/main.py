"""ROTGRAVE -- run it.

    warren --demo none --script games/rotgrave/main.py

Nothing is built in C++. The engine hands over an empty tree and the
game puts a town in it. The script's own directory is on sys.path
already -- `--script FILE` puts it there, the way every Python
runtime does -- so `import rotgrave` works from wherever it is run.
"""

import os

import warren as wr

from rotgrave.design import load
from rotgrave.town import Town, AREA_OPEN, AREA_SEWER
from rotgrave.player import Player


BINDINGS = {
    "forward": wr.Key.W, "back": wr.Key.S,
    "left": wr.Key.A, "right": wr.Key.D,
    "jump": wr.Key.SPACE, "sprint": wr.Key.LSHIFT,
}


class Game(wr.Node3D):
    def _ready(self):
        for action, key in BINDINGS.items():
            wr.bind_action(action, key)

        self.design = load()
        wr.log(f"ROTGRAVE: {len(self.design.zones)} zones, "
               f"{len(self.design.doors)} doors, "
               f"{len(self.design.spawns)} spawns")

        self.town = Town(self.design, seed=1).build(self)

        start = self.design.zones["crossroads"]
        cx, _, cz = start.centre
        self.player = Player(wr.physics()).spawn(self, wr.Vec3(cx, 1.2, cz))

        # NOVEMBER, FOUR IN THE AFTERNOON. The mode is a dead town
        # and the light has to say so before anything else does.
        # Fog at this density puts the far side of the crossroads in
        # haze and hides the edge of the map, which is worth as much
        # as the mood: a player who can see the perimeter wall knows
        # how small the town is.
        # NOVEMBER, FOUR IN THE AFTERNOON. The mode is a dead town
        # and the light has to say so before anything else does. Fog
        # at this density puts the far side of the crossroads in haze
        # and hides the edge of the map, which is worth as much as
        # the mood: a player who can see the perimeter wall knows how
        # small the town is.
        #
        # Exposure is left alone deliberately -- see
        # docs/known-issues.md, where anything above 1.0 empties the
        # frame in this scene. The brightness is set with the sun and
        # the ambient instead, which is where it belongs anyway.
        env = wr.environment()
        if env:
            env.fog_colour = wr.Color(0.44, 0.46, 0.49, 1.0)
            env.fog_density = 0.0065
            env.ambient = wr.Color(0.32, 0.36, 0.42, 1.0)
            env.ambient_energy = 0.55
            env.env_intensity = 0.6
            env.sun_colour = wr.Color(1.0, 0.94, 0.84, 1.0)
            env.sun_energy = 2.4
            env.sun_direction = wr.Vec3(-0.42, -0.72, -0.55)
            env.shadow_distance = 90.0

        sun = wr.DirectionalLight3D()
        sun.name = "Sun"
        # Overcast, low and cold. A dead town at four in the
        # afternoon in November, which is the light the whole mode
        # is written for -- bright sun would make it a holiday.
        sun.energy = 1.5
        sun.euler = wr.Vec3(0.7, -0.62, 0.0)
        self.add_child(sun)

        polys = self.town.bake()
        wr.log(f"town: {polys} polys, {len(self.town.links)} links, "
               f"{self.town.region.bake_seconds() * 1000:.0f} ms")
        for line in self.town.door_report():
            wr.log("  DOOR " + line)
        for line in self.town.link_report():
            wr.log("  link " + line)
        for key, hit, total, y in self.town.coverage():
            mark = "ok " if hit > total * 0.4 else "GAP"
            wr.log(f"  {mark} {key:14s} y={y:6.2f} {hit:3d}/{total:3d}")
        # Can a body actually get there from where the run begins?
        # This is the question the pruning answers by deleting, and
        # it is worth asking out loud first.
        start = self.town.sample_point(self.design.zones["crossroads"])
        for key in sorted(self.design.zones):
            p = self.town.sample_point(self.design.zones[key])
            if not self.town.region.is_reachable(start, p):
                path = self.town.region.find_path(start, p)
                wr.log(f"  UNREACHABLE {key}: "
                       f"from ({start.x:.1f},{start.y:.1f},{start.z:.1f}) "
                       f"to ({p.x:.1f},{p.y:.1f},{p.z:.1f}) "
                       f"path={len(path)} pts")
        if os.environ.get("ROTGRAVE_PROBE"):
            r = self.town.region
            pairs = [("yard->church", (27, 0.7, -35.0), (27, 1.0, -37.0)),
                     ("yard->far yard", (27, 0.7, -35.0), (14, 0.7, -33.0)),
                     ("street->yard", (27, 0.1, -28.0), (27, 0.7, -35.0)),
                     ("church->crypt", (27, 1.0, -40.0), (20, -6.0, -46.0))]
            for label, a, bb in pairs:
                pa = r.nearest_point(wr.Vec3(*a))
                pb = r.nearest_point(wr.Vec3(*bb))
                ok = r.is_reachable(pa, pb)
                n = len(r.find_path(pa, pb))
                wr.log(f"  probe {label:16s} {'OK ' if ok else 'NO '} "
                       f"({pa.x:.1f},{pa.y:.2f},{pa.z:.1f}) -> "
                       f"({pb.x:.1f},{pb.y:.2f},{pb.z:.1f}) path={n}")
                if label == "yard->church":
                    for q in r.find_path(pa, pb):
                        wr.log(f"      ({q.x:.2f},{q.y:.2f},{q.z:.2f})")

        wr.log(f"open at the start: {', '.join(sorted(self.town.opened))}")
        for key, (cost, door) in sorted(self.town.buyable().items()):
            wr.log(f"  buy {key} for {cost} via {door.kind}")

        # F1 shows the bake. Off by default, on in a heartbeat --
        # it is the difference between "they will not go upstairs"
        # and "there is no upstairs".
        self._nav_debug = False
        self._t = 0.0

        # A look at the whole town from above, with the bake drawn
        # over it. Not a game mode -- a way to see at a glance which
        # zones baked, which roofs exist and where a link dangles,
        # none of which is visible from the ground.
        overhead = os.environ.get("ROTGRAVE_OVERHEAD")
        if overhead:
            self._nav_debug = True
            self.town.show_navmesh(True)
            if overhead == "2":
                # Hide the town, so the bake is all there is to see.
                self.town._mesh_node.visible = False
            spot = os.environ.get("ROTGRAVE_AT")
            if spot:
                x, y, z = (float(v) for v in spot.split(","))
                self.player.overhead(wr.Vec3(x, y, z))
            else:
                self.player.overhead(wr.Vec3(-6, 175, -4))

    def _process(self, dt):
        if wr.mouse_pressed(wr.Mouse.LEFT):
            wr.capture_mouse(True)
        if wr.key_pressed(wr.Key.ESCAPE):
            wr.capture_mouse(False)
        if wr.key_pressed(wr.Key.F1):
            self._nav_debug = not self._nav_debug
            self.town.show_navmesh(self._nav_debug)
        self.player.update(dt, wr.mouse_captured())


def start():
    root = wr.scene() or wr.root()
    g = Game()
    g.name = "ROTGRAVE"
    root.add_child(g)


start()
