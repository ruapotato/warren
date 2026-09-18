"""ROTGRAVE -- twelve guns and a knife.

Every number comes from `design/rotgrave_design.json`: damage,
rate, magazine, reserve, pellets, how many bodies a round goes
through, how far it carries before it starts falling off, what a
head is worth, and what Pack-a-Punch turns it into. None of it is
re-tuned here.

WHAT A SHOT IS. One ray from the camera, jittered by the spread,
repeated per pellet. It travels until it has used up its pierce
count or hit the world, whichever comes first, and every body it
passes through takes damage scaled by how far along it was. The
world is traced too and the shorter hit wins, so a shambler behind
a wall is behind a wall.

Spread is a cone, and it is the only source of randomness in a
shot. Recoil moves the camera and the camera decides the ray, so
recoil is felt rather than simulated separately -- a gun that
kicks and a gun whose bullets wander are different things, and
players can tell.
"""

import math
import random

import warren as wr


# Pack-a-Punch, from the dump: the cost and the multiplier the
# design states. The name each gun becomes is in the dump too.
UPGRADE_DAMAGE = 3.1
UPGRADE_MAG = 1.5
UPGRADE_RESERVE = 1.6


class Gun:
    """One gun, in hand: what it is, and what state it is in."""

    __slots__ = ("spec", "key", "upgraded", "mag", "reserve", "next_shot",
                 "reload_until", "_rng")

    def __init__(self, spec, upgraded=False, rng=None):
        self.spec = spec
        self.key = spec.key
        self.upgraded = upgraded
        self._rng = rng or random.Random(0)
        self.mag = self.mag_size
        self.reserve = self.reserve_size
        self.next_shot = 0.0
        self.reload_until = 0.0

    # --------------------------------------------------- the numbers

    @property
    def name(self):
        if not self.upgraded:
            return self.spec.name
        return self.spec.raw.get("upgrade_name") or self.spec.name

    @property
    def damage(self):
        d = float(self.spec.damage)
        return d * UPGRADE_DAMAGE if self.upgraded else d

    @property
    def mag_size(self):
        n = int(self.spec.mag)
        return int(n * UPGRADE_MAG) if self.upgraded else n

    @property
    def reserve_size(self):
        n = int(self.spec.reserve)
        return int(n * UPGRADE_RESERVE) if self.upgraded else n

    @property
    def interval(self):
        return 60.0 / max(float(self.spec.rpm), 1.0)

    @property
    def reloading(self):
        return self.reload_until > 0.0

    @property
    def empty(self):
        return self.mag <= 0

    # ------------------------------------------------------- actions

    def can_fire(self, now):
        return not self.reloading and self.mag > 0 and now >= self.next_shot

    def begin_reload(self, now):
        """Returns True if a reload actually started.

        An empty magazine takes longer than a partial one, which is
        the dump's `reload_empty` -- and is why a player who counts
        their shots is playing better than one who does not.
        """
        if self.reloading or self.reserve <= 0 or self.mag >= self.mag_size:
            return False
        empty = self.mag <= 0
        wait = float(self.spec.get("reload_empty" if empty else "reload", 2.0))
        self.reload_until = now + wait
        return True

    def tick(self, now):
        if self.reloading and now >= self.reload_until:
            self.reload_until = 0.0
            want = self.mag_size - self.mag
            take = min(want, self.reserve)
            self.mag += take
            self.reserve -= take

    def give_ammo(self):
        """A full reserve, which is what an ammo buy does."""
        self.reserve = self.reserve_size

    def upgrade(self):
        if self.upgraded:
            return False
        self.upgraded = True
        self.mag = self.mag_size
        self.reserve = self.reserve_size
        return True

    # --------------------------------------------------------- shots

    def rays(self, origin, direction, moving, aiming):
        """One ray per pellet, jittered inside the spread cone.

        Spread is in degrees and widens while moving, because the
        alternative -- a gun that is as accurate running as standing
        -- removes the only reason to stop running.
        """
        if self.can_fire(0.0) is None:
            return []
        spread = float(self.spec.get("spread_ads" if aiming else "spread", 1.0))
        if moving:
            spread *= float(self.spec.get("moving", 1.0))
        spread = math.radians(spread)

        # A frame to jitter in: any two axes perpendicular to the aim.
        up = wr.Vec3(0, 1, 0)
        side = direction.cross(up)
        if side.length() < 1e-4:
            side = wr.Vec3(1, 0, 0)
        side = side.normalized()
        lift = side.cross(direction).normalized()

        out = []
        for _ in range(max(1, int(self.spec.get("pellets", 1)))):
            a = self._rng.uniform(0.0, math.tau)
            r = spread * math.sqrt(self._rng.random())
            d = direction + side * (math.cos(a) * r) + lift * (math.sin(a) * r)
            out.append(d.normalized())
        return out

    def fire(self, now):
        """Consume a round. The caller does the tracing."""
        self.mag -= 1
        self.next_shot = now + self.interval
        return True

    def falloff_at(self, distance):
        """Damage multiplier at a range.

        Full damage to the gun's stated range, then down to its
        `falloff` fraction over the next half again. A shotgun's
        eighteen metres and a sniper's three hundred are the same
        rule with different numbers, which is what makes the choice
        between them a choice about distance.
        """
        near = float(self.spec.range)
        if distance <= near:
            return 1.0
        far = near * 1.5
        floor = float(self.spec.get("falloff", 0.5))
        if distance >= far:
            return floor
        t = (distance - near) / max(far - near, 1e-3)
        return 1.0 + (floor - 1.0) * t


class Knife:
    """Always carried, always free, and worth more per kill than a
    gun -- which is the whole reason anybody uses one."""

    PAY = 130

    def __init__(self, spec):
        self.spec = spec
        self.next_swing = 0.0

    @property
    def name(self):
        return self.spec.name

    @property
    def reach(self):
        return float(self.spec.reach)

    def can_swing(self, now):
        return now >= self.next_swing

    def swing(self, now):
        self.next_swing = now + 60.0 / max(float(self.spec.rpm), 1.0)
        return float(self.spec.damage)


def ray_hits_body(origin, direction, length, centre, radius, height):
    """Where a ray meets an upright body, or None.

    The body is a vertical capsule's bounding cylinder, which is
    close enough: the difference between a cylinder and a capsule
    at these sizes is a few centimetres at the shoulders, and no
    player has ever noticed one.

    Returns (distance along the ray, height up the body as 0..1).
    """
    # Solve in plan: a circle against a 2D ray.
    ox, oz = origin.x - centre.x, origin.z - centre.z
    dx, dz = direction.x, direction.z
    a = dx * dx + dz * dz
    if a < 1e-9:
        return None
    b = 2.0 * (ox * dx + oz * dz)
    c = ox * ox + oz * oz - radius * radius
    disc = b * b - 4.0 * a * c
    if disc < 0.0:
        return None
    root = math.sqrt(disc)
    t = (-b - root) / (2.0 * a)
    if t < 0.0:
        t = (-b + root) / (2.0 * a)
    if t < 0.0 or t > length:
        return None
    y = origin.y + direction.y * t
    rel = (y - centre.y) / max(height, 1e-3)
    if rel < 0.0 or rel > 1.0:
        return None
    return t, rel


# Where the head starts, as a fraction of the body's height. The
# dump gives a head MULTIPLIER per gun but not where a head is; a
# shade above four fifths is where one is on a standing figure.
HEAD_FROM = 0.82


def resolve_shot(world, zombies, gun, origin, direction, moving, aiming):
    """One trigger pull: every pellet, every body it goes through.

    The world is traced first so that a wall stops a bullet, and
    each pellet then passes through bodies in order until it has
    used up the gun's pierce. Damage falls off with distance and
    doubles-ish on a head, by the gun's own multiplier -- a
    springfield rewards the shot a shotgun does not.

    Returns [(zombie, damage, headshot)], one entry per body hit,
    which the caller turns into points and corpses.
    """
    hits = []
    for ray in gun.rays(origin, direction, moving, aiming):
        far = origin + ray * float(gun.spec.range) * 1.5
        stop = (far - origin).length()
        wall = world.trace(origin, far, 0xffffffff)
        if wall.get("hit"):
            p = wall["position"]
            stop = min(stop, (wr.Vec3(p.x, p.y, p.z) - origin).length())

        along = []
        for z in zombies:
            if z.dead:
                continue
            h = z.agent.height
            base = z.position
            got = ray_hits_body(origin, ray, stop, base, z.agent.radius, h)
            if got is not None:
                along.append((got[0], got[1], z))
        along.sort(key=lambda e: e[0])

        pierce = max(1, int(gun.spec.get("pierce", 1)))
        # A round that has gone through somebody has less left for
        # whoever is behind them.
        carry = 1.0
        for dist, rel, z in along[:pierce]:
            head = rel >= HEAD_FROM
            mult = float(gun.spec.get("head", 1.0)) if head else 1.0
            dmg = gun.damage * gun.falloff_at(dist) * mult * carry
            hits.append((z, dmg, head))
            carry *= 0.75
    return hits
