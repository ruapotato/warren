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


class Bodies:
    """One mesh per species, built once and shared.

    Thirty bodies on screen is thirty MeshInstance3D nodes pointing
    at eleven meshes, not thirty meshes.
    """

    def __init__(self, design):
        self.design = design
        self.meshes = {}
        self.materials = {}
        for key, sp in design.undead.items():
            self.meshes[key] = self._build(sp)
            m = wr.Material()
            m.albedo = _tint(sp.tint)
            m.roughness = 0.82
            self.materials[key] = m

    def _build(self, sp):
        b = wr.MeshBuilder()
        h = sp.height if sp.height > 0.1 else 1.8
        r = sp.radius
        if sp.kind == "scene":
            # The four-legged ones: long, low, and read from above,
            # which is how a player usually sees them coming.
            b.add_box(wr.Vec3(0, h * 0.62, 0), wr.Vec3(r * 1.5, h * 0.5,
                                                       h * 1.5))
            b.add_box(wr.Vec3(0, h * 0.78, -h * 0.85),
                      wr.Vec3(r * 1.2, r * 1.2, r * 1.6))
            for sx in (-1, 1):
                for sz in (-1, 1):
                    b.add_box(wr.Vec3(sx * r * 0.7, h * 0.2, sz * h * 0.5),
                              wr.Vec3(r * 0.35, h * 0.45, r * 0.35))
            return b.build()

        # Upright: legs, trunk, arms out in front, head. The arms
        # forward matter -- a shambler reaching is the shape that
        # says it has seen you, and the one the old version got
        # wrong for a long time by hanging them behind the back.
        b.add_box(wr.Vec3(0, h * 0.62, 0), wr.Vec3(r * 1.9, h * 0.44,
                                                   r * 1.2))
        b.add_box(wr.Vec3(0, h * 0.92, 0), wr.Vec3(r * 1.0, h * 0.14,
                                                   r * 1.0))
        for sx in (-1, 1):
            b.add_box(wr.Vec3(sx * r * 0.55, h * 0.2, 0),
                      wr.Vec3(r * 0.6, h * 0.4, r * 0.7))
            b.add_box(wr.Vec3(sx * r * 1.05, h * 0.66, -r * 1.1),
                      wr.Vec3(r * 0.45, r * 0.45, h * 0.3))
        return b.build()


class Zombie:
    """One body: an agent, a mesh, a target and a grudge."""

    __slots__ = ("node", "agent", "species", "health", "max_health",
                 "damage", "bias", "swing_at", "dead", "round_no", "mesh")

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
        self.node.set_material(0, bodies.materials[species_key])
        self.agent.add_child(self.node)

    @property
    def position(self):
        return self.agent.global_position

    def hurt(self, amount, headshot=False):
        """Returns the points earned, and whether this killed it."""
        if self.dead:
            return 0, False
        self.health -= amount * (2.0 if headshot else 1.0)
        if self.health > 0.0:
            return PAY_HIT, False
        self.dead = True
        return (PAY_HEAD if headshot else PAY_KILL) + self.species.points, True

    def update(self, dt, target, now):
        if self.dead:
            return None
        self.agent.max_speed = self.species.speed_at(self.round_no)
        if target is None:
            return None
        self.agent.set_target(target)

        # Close enough, and the swing has come round again.
        d = self.position - target
        flat = math.sqrt(d.x * d.x + d.z * d.z)
        if flat > self.species.reach or now < self.swing_at:
            return None
        self.swing_at = now + self.species.swing
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
