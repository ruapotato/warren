"""ROTGRAVE -- the dead, and what they want.

Eleven species out of `design/rotgrave_design.json`, which carries
what each one is worth in points and how it moves, and nothing about
what it looks like. The old version's bodies are MakeHuman meshes in
a bespoke figure format that this engine does not read; bringing
them across is its own job. Until then the bodies are built here,
out of boxes, tinted by the species' own colour. That is enough to
tell a shambler from a hound at forty metres, which is the only
thing the silhouette has to do.

WHO THEY GO FOR IS NOT "THE NEAREST".

Nearest splits a crowd of thirty perfectly evenly and gives four
players the same fight. The weighting here is distance, then how
recently that player fired -- a loud player pulls -- then a bias
fixed per body when it spawns, so some of them fixate and the crowd
arrives as a crowd rather than as a fair share. Downed players are
weighted down, so a squad is not punished twice for one mistake.
"""

import math
import random

import warren as wr


# The health curve, which is the only one of the four in DESIGN.md
# that the dump does not carry a base for. Flat to round nine, then
# ten per cent a round, which doubles about every seven.
BASE_HEALTH = 150.0
HEALTH_FLAT_UNTIL = 9
HEALTH_GROWTH = 1.10

# Damage climbs far more slowly than health, on purpose. A hit that
# halves you at round ten and kills you at twelve is not a
# difficulty curve, it is a wall; the pressure should come from how
# MANY and how FAST, which a player can out-play.
DAMAGE_BASE = 22.0
DAMAGE_PER_ROUND = 1.6

PAY_HIT = 10
PAY_KILL = 60
PAY_HEAD = 100


def health_at(round_no, species):
    growth = HEALTH_GROWTH ** max(0, round_no - HEALTH_FLAT_UNTIL)
    return BASE_HEALTH * growth * species.health


def damage_at(round_no, species):
    return (DAMAGE_BASE + DAMAGE_PER_ROUND * (round_no - 1)) * species.damage


def _tint(hexstr):
    v = int(hexstr, 16)
    return wr.Color(((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0,
                    (v & 255) / 255.0, 1.0)


def shade(c, k):
    """A tint, lightened or darkened. Keeps a body from being one
    flat colour, which is what makes a box look like a box."""
    return wr.Color(min(1.0, c.r * k), min(1.0, c.g * k),
                    min(1.0, c.b * k), 1.0)


def build_figure(skin, cloth, height=1.8, radius=0.38, hunch=0.0,
                 arms_forward=0.55, bulk=1.0):
    """An upright body, out of boxes, in parts.

    Parts, not one box, and each with its own colour: the engine
    multiplies vertex colour into albedo, so a single material
    gives a figure with a head a different tone from its coat. A
    body that is one flat slab reads as furniture, which is exactly
    what the first pass looked like.

    `hunch` leans the trunk forward and drops the head -- the
    difference between a survivor and something that used to be
    one -- and `arms_forward` is how far the arms reach. Arms
    forward is not decoration: a shambler reaching is the shape
    that says it has seen you, and hanging them at the back was the
    single most-complained-about thing in the version this is a
    port of.
    """
    b = wr.MeshBuilder()
    h, r = height, radius * bulk
    lean = hunch * 0.16

    # Legs.
    b.colour = shade(cloth, 0.72)
    for sx in (-1, 1):
        b.add_box(wr.Vec3(sx * r * 0.45, h * 0.21, 0.0),
                  wr.Vec3(r * 0.52, h * 0.42, r * 0.62))
        b.add_box(wr.Vec3(sx * r * 0.45, h * 0.035, r * 0.12),
                  wr.Vec3(r * 0.56, h * 0.07, r * 0.95))

    # Trunk, leaning.
    b.colour = cloth
    b.add_box(wr.Vec3(0.0, h * 0.60, -lean * h),
              wr.Vec3(r * 1.75, h * 0.40, r * 1.05))
    b.colour = shade(cloth, 0.86)
    b.add_box(wr.Vec3(0.0, h * 0.44, -lean * h * 0.4),
              wr.Vec3(r * 1.55, h * 0.12, r * 0.98))

    # Arms: upper out from the shoulder, lower reaching.
    b.colour = shade(cloth, 0.92)
    for sx in (-1, 1):
        b.add_box(wr.Vec3(sx * r * 1.05, h * 0.62, -lean * h),
                  wr.Vec3(r * 0.42, h * 0.30, r * 0.46))
        b.colour = skin
        b.add_box(wr.Vec3(sx * r * 1.05, h * 0.50 + arms_forward * h * 0.12,
                          -(lean + arms_forward * 0.30) * h),
                  wr.Vec3(r * 0.40, r * 0.40, h * 0.34 * (0.4 + arms_forward)))
        b.colour = shade(cloth, 0.92)

    # Neck and head, dropped by the hunch.
    b.colour = skin
    b.add_box(wr.Vec3(0.0, h * 0.82, -lean * h * 1.3),
              wr.Vec3(r * 0.42, h * 0.06, r * 0.42))
    b.add_box(wr.Vec3(0.0, h * 0.90 - hunch * h * 0.04,
                      -(lean * 1.6) * h),
              wr.Vec3(r * 0.86, h * 0.13, r * 0.80))
    # A brow, so the head has a front.
    b.colour = shade(skin, 0.7)
    b.add_box(wr.Vec3(0.0, h * 0.93 - hunch * h * 0.04,
                      -(lean * 1.6) * h - r * 0.42),
              wr.Vec3(r * 0.78, h * 0.035, r * 0.10))
    return b.build()


def build_beast(skin, cloth, height=1.2, radius=0.4):
    """Four legs, low and long. Read from above, which is how a
    player usually first sees one coming."""
    b = wr.MeshBuilder()
    h, r = height, radius
    b.colour = cloth
    b.add_box(wr.Vec3(0, h * 0.66, 0), wr.Vec3(r * 1.5, h * 0.44, h * 1.35))
    b.colour = shade(cloth, 0.8)
    b.add_box(wr.Vec3(0, h * 0.62, h * 0.78),
              wr.Vec3(r * 0.5, h * 0.18, h * 0.5))   # tail end
    b.colour = skin
    b.add_box(wr.Vec3(0, h * 0.74, -h * 0.80),
              wr.Vec3(r * 1.1, h * 0.30, r * 1.5))   # head
    b.colour = shade(skin, 0.62)
    b.add_box(wr.Vec3(0, h * 0.66, -h * 1.02),
              wr.Vec3(r * 0.7, h * 0.16, r * 0.6))   # muzzle
    b.colour = shade(cloth, 0.7)
    for sx in (-1, 1):
        for sz, k in ((-0.55, 1.0), (0.55, 0.92)):
            b.add_box(wr.Vec3(sx * r * 0.78, h * 0.22, sz * h),
                      wr.Vec3(r * 0.32, h * 0.44 * k, r * 0.32))
    return b.build()


class Bodies:
    """One mesh per species, built once and shared.

    Thirty bodies on screen is thirty MeshInstance3D nodes pointing
    at eleven meshes, not thirty meshes. The material is white,
    because the colour is in the vertices.
    """

    def __init__(self, design):
        self.design = design
        self.meshes = {}
        self.material = wr.Material()
        self.material.albedo = wr.Color(1, 1, 1, 1)
        self.material.roughness = 0.88
        for key, sp in design.undead.items():
            self.meshes[key] = self._build(sp)

    def _build(self, sp):
        cloth = _tint(sp.tint)
        # Skin from the species' gore colour, which is the one other
        # colour the dump gives per species and is about right for
        # what is left of one.
        skin = _gore_tint(sp.gore, cloth)
        h = sp.height if sp.height > 0.1 else 1.8
        if sp.kind == "scene":
            return build_beast(skin, cloth, h, sp.radius)
        # The faster it runs, the further forward it leans.
        hunch = min(1.0, 0.25 + sp.run_speed / 12.0)
        return build_figure(skin, cloth, h, sp.radius, hunch=hunch,
                            arms_forward=0.85, bulk=1.0 + (sp.health - 1.0)
                            * 0.25)


def _gore_tint(gore, fallback):
    """The dump writes gore as "(r, g, b, a)". Lightened, because
    what it describes is what comes out rather than what is left."""
    try:
        parts = [float(v) for v in gore.strip("() ").split(",")]
        c = wr.Color(parts[0], parts[1], parts[2], 1.0)
        return shade(c, 2.1)
    except Exception:
        return shade(fallback, 1.3)


class Zombie:
    """One body: an agent, a mesh, a target and a grudge."""

    __slots__ = ("node", "agent", "species", "health", "max_health",
                 "damage", "bias", "swing_at", "dead", "round_no", "mesh",
                 "groan_at", "sound")

    # Beyond this a body stops asking for a new path every time the
    # player moves; the route it has is good enough until it is not.
    RETARGET_STEP = 2.5

    def __init__(self, design, bodies, species_key, round_no, at, parent, rng):
        sp = design.undead[species_key]
        self.species = sp
        self.round_no = round_no
        self.max_health = health_at(round_no, sp)
        self.health = self.max_health
        self.damage = damage_at(round_no, sp)
        # A bias fixed at spawn, so some of them fixate.
        self.bias = rng.uniform(0.7, 1.45)
        self.swing_at = 0.0
        self.dead = False
        self.sound = None
        # Staggered, so a crowd that spawns together does not groan
        # in chorus -- which reads as one large thing rather than as
        # many small ones.
        self.groan_at = rng.uniform(1.0, 6.0)

        self.agent = wr.NavAgent3D()
        self.agent.name = f"{species_key}"
        self.agent.radius = sp.radius
        self.agent.height = sp.height if sp.height > 0.1 else 1.8
        self.agent.max_speed = sp.speed_at(round_no)
        self.agent.max_accel = 9.0
        # Near enough to swing. Thirty bodies cannot stand on one
        # point, and asking them to is what makes a mob orbit.
        self.agent.goal_radius = max(sp.reach * 0.8, 1.2)
        self.agent.position = at
        parent.add_child(self.agent)

        self.node = wr.MeshInstance3D()
        self.node.name = "Body"
        self.node.mesh = bodies.meshes[species_key]
        self.node.set_material(0, bodies.material)
        self.agent.add_child(self.node)

    @property
    def position(self):
        return self.agent.global_position

    def hurt(self, amount, headshot=False):
        """Returns the points earned, and whether this killed it."""
        if self.dead:
            return 0, False
        self.health -= amount * (2.0 if headshot else 1.0)
        if self.sound:
            self.sound.at("hit_head" if headshot else "hit_flesh",
                          self.position, volume=0.8)
        if self.health > 0.0:
            return PAY_HIT, False
        self.dead = True
        if self.sound:
            self.sound.pick(["undead_die1", "undead_die2", "undead_die3"],
                            self.position, volume=0.9)
            self.sound.at("gore_burst", self.position, volume=0.5)
        return (PAY_HEAD if headshot else PAY_KILL) + self.species.points, True

    def update(self, dt, target, now):
        if self.dead:
            return None
        self.agent.max_speed = self.species.speed_at(self.round_no)
        if target is None:
            return None
        self.agent.set_target(target)

        # A groan every so often, so a crowd behind you is audible
        # before it is visible. That is most of what the sound is
        # for in this mode.
        if self.sound and now >= self.groan_at:
            self.groan_at = now + 4.0 + (self.bias * 3.0)
            if self.species.kind == "scene":
                self.sound.pick(["hound_growl", "hound_bark", "hound_bark2"],
                                self.position, volume=0.75)
            else:
                self.sound.pick(["groan1", "groan2", "groan3", "groan4",
                                 "groan5"], self.position, volume=0.7)

        # Close enough, and the swing has come round again.
        d = self.position - target
        flat = math.sqrt(d.x * d.x + d.z * d.z)
        if flat > self.species.reach or now < self.swing_at:
            return None
        self.swing_at = now + self.species.swing
        if self.sound:
            self.sound.pick(["undead_attack1", "undead_attack2"],
                            self.position, volume=0.9)
        return self.damage

    def free(self):
        self.agent.queue_free()


def pick_target(zombie, players, now):
    """Which of them it goes for.

    Distance first, because a body that ignores the person in front
    of it is not frightening. Then loudness, then the body's own
    bias. Downed players are weighted down so a squad is not
    punished twice for one mistake.
    """
    best, best_score = None, -1.0
    for p in players:
        d = zombie.position - p.position
        dist = max(math.sqrt(d.x * d.x + d.z * d.z), 0.5)
        score = 1.0 / dist
        # Fired in the last few seconds: a loud player pulls.
        since = now - getattr(p, "last_shot", -99.0)
        if since < 3.0:
            score *= 1.0 + (3.0 - since) * 0.25
        if getattr(p, "downed", False):
            score *= 0.25
        score *= zombie.bias
        if score > best_score:
            best, best_score = p, score
    return best
