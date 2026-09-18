"""ROTGRAVE -- run it.

    warren --demo none --script games/rotgrave/main.py

Nothing is built in C++. The engine hands over an empty tree and the
game puts a town in it. The script's own directory is on sys.path
already -- `--script FILE` puts it there, the way every Python
runtime does -- so `import rotgrave` works from wherever it is run.
"""

import warren as wr

from rotgrave.design import load
from rotgrave.town import Town, AREA_OPEN, AREA_SEALED
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

        self._t = 0.0

    def _process(self, dt):
        if wr.mouse_pressed(wr.Mouse.LEFT):
            wr.capture_mouse(True)
        if wr.key_pressed(wr.Key.ESCAPE):
            wr.capture_mouse(False)
        self.player.update(dt, wr.mouse_captured())


def start():
    root = wr.scene() or wr.root()
    g = Game()
    g.name = "ROTGRAVE"
    root.add_child(g)


start()
