"""ROTGRAVE -- the design, as the previous version left it.

`design/rotgrave_design.json` was dumped by the code that was running
the game, not transcribed from it. That matters more than it sounds:
the round curves in it are SAMPLED -- forty rounds of crowd size,
spawn interval, alive cap and the exact species mix, written out by
evaluating the formulas rather than by copying them. A formula copied
by hand is a formula copied wrong, and the tuning is the part of the
old version worth keeping.

So nothing in this module recomputes anything. It reads, it checks
that what it read is the shape the rest of the game expects, and it
hands back plain Python. Where the game needs a number the dump does
not contain, that number belongs in the game's own code with a reason
written next to it -- not in here pretending to be design data.
"""

import json
import os

_HERE = os.path.dirname(os.path.abspath(__file__))
_DEFAULT = os.path.join(_HERE, "..", "design", "rotgrave_design.json")


class Zone:
    """One named area of Hollow Ridge.

    `rect` in the dump is [x, z, w, h] with x and z the LOW corner,
    which is the convention the whole plan uses -- doors, spawns and
    box spots are all absolute positions in the same frame.
    """

    __slots__ = ("key", "name", "blurb", "cost", "indoor", "pay", "y",
                 "x", "z", "w", "h", "light")

    def __init__(self, key, raw):
        self.key = key
        self.name = raw["name"]
        self.blurb = raw["blurb"]
        self.cost = raw["cost"]
        self.indoor = raw["indoor"]
        self.pay = list(raw["pay"])
        self.y = float(raw.get("y", 0.0))
        self.light = raw.get("light")
        self.x, self.z, self.w, self.h = (float(v) for v in raw["rect"])

    @property
    def x1(self):
        return self.x + self.w

    @property
    def z1(self):
        return self.z + self.h

    @property
    def centre(self):
        return (self.x + self.w * 0.5, self.y, self.z + self.h * 0.5)

    def contains(self, x, z):
        return self.x <= x <= self.x1 and self.z <= z <= self.z1

    # The starting zones are the ones that cost nothing, which is the
    # dump's way of saying "you begin here".
    @property
    def free(self):
        return self.cost == 0

    def __repr__(self):
        return f"<Zone {self.key} {self.name!r} {self.w:g}x{self.h:g}>"


class Door:
    """A way between two zones, and its price.

    `kind` is what it looks like -- a door, a debris pile, an open
    gap -- and `dir` is the axis it is cut through, so a door with
    dir "x" is a hole in a wall whose normal runs along x.
    """

    __slots__ = ("a", "b", "at", "cost", "kind", "dir", "w", "h")

    def __init__(self, raw):
        self.a = raw["a"]
        self.b = raw["b"]
        self.at = tuple(float(v) for v in raw["at"])
        self.cost = raw["cost"]
        self.kind = raw["kind"]
        self.dir = raw["dir"]
        self.w = float(raw["w"])
        self.h = float(raw["h"])

    def joins(self, zone_key):
        return self.a == zone_key or self.b == zone_key

    def other(self, zone_key):
        return self.b if self.a == zone_key else self.a

    def __repr__(self):
        return f"<Door {self.a}->{self.b} {self.kind} {self.cost}>"


class Spawn:
    """Where the dead come in.

    Three kinds, and the difference is not decoration:

      window  a boarded opening in a wall. It faces a direction, it
              can be re-boarded for points, and a body pulling the
              boards off is standing still in the open facing away.
              These are the defence -- not because they stop
              anything but because they turn a crowd arriving at
              once into a queue at a place the player chose.
      hole    a gap in the ground or a broken floor. No boards, no
              facing, nothing to repair.
      grave   the churchyard. Same as a hole, and worth its own name
              because what comes out of one is worth staging
              differently.

    Only a window has a `face`; the others come up out of the ground
    and there is no direction to give them.
    """

    __slots__ = ("at", "face", "kind", "zone")

    _INWARD = {"x-": (1.0, 0.0), "x+": (-1.0, 0.0),
               "z-": (0.0, 1.0), "z+": (0.0, -1.0)}

    def __init__(self, raw):
        self.at = tuple(float(v) for v in raw["at"])
        self.face = raw.get("face")
        self.kind = raw["kind"]
        self.zone = raw["zone"]

    @property
    def boarded(self):
        return self.kind == "window"

    @property
    def inward(self):
        """The direction a body walks IN from, or nothing for the
        kinds that come up out of the ground."""
        return self._INWARD[self.face] if self.face else (0.0, 0.0)

    def __repr__(self):
        return f"<Spawn {self.kind} in {self.zone} at {self.at}>"


class Species:
    """One of the eleven.

    `weight` maps a round number to how likely this species is in the
    mix -- but the mix for each of the first forty rounds is already
    sampled in `rounds`, so the weights only matter past forty.
    """

    __slots__ = ("key", "name", "desc", "health", "damage", "speed",
                 "run_speed", "sprint_at", "radius", "reach", "swing",
                 "points", "climbs", "burns", "kind", "scene", "tags",
                 "tint", "gore", "height", "scale", "weight")

    def __init__(self, key, raw):
        self.key = key
        self.name = raw["name"]
        self.desc = raw["desc"]
        self.health = float(raw["health"])
        self.damage = float(raw["damage"])
        self.speed = float(raw["speed"])
        self.run_speed = float(raw["run_speed"])
        self.sprint_at = int(raw["sprint_at"])
        self.radius = float(raw["radius"])
        self.reach = float(raw["reach"])
        self.swing = float(raw["swing"])
        self.points = int(raw["points"])
        self.climbs = bool(raw.get("climbs", False))
        self.burns = bool(raw.get("burns", False))
        self.kind = raw["kind"]
        self.scene = raw.get("scene")
        self.tags = list(raw.get("tags", []))
        self.tint = raw["tint"]
        self.gore = raw["gore"]
        self.height = float(raw.get("height", 1.8))
        self.scale = float(raw.get("scale", 1.0))
        self.weight = {int(k): v for k, v in raw.get("weight", {}).items()}

    def speed_at(self, round_no):
        """Walk until its sprint round, run after. The crossing round
        is per species: a hellhound runs from the first, a shambler
        not until fourteen."""
        return self.run_speed if round_no >= self.sprint_at else self.speed

    def __repr__(self):
        return f"<Species {self.key} {self.name!r}>"


class Weapon:
    """A gun, or the knife.

    Kept as the raw dict plus named access, because the dump carries
    thirty-odd fields per weapon and the game uses most of them --
    copying each one into a slot here would be thirty chances to
    rename something by accident.
    """

    __slots__ = ("key", "raw")

    def __init__(self, key, raw):
        self.key = key
        self.raw = raw

    def __getattr__(self, name):
        try:
            return self.raw[name]
        except KeyError:
            raise AttributeError(f"{self.key} has no {name!r}") from None

    def get(self, name, default=None):
        return self.raw.get(name, default)

    @property
    def shots_per_second(self):
        return float(self.raw["rpm"]) / 60.0

    def __repr__(self):
        return f"<Weapon {self.key} {self.raw.get('name')!r}>"


class Round:
    """One round, already sampled.

    `crowd_1p` is the literal order the species come in, so a round
    plays out the same way it did in the version that was tuned.

    `special` carries two different vocabularies in one field, which
    is worth knowing before reading it:

      "hounds"   the PACK round -- every fifth from six, nothing but
                 hellhounds. A round type, not a species; the
                 species is "hound", singular.
      a species  the BOSS round -- every tenth, with one of that
                 species in it.
      ""         an ordinary round.

    `is_pack` and `boss` say which, so nothing downstream has to
    remember that the plural means something different.
    """

    __slots__ = ("number", "count", "count_4p", "gap", "cap", "cap_4p",
                 "special", "crowd")

    def __init__(self, raw):
        self.number = raw["round"]
        self.count = raw["count_1p"]
        self.count_4p = raw["count_4p"]
        self.gap = float(raw["gap"])
        self.cap = raw["cap_1p"]
        self.cap_4p = raw["cap_4p"]
        self.special = raw["special"]
        self.crowd = list(raw["crowd_1p"])

    # The pack round's name in the dump. Spelled out here because
    # it is the one string in the design that is a round type
    # wearing a species' clothes.
    PACK = "hounds"

    @property
    def is_pack(self):
        """Nothing but hellhounds. They ignore windows, so the round
        cannot be trained the same way -- which is the point of it."""
        return self.special == self.PACK

    @property
    def boss(self):
        """The species of the one big thing, or None."""
        return None if (not self.special or self.is_pack) else self.special

    def __repr__(self):
        return f"<Round {self.number} x{self.count}{' ' + self.special if self.special else ''}>"


class Design:
    """The whole dump, checked and indexed."""

    def __init__(self, path=None):
        with open(path or _DEFAULT) as f:
            raw = json.load(f)
        self.raw = raw

        plan = raw["plan"]
        self.street_w = float(plan["street_w"])
        self.alley_w = float(plan["alley_w"])
        self.floor_h = float(plan["floor_h"])
        self.wall_h = float(plan["wall_h"])
        self.roof_parapet = float(plan["roof_parapet"])
        self.sewer_y = float(plan["sewer_y"])
        self.extent = float(plan["extent"])

        self.zones = {k: Zone(k, v) for k, v in plan["zones"].items()}
        self.doors = [Door(d) for d in plan["doors"]]
        self.spawns = [Spawn(s) for s in plan["spawns"]]
        self.box_spots = [(tuple(float(v) for v in b["at"]), b["zone"])
                          for b in plan["box_spots"]]
        self.traps = list(plan["traps"])
        self.perks = list(plan["perks"])
        self.power = plan["power"]
        self.pap = plan["pap"]

        self.levels = self._resolve_levels()

        self.undead = {k: Species(k, v) for k, v in raw["undead"].items()}
        self.rounds = [Round(r) for r in raw["rounds"]]

        w = raw["weapons"]
        self.weapons = {k: Weapon(k, v) for k, v in w["list"].items()}
        self.melee = Weapon("knife", w["melee"])
        self.box_weights = dict(w["box_weights"])
        self.upgrade_cost = int(w["upgrade_cost"])
        self.upgrade_names = dict(w["upgrade_names"])

        self._check()

    # ------------------------------------------------------- heights
    #
    # THE ROOFS ARE WHERE THE DOORS SAY, NOT WHERE THE ZONES DO.
    #
    # A roof zone carries a `y` -- 3.6 for the east roofs, 4.0 for
    # the motel -- and the doors that reach those roofs disagree with
    # it: the diner's ladder rises 7.0, the motel's 7.4, and the two
    # planks between roofs sit at 7.0 and 7.2. Seven metres is two
    # storeys of `floor_h`, which is what "two storeys of doors that
    # do not lock" describes, and 3.6 is one.
    #
    # The doors are the ones to believe. They agree with each other
    # in six places and with the prose; the zone `y` agrees with
    # nothing and is most likely a storey COUNT that was written into
    # a height field. Believing it would put the motel roof one floor
    # below its own ladder.
    #
    # Underground the two sources agree to within a few centimetres,
    # and there the zone wins -- a crypt with its floor at -6.7
    # because that is where a hatch worked out to is a crypt whose
    # floor does not match the drain it opens onto.

    def _resolve_levels(self):
        floor = {k: z.y for k, z in self.zones.items()}
        roof = {k: None for k in self.zones}

        def observe(key, height):
            nominal = self.zones[key].y
            # Close to the zone's own height: it IS the zone's floor,
            # and the zone's number is the tidier one.
            if abs(height - nominal) < 0.75:
                return
            # Well above it: a roof on top of that zone. Well below,
            # or above but on a zone that is itself a roof: the
            # zone's floor was wrong and this is it.
            if self.zones[key].indoor and height > nominal + 2.0:
                roof[key] = max(roof[key] or height, height)
            else:
                # Several doors can name the same roof at slightly
                # different heights -- a ladder to 7.4 and a plank at
                # 7.2. Take the highest: a surface below a way onto
                # it is a way onto thin air.
                floor[key] = max(floor[key], height) if floor[key] > nominal \
                    else height

        # A ROOF IS ON WHATEVER IS UNDER IT. The fire escape from
        # the east roofs down into the clinic has its top end at
        # (38, 7, -4), which is outside the east_roof rectangle and
        # inside the clinic's. The plan names two roof zones and
        # implies a third: the clinic is part of the roof circuit
        # because a door says you can get onto it.
        #
        # So a high door end that lands over a building, rather than
        # over the roof zone it is attributed to, gives THAT
        # building a roof. Without it the link dangles in mid-air,
        # attaches to nothing, and the only symptom is that nothing
        # ever uses the fire escape.
        def owner_of(x, z, height):
            best = None
            for key, zone in self.zones.items():
                if not zone.contains(x, z):
                    continue
                if zone.y > height - 1.5:
                    continue
                if best is None or zone.y > self.zones[best].y:
                    best = key
            return best

        for d in self.doors:
            if d.dir == "y":
                a_at = d.at[1]
                # The rise is unsigned; which way it goes is decided
                # by which end is nearer the far zone's own height.
                up, down = a_at + d.h, a_at - d.h
                nb = self.zones[d.b].y
                b_at = up if abs(up - nb) <= abs(down - nb) else down
                observe(d.a, a_at)
                observe(d.b, b_at)
                # Whichever end is higher, if the zone it belongs to
                # does not cover that spot, the building underneath
                # has a roof there.
                hx, hz = d.at[0], d.at[2]
                high_key, high_y = ((d.a, a_at) if a_at >= b_at
                                    else (d.b, b_at))
                if high_y > 2.0 and not self.zones[high_key].contains(hx, hz):
                    under = owner_of(hx, hz, high_y)
                    if under is not None and self.zones[under].indoor:
                        roof[under] = max(roof[under] or high_y, high_y)
            elif d.at[1] > 2.0:
                # A plank or a walkway between two things at height:
                # both ends are up there.
                observe(d.a, d.at[1])
                observe(d.b, d.at[1])

        # FLUSH THRESHOLDS. A building whose floor sits a little
        # above the ground outside its door has a step in the
        # doorway, and the bake closes a doorway with a step of more
        # than one cell in it -- see docs/known-issues.md. The
        # church, 0.3 m above its own churchyard, is the one place
        # in the plan where this bites, and the result is a church
        # nothing can walk into.
        #
        # Snapping the inside to the outside costs a third of a
        # metre of level design and buys a door that works. It is a
        # workaround and is labelled as one; when the bake is fixed
        # this can go.
        for key, zone in self.zones.items():
            if not zone.indoor:
                continue
            outside = None
            for other in self.zones.values():
                if other.indoor or other is zone:
                    continue
                if not (other.x <= zone.x and other.x1 >= zone.x1 and
                        other.z <= zone.z and other.z1 >= zone.z1):
                    continue
                if abs(floor[other.key] - floor[key]) > 0.5:
                    continue
                if outside is None or floor[other.key] > outside:
                    outside = floor[other.key]
            if outside is not None:
                floor[key] = outside

        return {k: {"floor": floor[k], "roof": roof[k]} for k in self.zones}

    def floor_of(self, zone_key):
        """The height a body stands at in this zone."""
        return self.levels[zone_key]["floor"]

    def roof_of(self, zone_key):
        """The height of this building's walkable roof, or None."""
        return self.levels[zone_key]["roof"]

    def door_ends(self, door):
        """The two world positions a door joins, as (a, b).

        For a hole in a wall both are the same point at the wall.
        For a ladder, a hatch or a plank they are the two ends of
        the climb, which is what an off-mesh link needs.
        """
        x, y, z = door.at
        if door.dir != "y":
            return ((x, y, z), (x, y, z))
        # WHICH WAY THE CLIMB GOES. The rise is unsigned, so both
        # ends are candidates; the right one is whichever lands
        # nearest a surface the far zone actually has.
        #
        # Asking "is it nearer the floor or the roof" separately gets
        # this wrong for the fire escape, where the near end is
        # ALREADY on the far zone's roof and the answer is its floor:
        # measuring against the roof then picks the candidate seven
        # metres above it. Measuring against the nearest surface of
        # either kind has no such case.
        surfaces = [v for v in (self.levels[door.b]["floor"],
                                self.levels[door.b]["roof"]) if v is not None]
        def miss(candidate):
            return min(abs(candidate - s) for s in surfaces)
        up, down = y + door.h, y - door.h
        far = up if miss(up) <= miss(down) else down
        # SNAPPED TO THE SURFACE AT EACH END. The rises in the dump
        # are authored numbers and land within a few centimetres of
        # the floors they connect -- a hatch that works out to -6.7
        # onto a crypt floor at -6.4, a ladder ending 0.2 above its
        # roof. An off-mesh link whose end is not ON the mesh is a
        # link nothing uses, and the failure is silent.
        return ((x, self._snap(door.a, y), z),
                (x, self._snap(door.b, far), z))

    def _snap(self, zone_key, height):
        best = height
        for candidate in (self.levels[zone_key]["floor"],
                          self.levels[zone_key]["roof"]):
            if candidate is None:
                continue
            if abs(candidate - height) < abs(best - height) or best == height:
                if abs(candidate - height) < 0.75:
                    best = candidate
        return best

    # ---------------------------------------------------------- checks
    #
    # Not validation for its own sake. Each of these is a rule the
    # design states in prose, and a rule the rest of the game assumes
    # without re-checking. If the dump ever stops satisfying one, the
    # failure without this is a subtle one a long way from the cause.

    def _check(self):
        problems = []

        # "No room in the town has one door." A dead end is where a
        # run ends -- not because it was hard but because it was over
        # the moment you walked in.
        for key in self.zones:
            n = sum(1 for d in self.doors if d.joins(key))
            if n < 2:
                problems.append(f"zone {key!r} has {n} door(s); every zone "
                                f"must close a loop")

        # Doors must join zones that exist.
        for d in self.doors:
            for end in (d.a, d.b):
                if end not in self.zones:
                    problems.append(f"door {d.a}->{d.b} names unknown zone "
                                    f"{end!r}")

        # Spawns and fixtures must name zones that exist.
        for s in self.spawns:
            if s.zone not in self.zones:
                problems.append(f"spawn names unknown zone {s.zone!r}")
            # A window with no facing cannot be boarded, drawn or
            # approached, and the dump has no business containing one.
            if s.boarded and s.face not in Spawn._INWARD:
                problems.append(f"window spawn in {s.zone!r} faces "
                                f"{s.face!r}, which is not a direction")
        for _, zone in self.box_spots:
            if zone not in self.zones:
                problems.append(f"box spot names unknown zone {zone!r}")

        # Every species the rounds ask for must exist, or a round will
        # fail forty minutes into a run.
        named = set()
        for r in self.rounds:
            named.update(r.crowd)
            if r.boss:
                named.add(r.boss)
        for key in sorted(named):
            if key not in self.undead:
                problems.append(f"rounds ask for unknown species {key!r}")

        # A pack round is nothing but hellhounds, which is the whole
        # reason it exists -- if its crowd has anything else in it,
        # the round is an ordinary round wearing the wrong label.
        for r in self.rounds:
            if r.is_pack and set(r.crowd) != {"hound"}:
                problems.append(f"round {r.number} says it is the pack but "
                                f"its crowd is {sorted(set(r.crowd))}")

        # Every weapon the box can give must exist.
        for key in self.box_weights:
            if key not in self.weapons:
                problems.append(f"the box can roll unknown weapon {key!r}")

        if problems:
            raise ValueError("the design dump is not the shape the game "
                             "expects:\n  " + "\n  ".join(problems))

    # ------------------------------------------------------- accessors

    def zone_at(self, x, z, y=None):
        """The zone containing a point. Several zones overlap in plan
        -- a roof over a courtyard, the drain under the whole town --
        so a height picks between them when one is given."""
        best, best_dy = None, None
        for zone in self.zones.values():
            if not zone.contains(x, z):
                continue
            if y is None:
                if best is None or zone.indoor:
                    best = zone
                continue
            dy = abs(zone.y - y)
            if best_dy is None or dy < best_dy:
                best, best_dy = zone, dy
        return best

    def round_at(self, n):
        """Rounds past the end of the dump repeat the last one's
        shape. Forty was where the tuning stopped; past it the curves
        are the same curves and the game is already over."""
        if n <= len(self.rounds):
            return self.rounds[max(0, n - 1)]
        return self.rounds[-1]

    def doors_of(self, zone_key):
        return [d for d in self.doors if d.joins(zone_key)]

    def spawns_of(self, zone_key):
        return [s for s in self.spawns if s.zone == zone_key]

    def starting_zones(self):
        return [z for z in self.zones.values() if z.free]

    def bounds(self):
        """The town's extent in plan, and its floor and ceiling."""
        x0 = min(z.x for z in self.zones.values())
        x1 = max(z.x1 for z in self.zones.values())
        z0 = min(z.z for z in self.zones.values())
        z1 = max(z.z1 for z in self.zones.values())
        y0 = min(z.y for z in self.zones.values())
        y1 = max(z.y for z in self.zones.values()) + self.wall_h
        return (x0, y0, z0, x1, y1, z1)


_cached = None


def load(path=None):
    """The design, loaded once. Everything in it is read-only in
    practice, so one copy serves the whole game."""
    global _cached
    if _cached is None or path is not None:
        d = Design(path)
        if path is None:
            _cached = d
        return d
    return _cached
