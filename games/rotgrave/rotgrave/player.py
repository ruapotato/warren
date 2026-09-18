"""ROTGRAVE -- the survivor, and the camera over their shoulder.

Third person, and zoomed in enough that the body reads. The reason
is the portal gun: a first-person view through a hole you made is a
view with no way to tell whether you went through it. Seeing your own
body arrive on the other side is the feedback that makes the
mechanic legible, and it only works if the body is big enough in
frame to watch.
"""

import math
import random

import warren as wr

from .figures import Wardrobe
from .undead import build_figure

# WHO YOU ARE. One figure, for now: the old version had a
# character-maker that picked a body, a haircut and an outfit out of
# the modular pieces in the archive, and every one of those pieces
# still converts. Until that screen exists there is one survivor,
# and they are 1.8 m because that is the height the camera boom, the
# step height and the zombies' reach were all set against.
HERO = "hero"
HERO_HEIGHT = 1.8
from .weapons import Gun, Knife, resolve_shot


class Player:
    EYE = 1.62
    WALK = 4.6
    SPRINT = 7.2
    # How far back and up the camera sits, and how fast it catches
    # up. Instant is nauseating and slow is seasick; a tenth of a
    # second reads as a camera operator who is paying attention.
    BOOM = 4.2
    BOOM_UP = 1.15
    BOOM_LAG = 0.10

    # A survivor takes a few hits, not one and not twenty. The
    # number matters less than the regeneration: out of contact for
    # a moment and you are whole again, which is what lets a player
    # take a risk and recover rather than limp for a round.
    MAX_HEALTH = 100.0
    REGEN_AFTER = 4.0
    REGEN_RATE = 26.0

    def __init__(self, world):
        self.world = world
        self.body = None
        self.camera = None
        self.yaw = 0.0
        self.pitch = -0.12
        self._boom = wr.Vec3()
        self._captured = False

        self.health = self.MAX_HEALTH
        self.downed = False
        self.last_hurt = -99.0
        self.last_shot = -99.0
        self.points = 0

        # THREE SLOTS, AND THE KNIFE IS ONE OF THEM.
        #
        # One and two are the guns, three is the knife. Keeping the
        # knife on a slot rather than on a modifier key is the old
        # version's choice and a good one: it is a weapon, it earns
        # more per kill than any gun, and a player who commits to it
        # is taking a real risk rather than tapping a button.
        self.sound = None
        self._step_at = 0.0
        self._was_reloading = False
        self.guns = [None, None]
        self.knife = None
        self.slot = 0
        self.aiming = False
        self.moving = False

    def spawn(self, parent, at, wardrobe=None):
        self.body = wr.CharacterBody3D()
        self.body.name = "Survivor"
        self.body.radius = 0.4
        self.body.height = 1.8
        self.body.step_height = 0.45
        self.body.position = at
        parent.add_child(self.body)

        # A VISIBLE BODY, and a legible one. A third-person camera
        # with nothing under it is a floating camera, and a
        # third-person camera with a featureless slab under it is a
        # camera following a fridge -- which is what the first pass
        # looked like.
        #
        # The survivor is the MakeHuman figure in assets/figures,
        # dressed and standing upright. Upright is the point: they
        # have to be the one silhouette on screen that is obviously
        # not one of them, and posture does that at any distance
        # where colour will not.
        shape = (wardrobe or Wardrobe()).make(HERO, HERO_HEIGHT)
        if shape is None:
            shape = wr.MeshInstance3D()
            shape.mesh = build_figure(
                skin=wr.Color(0.78, 0.63, 0.50, 1.0),
                cloth=wr.Color(0.24, 0.29, 0.26, 1.0),
                height=HERO_HEIGHT, radius=0.34, hunch=0.0,
                arms_forward=0.10)
            mat = wr.Material()
            mat.albedo = wr.Color(1, 1, 1, 1)
            mat.roughness = 0.78
            shape.set_material(0, mat)
        shape.name = "Body"
        self.body.add_child(shape)

        self.camera = wr.Camera3D()
        self.camera.name = "Eye"
        parent.add_child(self.camera)
        self.camera.make_current()
        self._boom = at
        return self

    @property
    def position(self):
        return self.body.global_position if self.body else wr.Vec3()

    def look_direction(self):
        cp = math.cos(self.pitch)
        return wr.Vec3(-math.sin(self.yaw) * cp, math.sin(self.pitch),
                       -math.cos(self.yaw) * cp)

    def overhead(self, at):
        """Park the camera above the town and stop driving it."""
        self._overhead = True
        self.camera.position = at
        self.camera.euler = wr.Vec3(0.0, -1.35, 0.0)

    def arm(self, design, rng=None):
        rng = rng or random.Random(1)
        self.guns[0] = Gun(design.weapons["pistol"], rng=rng)
        self.knife = Knife(design.melee)

    @property
    def held(self):
        return self.knife if self.slot == 2 else self.guns[self.slot]

    def select(self, slot):
        if slot == 2 or self.guns[slot] is not None:
            self.slot = slot

    def take_gun(self, gun):
        """Into the empty slot, or over the one in hand.

        Over the one IN HAND rather than the oldest: a player who
        picks something up while holding the gun they want to keep
        has made a mistake they can see coming, which is fair, and
        one they can avoid by switching first.
        """
        slot = self.slot if self.slot < 2 else 0
        if self.guns[0] is None:
            slot = 0
        elif self.guns[1] is None:
            slot = 1
        self.guns[slot] = gun
        self.slot = slot
        return slot

    def shoot(self, zombies, now):
        """Pull the trigger. Returns the hits, or nothing."""
        gun = self.held
        if gun is None or isinstance(gun, Knife):
            return []
        gun.tick(now)
        if not gun.can_fire(now):
            if gun.empty:
                gun.begin_reload(now)
            return []
        gun.fire(now)
        self.last_shot = now
        if self.sound:
            # A GUN IS THREE SOUNDS. The crack is the round leaving,
            # the body is what makes a rifle a rifle, and the tail is
            # the street answering -- which is the only part that
            # says whether you are indoors. The bank keeps the tail
            # separate so it can be played at its own volume.
            self.sound.at(gun.spec.get("shot", "gun_pistol"),
                          self.position, volume=1.0, jitter=0.04)
            tail = gun.spec.get("tail")
            if tail:
                self.sound.at(tail, self.position, volume=0.55, jitter=0.03)
        origin, _ = self.aim_ray()
        return resolve_shot(self.world, zombies, gun, origin,
                            self.look_direction(), self.moving, self.aiming)

    def stab(self, zombies, now):
        k = self.knife
        if k is None or not k.can_swing(now):
            return []
        damage = k.swing(now)
        self.last_shot = now
        origin = self.camera.global_position
        aim = self.look_direction()
        out = []
        for z in zombies:
            if z.dead:
                continue
            d = z.position - origin
            if d.length() > k.reach:
                continue
            if aim.dot(d.normalized()) < 0.55:
                continue  # behind you, or beside you
            out.append((z, damage, False))
            break  # one body a swing
        return out

    def wound(self, amount):
        if self.downed:
            return
        self.health -= amount
        self.last_hurt = wr.time()
        if self.sound:
            self.sound.pick(["hurt_player1", "hurt_player2", "hurt_player3"],
                            volume=0.9)
        if self.health <= 0.0:
            self.health = 0.0
            self.downed = True
            if self.sound:
                self.sound.flat("downed", volume=1.0)
            wr.log("you are down")

    def revive(self):
        self.downed = False
        self.health = self.MAX_HEALTH * 0.5

    def _heal(self, dt):
        if self.downed:
            return
        if wr.time() - self.last_hurt < self.REGEN_AFTER:
            return
        self.health = min(self.MAX_HEALTH,
                          self.health + self.REGEN_RATE * dt)

    def aim_ray(self):
        """From the camera, through the crosshair, out to range.

        From the CAMERA rather than from the body, because the
        crosshair is on the camera and a shot that does not go where
        the crosshair is pointing is a shot the player will call a
        miss whatever the geometry says.
        """
        origin = self.camera.global_position
        return origin, origin + self.look_direction() * 120.0

    def update(self, dt, window_captured):
        if not self.body or getattr(self, "_overhead", False):
            return
        self._heal(dt)
        if window_captured:
            m = wr.mouse_motion()
            self.yaw -= m.x * 0.0022
            self.pitch = max(-1.35, min(1.15, self.pitch - m.y * 0.0022))

        # Movement is in the camera's frame with the pitch thrown
        # away, so looking at your feet does not walk you into the
        # ground.
        fwd = wr.Vec3(-math.sin(self.yaw), 0.0, -math.cos(self.yaw))
        right = wr.Vec3(math.cos(self.yaw), 0.0, -math.sin(self.yaw))
        wish = wr.Vec3()
        if wr.action_down("forward"):
            wish = wish + fwd
        if wr.action_down("back"):
            wish = wish - fwd
        if wr.action_down("right"):
            wish = wish + right
        if wr.action_down("left"):
            wish = wish - right
        if wish.length() > 0.001:
            wish = wish.normalized()

        self.moving = wish.length() > 0.001
        speed = self.SPRINT if wr.action_down("sprint") else self.WALK
        v = self.body.velocity
        target = wish * speed
        # Ground acceleration is quick and air control is not, which
        # is the difference between a body with weight and a hovering
        # camera. Nothing here is a physics model; it is a feel.
        rate = 14.0 if self.body.is_on_floor() else 2.5
        k = min(1.0, rate * dt)
        v = wr.Vec3(v.x + (target.x - v.x) * k, v.y,
                    v.z + (target.z - v.z) * k)
        if self.body.is_on_floor() and wr.action_pressed("jump"):
            v = wr.Vec3(v.x, 6.0, v.z)
        self.body.velocity = v
        self.body.move_and_slide(self.world, dt)
        self._footsteps(dt, speed)

        # The body faces where it is going, not where the camera
        # looks -- a survivor walking sideways while staring forward
        # is a strafing animation nobody has.
        if wish.length() > 0.001:
            self.body.euler = wr.Vec3(math.atan2(-wish.x, -wish.z), 0.0, 0.0)

        self._follow(dt)

    def _footsteps(self, dt, speed):
        """A step every so many metres, not every so many seconds.

        Timed steps drift out of phase with the legs the moment the
        speed changes, and the ear notices immediately even when the
        eye does not.
        """
        if not self.sound or not self.body.is_on_floor():
            return
        v = self.body.velocity
        pace = math.sqrt(v.x * v.x + v.z * v.z)
        if pace < 0.6:
            return
        self._step_at -= pace * dt
        if self._step_at > 0.0:
            return
        # Every 0.8 m walking, a little longer at a run: a sprint is
        # a longer stride, not a faster shuffle.
        self._step_at = 0.8 if speed <= self.WALK else 1.15
        # Quieter than everything else on purpose. The old version's
        # notes single this out: footsteps too loud against the rest
        # is the first thing anybody complains about.
        self.sound.pick(["step_road1", "step_road2", "step_road3"],
                        self.position, volume=0.30, jitter=0.12)

    def _reload_noise(self, gun, now):
        if not self.sound or gun is None:
            return
        if gun.reloading and not self._was_reloading:
            self.sound.at("mag_out", self.position, volume=0.7)
        elif self._was_reloading and not gun.reloading:
            self.sound.at("mag_in", self.position, volume=0.7)
            self.sound.at("bolt_fwd", self.position, volume=0.5)
        self._was_reloading = gun.reloading

    def _follow(self, dt):
        want = self.position + wr.Vec3(0.0, self.EYE, 0.0)
        k = 1.0 - math.exp(-dt / max(self.BOOM_LAG, 1e-4))
        self._boom = self._boom + (want - self._boom) * k

        look = self.look_direction()
        eye = self._boom - look * self.BOOM + wr.Vec3(0.0, self.BOOM_UP, 0.0)

        # Do not put the camera through a wall. Tracing from the head
        # outwards and stopping short is the whole of third-person
        # camera collision and the difference between a shooter and a
        # tour of the inside of a brick.
        hit = self.world.trace(self._boom, eye, 0xffffffff)
        if hit.get("hit"):
            p = hit["position"]
            n = hit.get("normal", wr.Vec3(0, 1, 0))
            eye = wr.Vec3(p.x, p.y, p.z) + n * 0.25

        self.camera.position = eye
        self.camera.euler = wr.Vec3(self.yaw, self.pitch, 0.0)
