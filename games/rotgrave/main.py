"""ROTGRAVE -- run it.

    warren --demo none --script games/rotgrave/main.py

Nothing is built in C++. The engine hands over an empty tree and the
game puts a town in it. The script's own directory is on sys.path
already -- `--script FILE` puts it there, the way every Python
runtime does -- so `import rotgrave` works from wherever it is run.
"""

import math
import os

import warren as wr

# A hand on the trigger for headless runs. See Game._autofire.
_AUTOFIRE = bool(os.environ.get("ROTGRAVE_AUTOFIRE"))

from rotgrave.design import load
from rotgrave.town import Town, AREA_OPEN, AREA_SEWER
from rotgrave.player import Player
from rotgrave.effects import Effects
from rotgrave.hud import Hud
from rotgrave.sound import Sound
from rotgrave.undead import Bodies
from rotgrave.weapons import Knife
from rotgrave.rounds import Director


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
        self.player.arm(self.design)

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
        # ROTGRAVE_PROBE="x0,y,z0,x1,z1" walks a line and reports
        # what the navmesh has along it. A junction that does not
        # connect shows up as a jump in height, a run of misses, or
        # two stretches whose areas never meet.
        probe = os.environ.get("ROTGRAVE_PROBE")
        if probe:
            x0, py, z0, x1, z1 = (float(v) for v in probe.split(","))
            r = self.town.region
            n = 24
            prev = None
            for i in range(n + 1):
                t = i / n
                x, z = x0 + (x1 - x0) * t, z0 + (z1 - z0) * t
                want = wr.Vec3(x, py, z)
                got = r.nearest_point(want)
                d = (got - want).length()
                jump = "" if prev is None else f" dy={got.y - prev:+.2f}"
                mark = "   " if d < 0.6 else "!! "
                wr.log(f"  {mark}probe ({x:6.1f},{z:6.1f}) -> y={got.y:5.2f} "
                       f"off={d:4.2f} area={r.area_at(got):3d}{jump}")
                prev = got.y
            a = r.nearest_point(wr.Vec3(x0, py, z0))
            b = r.nearest_point(wr.Vec3(x1, py, z1))
            path = r.find_path(a, b)
            wr.log(f"  probe ends: reachable={r.is_reachable(a, b)} "
                   f"path={len(path)} "
                   f"end=({path[-1].x:.1f},{path[-1].z:.1f}) "
                   f"wanted=({b.x:.1f},{b.z:.1f})" if path else "  no path")

        wr.log(f"open at the start: {', '.join(sorted(self.town.opened))}")
        for key, (cost, door) in sorted(self.town.buyable().items()):
            wr.log(f"  buy {key} for {cost} via {door.kind}")

        # F1 shows the bake. Off by default, on in a heartbeat --
        # it is the difference between "they will not go upstairs"
        # and "there is no upstairs".
        self.sound = Sound(self)
        self.player.sound = self.sound
        self.effects = Effects(self)
        self.hud = Hud(self)
        self.bodies = Bodies(self.design)
        # Under the region, because that is where a NavAgent3D
        # looks for the crowd it belongs to -- it walks up its
        # ancestors until it finds a NavRegion3D, and one parented
        # anywhere else never moves.
        self.director = Director(self.design, self.town, self.bodies,
                                 self.town.region, seed=1,
                                 sound=self.sound)

        self._nav_debug = False
        self._report_at = 0.0
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

        now = wr.time()
        self.player.update(dt, wr.mouse_captured())

        if _AUTOFIRE:
            self._autofire(now)
        elif wr.mouse_captured():
            self._weapons(now)

        self.director.update(dt, now, [self.player])
        self.effects.update(now)
        self.hud.update(now, self.player, self.director)

        if now >= self._report_at:
            self._report_at = now + 5.0
            held = self.player.held
            ammo = ("knife" if isinstance(held, Knife) or held is None
                    else f"{held.mag}/{held.reserve}")
            wr.log(self.director.report()
                   + f"  |  {self.player.health:.0f} hp"
                   + f"  |  {self.player.points} pts"
                   + f"  |  {held.name if held else '-'} {ammo}")

    def _autofire(self, now):
        """A hand on the trigger, for a run with nobody at the keyboard.

        Not a cheat and not a bot: it aims at the nearest body and
        pulls, which is the one thing a headless run cannot do and
        the one thing the whole round loop depends on. Without it a
        test run spawns to the cap and stops, and nothing past the
        spawner is ever exercised.
        """
        p = self.player
        alive = [z for z in self.director.alive if not z.dead]
        if not alive:
            return
        eye = p.camera.global_position
        world = wr.physics()
        best, best_d = None, 1e9
        for z in alive:
            d = (z.position - eye).length()
            if d >= best_d or d > 60.0:
                continue
            # Only what it can actually see. Firing at a body
            # through a building empties the magazine into a wall,
            # which is what a player would not do and what made the
            # first headless run look like the guns did not work.
            chest = z.position + wr.Vec3(0, z.agent.height * 0.6, 0)
            blocked = world.trace(eye, chest, 0xffffffff)
            if blocked.get("hit"):
                q = blocked["position"]
                if (wr.Vec3(q.x, q.y, q.z) - eye).length() < d - 0.6:
                    continue
            best, best_d = z, d
        if best is None:
            return
        aim = (best.position + wr.Vec3(0, best.agent.height * 0.6, 0)) - eye
        if aim.length() < 1e-3:
            return
        aim = aim.normalized()
        p.yaw = math.atan2(-aim.x, -aim.z)
        p.pitch = math.asin(max(-1.0, min(1.0, aim.y)))

        held = p.held
        if held is None or isinstance(held, Knife):
            return
        held.tick(now)
        if held.empty:
            held.begin_reload(now)
            return
        before = held.mag
        hits = p.shoot(alive, now)
        if held.mag < before:
            self._flash(now)
        self._land(hits, now)

    def _weapons(self, now):
        p = self.player
        for key, slot in ((wr.Key.NUM1, 0), (wr.Key.NUM2, 1),
                          (wr.Key.NUM3, 2)):
            if wr.key_pressed(key) and p.slot != slot:
                p.select(slot)
                self.sound.flat("weapon_swap", volume=0.6)

        held = p.held
        if held is not None and not isinstance(held, Knife):
            held.tick(now)
            p._reload_noise(held, now)
            if wr.key_pressed(wr.Key.R):
                held.begin_reload(now)
            p.aiming = wr.mouse_down(wr.Mouse.RIGHT)

        # An automatic weapon fires while held, a semi-automatic on
        # the press. The dump says which, per gun.
        fire = False
        if isinstance(held, Knife) or held is None:
            fire = wr.mouse_pressed(wr.Mouse.LEFT)
        elif held.spec.get("auto", False):
            fire = wr.mouse_down(wr.Mouse.LEFT)
        else:
            fire = wr.mouse_pressed(wr.Mouse.LEFT)
        if not fire:
            return

        alive = self.director.alive
        if isinstance(held, Knife) or held is None:
            hits = p.stab(alive, now)
            pay_kill = Knife.PAY
        else:
            before = held.mag
            hits = p.shoot(alive, now)
            if held.mag < before:
                self._flash(now)
            pay_kill = None
        self._land(hits, now, pay_kill)

    def _flash(self, now):
        p = self.player
        held = p.held
        if held is None or isinstance(held, Knife):
            return
        aim = p.look_direction()
        # At the shoulder rather than at the camera: the camera is
        # two metres behind the body, and a flash there is a flash
        # floating in the air behind the player.
        muzzle = (p.position + wr.Vec3(0.0, 1.35, 0.0)
                  + aim * 0.75 + p.body.right() * 0.18)
        origin, far = p.aim_ray()
        end = origin + aim * float(held.spec.range)
        wall = wr.physics().trace(muzzle, end, 0xffffffff)
        if wall.get("hit"):
            q = wall["position"]
            end = wr.Vec3(q.x, q.y, q.z)
        self.effects.shot(now, muzzle, aim, end)

    def _land(self, hits, now, pay_kill=None):
        """Turn hits into points, corpses and feedback."""
        p = self.player
        for z, damage, head in hits:
            points, killed = z.hurt(damage, headshot=head)
            if killed and pay_kill is not None:
                points = pay_kill + z.species.points
            p.points += points
            self.hud.hit(now, killed)
            if killed:
                self.director.killed(z, points)
                self.effects.corpse(now, z.node, z.position,
                                    z.agent.euler.x)
            if points:
                self.sound.flat("points_up", volume=0.25)


def start():
    root = wr.scene() or wr.root()
    g = Game()
    g.name = "ROTGRAVE"
    root.add_child(g)


start()
