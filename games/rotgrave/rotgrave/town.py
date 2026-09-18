"""ROTGRAVE -- Hollow Ridge, built from the plan.

The town is AUTHORED and its contents are generated, which is the
rule the old version stated and the reason this file reads the way it
does. The rooms, the doors, the windows and the costs come out of
`design/rotgrave_design.json` and never vary; a player who learns
where the crossroads are has learnt something that stays learnt.
What the seed decides is furniture, rubbish and which wall is brick.

A wholly procedural town has no memory in it: every corner is a
corner you have not learnt, which is the opposite of what a round
mode is for.

WHAT THE PLAN GIVES AND WHAT IT DOES NOT. Each zone is a rectangle,
a floor height, and whether it is indoors. That is a footprint, not a
building -- the walls, the openings, the roofs and the ground between
are derived here. Two conventions in the dump are worth stating
before the code relies on them:

  A door with `dir` of "x" or "z" is a hole cut in a wall, and the
  letter is the axis of that wall's normal.

  A door with `dir` of "y" is not a hole in anything. It is a ladder,
  a hatch, a plank between two roofs -- a way up or down. Those
  become off-mesh links in the navigation, because that is exactly
  what an off-mesh link is: a way between two points that is not
  walking.
"""

import math
import random

import os

import warren as wr

_NO_PRUNE = bool(os.environ.get("ROTGRAVE_NO_PRUNE"))
_TRACE_WALLS = bool(os.environ.get("ROTGRAVE_TRACE_WALLS"))


# ---------------------------------------------------------------- paint
#
# One mesh for the whole town with a submesh per material, because
# that is one upload and a handful of draws rather than four thousand
# nodes. The slots are named so the geometry code reads as intent --
# ROAD, BRICK -- rather than as numbers.

ROAD, KERB, BRICK, PLASTER, TIMBER, CONCRETE, ROOF, METAL, GRASS = range(9)

_PAINT = {
    ROAD:     (0x33353a, 0.95, 0.0),
    KERB:     (0x63635e, 0.90, 0.0),
    BRICK:    (0x6b4a3c, 0.88, 0.0),
    PLASTER:  (0x8d8677, 0.92, 0.0),
    TIMBER:   (0x5f4b34, 0.85, 0.0),
    CONCRETE: (0x4b4d51, 0.93, 0.0),
    ROOF:     (0x2b2d30, 0.85, 0.0),
    METAL:    (0x6e727a, 0.42, 0.75),
    GRASS:    (0x36422a, 0.97, 0.0),
}


def _colour(hexrgb):
    return wr.Color(((hexrgb >> 16) & 255) / 255.0,
                    ((hexrgb >> 8) & 255) / 255.0,
                    (hexrgb & 255) / 255.0, 1.0)


def materials():
    """One Material per slot, in slot order."""
    out = []
    for slot in range(len(_PAINT)):
        rgb, rough, metal = _PAINT[slot]
        m = wr.Material()
        m.albedo = _colour(rgb)
        m.roughness = rough
        m.metallic = metal
        out.append(m)
    return out


# ------------------------------------------------------- area bits
#
# Baked once with every doorway passable; what a body may cross is a
# filter. Re-baking a town of this size whenever a door opens costs a
# third of a second, spent at the exact moment a player has committed
# to a doorway with a crowd behind them.
#
# Bit 0 is the only one the pathfinder is normally asked about: "may
# a body be here at all". A zone nobody has bought has it clear, and
# buying the zone sets it. The rest describe the GROUND, so a species
# can be kept off it -- a hellhound has no business in the storm
# drain, and saying so is a filter rather than a special case in the
# spawner.

# OPEN AND SEALED ARE BOTH BITS, and that is not redundancy.
#
# A mask of zero passes no filter at all -- `(area & include) != 0`
# is false for every include -- so a polygon marked "not open" by
# clearing bit 0 and setting nothing else vanishes from every query
# in the engine, the default ones included. Half the town then has
# no navmesh as far as anything can tell, and the symptom is that
# nothing can path anywhere, which looks nothing like "that zone is
# not bought yet".
#
# So sealed is a bit that is SET. Exactly one of OPEN and SEALED is
# on at any time, a plain query sees both, and a body that may only
# use bought ground asks for OPEN.
AREA_OPEN = 1
AREA_SEALED = 2
AREA_SEWER = 4
AREA_ROOF = 8
AREA_INDOOR = 16


class Shaft:
    """A ladder or a hatch, and the hole it makes.

    Remembered before any geometry is emitted, because every
    horizontal surface it passes through has to leave a gap -- and a
    surface does not otherwise know that something climbs through it.
    """

    __slots__ = ("x", "z", "half", "lo", "hi", "door", "a", "b")

    def __init__(self, door, a, b):
        self.door = door
        self.a, self.b = a, b
        self.x, self.z = a[0], a[2]
        self.half = max(door.w, 0.8) * 0.5
        self.lo, self.hi = min(a[1], b[1]), max(a[1], b[1])

    def rect(self):
        return (self.x - self.half, self.z - self.half,
                self.x + self.half, self.z + self.half)

    def crosses(self, y0, y1):
        # Strictly between: a shaft whose end IS this surface does
        # not need a hole in it, it needs to stand on it.
        return self.lo < y1 - 0.05 and self.hi > y0 + 0.05


class Town:
    """The town's geometry, its navigation, and which of it is open."""

    WALL_T = 0.32
    SLAB_T = 0.6
    MARGIN = 8.0
    # Headroom under the drain's ceiling and inside the crypt.
    UNDER_H = 3.0

    def __init__(self, design, seed=1):
        self.design = design
        self.rng = random.Random(seed)
        self.root = None
        self.region = None
        self.opened = set()
        self.shafts = []
        self.links = []
        self._mesh_node = None

    # ------------------------------------------------------- building

    def build(self, parent):
        d = self.design
        self.root = wr.Node3D()
        self.root.name = "HollowRidge"
        parent.add_child(self.root)

        self.region = wr.NavRegion3D()
        self.region.name = "Nav"
        # A survivor and a shambler are near enough the same size,
        # and a hellhound is lower and wider. One bake, for the
        # upright body; the hounds use the same mesh and the same
        # doorways, which is true of the town as authored.
        self.region.agent_radius = 0.4
        self.region.agent_height = 1.8
        self.region.agent_climb = 0.45
        self.region.agent_slope = 48.0
        self.region.cell_size = 0.2
        self.region.cell_height = 0.15
        self.root.add_child(self.region)

        # The shafts first: anything that emits a horizontal surface
        # has to know where the holes go before it emits it.
        self._plan_shafts()

        b = wr.MeshBuilder()
        b.uv_scale = 0.5
        self._ground(b)
        self._buildings(b)
        self._roofs(b)
        self._underground(b)
        self._bridges(b)
        self._climbs(b)
        self._perimeter(b)
        self._detail(b)

        node = wr.MeshInstance3D()
        node.name = "Shell"
        mesh = b.build()
        node.mesh = mesh
        mats = materials()
        # Indexed by SLOT, not by the order the submeshes came out
        # in: the renderer looks a submesh's material up by its
        # material_slot, so handing it a list in submesh order paints
        # the town with whatever happened to be first.
        for slot in b.get_slots():
            node.set_material(slot, mats[slot])
        self.region.add_child(node)
        self._mesh_node = node

        # COLLISION IS THE SAME MESH. The old version kept a coarser
        # one, which is the right call when the visual mesh has
        # mouldings and window frames in it. This one is boxes; a
        # second, coarser set of boxes would be the same boxes.
        wr.physics().add_mesh(mesh, wr.Transform3D(), 1)

        self._place_links()
        return self

    def bake(self):
        """Bake navigation and mark every zone.

        Called after the geometry and the links are in the tree. The
        region would do this itself on its first tick; doing it here
        means the marking below happens before anything has had a
        chance to path.
        """
        # BAKE FIRST, PRUNE SECOND. Seeding the bake means naming
        # the points before there is a navmesh to name them on, and
        # a point named blind lands on whatever happens to be there
        # -- a crate, a kerb, the roof of a shed. What survives is
        # then whatever is reachable from the top of a crate.
        #
        # With a mesh in hand, `sample_point` can find ground that
        # is actually ground, and every free zone contributes one.
        self.region.clear_seeds()
        self.region.bake()
        self.region.collect_links()
        before = self.region.poly_count()
        self.pruned = 0
        if not _NO_PRUNE:
            # SEEDED FROM EVERY ZONE, not just the free ones.
            #
            # Pruning is here to remove ground that is not a place:
            # the top of a crate, the inside of a sealed void, a
            # roof slab with no way onto it. It is NOT here to
            # decide which of the plan's eighteen places count --
            # the plan already decided that, and a seam in the bake
            # that happens to cut the church off from its own
            # doorway should not silently delete the church.
            #
            # So every zone gets a seed. What survives is everything
            # the plan named plus whatever connects to it, and what
            # goes is the accidental geometry -- which is the whole
            # point.
            seeds = [self.sample_point(z) for z in self.design.zones.values()]
            self.pruned = self.region.prune_from(seeds)
        self._mark_zones()
        return self.region.poly_count()

    def show_navmesh(self, on=True):
        """Draw the bake over the town.

        The colours are per region, which is what makes a crack
        legible: two regions meeting with a gap between them is a
        gap, and a roof that is not there at all is bare.
        """
        node = self.root.find_child("NavDebug")
        if node is None:
            node = wr.Node3D()
            node.name = "NavDebug"
            self.root.add_child(node)
            unlit = wr.Material()
            unlit.unlit = True
            for name, mesh in (("Surface", self.region.debug_surface(0.07, 0)),
                               ("Edges", self.region.debug_edges(0.09)),
                               ("Links", self.region.debug_links())):
                mi = wr.MeshInstance3D()
                mi.name = name
                mi.mesh = mesh
                mi.cast_shadows = False
                mi.set_material(0, unlit)
                node.add_child(mi)
        node.visible = on
        return node

    def coverage(self):
        """How much of each zone the bake actually reached.

        Sampled on a grid at the zone's own floor height, because a
        zone with no navmesh in it is a zone nothing can walk into
        and the symptom -- bodies that never come -- is a long way
        from the cause.
        """
        out = []
        for key, zone in sorted(self.design.zones.items()):
            y = self.design.floor_of(key)
            hit = total = 0
            step = 3.0
            x = zone.x + step * 0.5
            while x < zone.x1:
                z = zone.z + step * 0.5
                while z < zone.z1:
                    total += 1
                    want = wr.Vec3(x, y + 0.3, z)
                    got = self.region.nearest_point(want)
                    if (got - want).length() < 1.2:
                        hit += 1
                    z += step
                x += step
            out.append((key, hit, total, y))
        return out

    def door_report(self):
        """Does every door in the plan actually join its two zones?

        The plan's own rule is that no room has one door, and the
        point of that rule is that a dead end is where a run ends.
        A door that the bake did not make passable breaks the rule
        silently: the zone is still on the map, still purchasable,
        and nothing can walk through it.

        Checked one door at a time rather than by flooding from the
        start, so the answer names the door rather than the far end
        of a chain of them.
        """
        out = []
        for door in self.design.doors:
            za, zb = self.design.zones[door.a], self.design.zones[door.b]
            # A door between two zones that do not touch and is not
            # up in the air is an adjacency the PLAN asserts, not a
            # way through geometry -- the route between them runs
            # through whatever is in between. Reporting those as
            # broken would be crying wolf, and a check that cries
            # wolf is a check nobody reads.
            if not self._rects_touch(za, zb) and not self.is_bridge(door):
                continue
            a = self.sample_point(za)
            b = self.sample_point(zb)
            if not self.region.is_reachable(a, b):
                out.append(f"{door.a} -> {door.b} ({door.kind}, "
                           f"{door.dir}) does not connect")
        return out

    def link_report(self):
        """Where every ladder and hatch ends up.

        A link whose end is not on the mesh is silent -- the level
        simply has a ladder nothing uses -- so it is worth asking
        out loud rather than waiting to notice that the roofs are
        never visited.
        """
        lines = []
        for s in self.shafts:
            ok = []
            for label, p in (("low", s.a), ("high", s.b)):
                want = wr.Vec3(p[0], p[1], p[2])
                got = self.region.nearest_point(want)
                d = (got - want).length()
                ok.append(f"{label} {p[1]:.2f} -> {got.y:.2f} ({d:.2f} m)")
            lines.append(f"{s.door.a}->{s.door.b} {s.door.kind}: "
                         + "; ".join(ok))
        return lines

    def _standing_point(self, zone):
        cx, _, cz = zone.centre
        return (cx, self.design.floor_of(zone.key) + 0.5, cz)

    def sample_point(self, zone):
        """Somewhere in this zone a body could actually stand.

        A zone's centre is not it: several zones have a building
        sitting in the middle of them, and the crossroads has a
        fountain. Scanning for a spot with navmesh under it is the
        difference between "unreachable" and "I asked about a wall".
        """
        y = self.design.floor_of(zone.key)
        cx, cz = zone.x + zone.w * 0.5, zone.z + zone.h * 0.5
        # FROM THE MIDDLE OUTWARD. Scanning from a corner finds the
        # corner, and a zone's corner is the likeliest place in it
        # to be a sliver, a doorstep or the inside of a wall --
        # which then reads as "this zone is unreachable" when what
        # is unreachable is the two square metres behind a pillar.
        step = 2.0
        candidates = []
        x = zone.x + step * 0.5
        while x < zone.x1:
            z = zone.z + step * 0.5
            while z < zone.z1:
                candidates.append(((x - cx) ** 2 + (z - cz) ** 2, x, z))
                z += step
            x += step
        candidates.sort()

        best, best_d = None, 1e9
        for _, x, z in candidates:
            want = wr.Vec3(x, y + 0.3, z)
            got = self.region.nearest_point(want)
            d = (got - want).length()
            if d < best_d:
                best, best_d = got, d
            if d < 0.4:
                return got
        return best if best is not None else wr.Vec3(*self._standing_point(zone))

    # ------------------------------------------------ what climbs what

    def _plan_shafts(self):
        self.shafts = []
        for door in self.design.doors:
            if door.dir != "y":
                continue
            a, b = self.design.door_ends(door)
            self.shafts.append(Shaft(door, a, b))

    def _holes_in(self, y0, y1, x0, z0, x1, z1):
        """The shaft rectangles crossing a slab, clipped to it."""
        out = []
        for s in self.shafts:
            if not s.crosses(y0, y1):
                continue
            hx0, hz0, hx1, hz1 = s.rect()
            if hx1 <= x0 or hx0 >= x1 or hz1 <= z0 or hz0 >= z1:
                continue
            out.append((max(hx0, x0), max(hz0, z0),
                        min(hx1, x1), min(hz1, z1)))
        return out

    def _slab(self, b, x0, z0, x1, z1, y0, y1, holes=()):
        """A horizontal slab with rectangular holes left out of it.

        Split on every hole edge and emit the cells that are not in a
        hole. Any number of holes, no boolean geometry, and the
        pieces meet exactly -- which matters, because two slabs that
        nearly meet are a seam the navigation bake reads as a drop.
        """
        if x1 - x0 <= 0.01 or z1 - z0 <= 0.01:
            return
        if not holes:
            b.add_bounds(wr.AABB(wr.Vec3(x0, y0, z0), wr.Vec3(x1, y1, z1)))
            return
        xs = sorted({x0, x1} | {v for h in (holes) for v in (h[0], h[2])
                                if x0 < v < x1})
        zs = sorted({z0, z1} | {v for h in (holes) for v in (h[1], h[3])
                                if z0 < v < z1})
        for i in range(len(xs) - 1):
            for j in range(len(zs) - 1):
                cx0, cx1, cz0, cz1 = xs[i], xs[i + 1], zs[j], zs[j + 1]
                mx, mz = (cx0 + cx1) * 0.5, (cz0 + cz1) * 0.5
                if any(h[0] <= mx <= h[2] and h[1] <= mz <= h[3]
                       for h in holes):
                    continue
                b.add_bounds(wr.AABB(wr.Vec3(cx0, y0, cz0),
                                     wr.Vec3(cx1, y1, cz1)))

    # ------------------------------------------------------ the ground

    def _ground(self, b):
        d = self.design
        x0, _, z0, x1, _, z1 = d.bounds()
        m = self.MARGIN

        # One slab under everything, with the drain's shafts punched
        # through it. Streets and grass are painted on top rather
        # than butted against it: two slabs meeting exactly edge to
        # edge is a seam the bake reads as a drop.
        b.slot = ROAD
        self._slab(b, x0 - m, z0 - m, x1 + m, z1 + m, -self.SLAB_T, 0.0,
                   self._holes_in(-self.SLAB_T, 0.0, x0 - m, z0 - m,
                                  x1 + m, z1 + m))

        for zone in d.zones.values():
            y = d.floor_of(zone.key)
            if zone.indoor or y <= 0.01 or y > 2.0:
                continue
            b.slot = GRASS
            self._slab(b, zone.x, zone.z, zone.x1, zone.z1, 0.0, y,
                       self._holes_in(0.0, y, zone.x, zone.z, zone.x1,
                                      zone.z1))
            self._kerb_steps(b, zone, y)

        # Pavement in front of every building: the one piece of
        # street furniture that changes how the place plays, because
        # it is where a player runs and where a crowd bunches.
        b.slot = KERB
        for zone in d.zones.values():
            if not zone.indoor or d.floor_of(zone.key) < 0.0:
                continue
            pad = 1.6
            b.add_bounds(wr.AABB(wr.Vec3(zone.x - pad, 0.0, zone.z - pad),
                                 wr.Vec3(zone.x1 + pad, 0.12, zone.z1 + pad)))

    # A TREAD HAS TO BE WIDE ENOUGH TO STAND ON, and the number is
    # not 0.45 m. The bake insets every walkable surface by a cell
    # plus the body's radius, from both sides, so a tread under
    # about 1.6 m leaves nothing in the middle -- the flight bakes
    # as a cliff, and a 0.6 m cliff is above the climb height, so
    # the churchyard becomes a place nothing can walk into.
    TREAD = 1.9
    RISER = 0.3

    def _kerb_steps(self, b, zone, rise):
        if rise <= 0.01:
            return
        steps = max(1, int(math.ceil(rise / self.RISER)))
        b.slot = KERB
        for i in range(steps):
            out = (steps - i) * self.TREAD
            top = rise * (i + 1) / steps
            b.add_bounds(wr.AABB(wr.Vec3(zone.x - out, 0.0, zone.z - out),
                                 wr.Vec3(zone.x1 + out, top, zone.z1 + out)))

    # -------------------------------------------------- the buildings

    def _buildings(self, b):
        d = self.design
        for zone in d.zones.values():
            if not zone.indoor or d.floor_of(zone.key) < -1.0:
                continue  # the crypt and the drain are built below
            self._building(b, zone)

    def _building(self, b, zone):
        d = self.design
        y = d.floor_of(zone.key)
        roof = d.roof_of(zone.key)
        # A BUILDING IS AS TALL AS WHAT REACHES ITS ROOF. Where a
        # plank or a ladder lands on top of one, that height is the
        # building's height -- two storeys, for the three that are
        # part of the roof circuit. The rest are one.
        top = roof if roof is not None else y + d.wall_h
        t = self.WALL_T

        b.slot = CONCRETE
        self._slab(b, zone.x - t, zone.z - t, zone.x1 + t, zone.z1 + t,
                   y - self.SLAB_T, y,
                   self._holes_in(y - self.SLAB_T, y, zone.x - t, zone.z - t,
                                  zone.x1 + t, zone.z1 + t))

        corners = [
            ((zone.x, zone.z), (zone.x1, zone.z), "z"),
            ((zone.x1, zone.z1), (zone.x, zone.z1), "z"),
            ((zone.x, zone.z1), (zone.x, zone.z), "x"),
            ((zone.x1, zone.z), (zone.x1, zone.z1), "x"),
        ]
        b.slot = BRICK
        for (ax, az), (bx, bz), axis in corners:
            self._wall(b, zone, ax, az, bx, bz, axis, y, top - y, t)

        # The roof. Where it is walkable it gets a parapet, and the
        # parapet is only waist high on purpose: a roof is a place to
        # be shot at from and a place to be cornered on, and a
        # chest-high wall does both. Tall enough to hide behind and
        # the roofs become the only sensible place to stand.
        if not self._roofed_over(zone):
            b.slot = ROOF
            self._slab(b, zone.x - t, zone.z - t, zone.x1 + t, zone.z1 + t,
                       top, top + 0.3,
                       self._holes_in(top, top + 0.3, zone.x - t, zone.z - t,
                                      zone.x1 + t, zone.z1 + t))
            if roof is not None:
                self._parapet(b, zone.x, zone.z, zone.x1, zone.z1, top + 0.3)

    def _parapet(self, b, x0, z0, x1, z1, y):
        b.slot = BRICK
        p = self.design.roof_parapet
        for a0, a1 in (((x0, z0), (x1, z0)), ((x1, z1), (x0, z1)),
                       ((x0, z1), (x0, z0)), ((x1, z0), (x1, z1))):
            b.add_wall(wr.Vec3(a0[0], y, a0[1]), wr.Vec3(a1[0], y, a1[1]),
                       p, 0.22)

    def _roofed_over(self, zone):
        """Is there a roof zone directly above this building?

        Two slabs a few centimetres apart is how a ceiling comes to
        z-fight with a floor, and how the bake finds two walkable
        surfaces where there is one roof.
        """
        d = self.design
        for other in d.zones.values():
            if other is zone or other.indoor:
                continue
            if d.floor_of(other.key) < d.floor_of(zone.key) + 1.0:
                continue
            if (other.x <= zone.x + 0.5 and other.x1 >= zone.x1 - 0.5 and
                    other.z <= zone.z + 0.5 and other.z1 >= zone.z1 - 0.5):
                return True
        return False

    def _wall(self, b, zone, ax, az, bx, bz, axis, base_y, height, t):
        """One side of a building, with its doorways left out of it.

        Rather than cutting holes, this emits the wall as the pieces
        BETWEEN the holes plus a lintel over each -- which is how a
        wall is actually built, needs no boolean geometry, and cannot
        leave behind the sliver of a face that a subtraction does.
        """
        run = math.hypot(bx - ax, bz - az)
        if run <= 0.01 or height <= 0.01:
            return
        ux, uz = (bx - ax) / run, (bz - az) / run

        cuts = self._cuts_for(zone, axis, ax, az, ux, uz, run, base_y)
        cuts.sort(key=lambda c: c[0])
        if _TRACE_WALLS and cuts:
            print(f"[wall] {zone.key} {axis} from ({ax:.1f},{az:.1f}) to "
                  f"({bx:.1f},{bz:.1f}) run={run:.1f} base={base_y:.2f} "
                  f"h={height:.2f}")
            for along, door, sill in cuts:
                print(f"       cut at {along:.2f} w={door.w} "
                      f"sill={sill:.2f} h={door.h} {door.a}->{door.b}")
        at = 0.0
        for along, door, sill in cuts:
            # WIDE ENOUGH TO GET THROUGH, whatever the plan says.
            #
            # The bake insets a walkable surface by a cell plus the
            # body's radius on each side, so an opening of 1.6 m --
            # which the two roof planks are -- leaves under half a
            # metre in the middle and closes. A door nothing can use
            # is not a door, and the roof circuit is three of them.
            half = max(door.w, self.MIN_DOOR) * 0.5
            lo, hi = max(0.0, along - half), min(run, along + half)
            head = min(sill + door.h, height)
            if lo > at:
                self._wall_piece(b, ax, az, ux, uz, at, lo, base_y, height, t)
            if sill > 0.01:
                self._wall_piece(b, ax, az, ux, uz, lo, hi, base_y, sill, t)
            if head < height:
                self._wall_piece(b, ax, az, ux, uz, lo, hi, base_y + head,
                                 height - head, t)
            at = max(at, hi)
        if at < run:
            self._wall_piece(b, ax, az, ux, uz, at, run, base_y, height, t)

    def _cuts_for(self, zone, axis, ax, az, ux, uz, run, base_y):
        """Which of this zone's doors belong to this wall.

        A DOOR ALWAYS MAKES A WAY THROUGH. Four of the doors in the
        plan sit metres from the wall they name -- a yard gate
        written at the yard's edge, a room's second exit written in
        the middle of the room. Requiring them to be on a wall
        leaves that wall solid, and the design's own rule is that no
        room has one door: a sealed room is where a run ends.

        So every door is assigned to the nearer of the two walls
        that face its axis, and placed at the point of that wall
        closest to where it was written. That can put a door a few
        metres from where it was drawn. A door a few metres out is a
        level that plays; a door that is not there is not.
        """
        cuts = []
        for door in self.design.doors_of(zone.key):
            if door.dir != axis or self.is_bridge(door):
                continue
            px, py, pz = door.at
            along = (px - ax) * ux + (pz - az) * uz
            across = (px - ax) * -uz + (pz - az) * ux
            # Is this the nearer of the two walls facing this axis?
            # The other one is the full width of the zone away.
            span = zone.w if axis == "x" else zone.h
            if abs(across) > abs(abs(across) - span):
                continue
            # Clamped into the run, with room for the opening itself
            # so a door is never half-buried in a corner.
            half = max(door.w, self.MIN_DOOR) * 0.5
            along = min(max(along, half + 0.4), run - half - 0.4)
            sill = max(base_y, py) - base_y
            cuts.append((along, door, sill))
        return cuts

    def _wall_piece(self, b, ax, az, ux, uz, t0, t1, base_y, height, t):
        if t1 - t0 <= 0.01 or height <= 0.01:
            return
        b.add_wall(wr.Vec3(ax + ux * t0, base_y, az + uz * t0),
                   wr.Vec3(ax + ux * t1, base_y, az + uz * t1), height, t)

    # ------------------------------------------------------- the roofs

    def _roofs(self, b):
        d = self.design
        for zone in d.zones.values():
            y = d.floor_of(zone.key)
            if zone.indoor or y < 1.0:
                continue
            b.slot = ROOF
            self._slab(b, zone.x, zone.z, zone.x1, zone.z1, y - 0.35, y,
                       self._holes_in(y - 0.35, y, zone.x, zone.z,
                                      zone.x1, zone.z1))
            self._parapet(b, zone.x, zone.z, zone.x1, zone.z1, y)

    # -------------------------------------------------- the underground

    def _underground(self, b):
        """The drain and the crypt.

        The drain's rectangle in the plan is its EXTENT, not its
        shape: "runs the length of the town, and comes up in three
        places" is a pair of tunnels, not a room fifty-six metres
        across. A room that size under the town would be the easiest
        place in the game to fight in, which is the opposite of what
        a shortcut should be.
        """
        d = self.design
        sewer = d.zones["sewer"]
        y = d.floor_of("sewer")
        wide = 5.0

        cx = (sewer.x + sewer.x1) * 0.5
        cz = (sewer.z + sewer.z1) * 0.5
        crypt = d.zones["crypt"]
        czc = crypt.z + crypt.h * 0.5

        arms = [
            (sewer.x, cz - wide * 0.5, sewer.x1, cz + wide * 0.5),
            (cx - wide * 0.5, sewer.z, cx + wide * 0.5, sewer.z1),
            # A spur to the crypt, so the back way is a route and not
            # a dead end with the upgrade machine at the bottom of it.
            (cx - wide * 0.5, czc - wide * 0.5, crypt.x, czc + wide * 0.5),
        ]
        # The shafts up to the street and the clinic have to come
        # down somewhere the tunnels actually are.
        for s in self.shafts:
            if s.lo > y + 0.5:
                continue
            arms.append((s.x - wide * 0.5, s.z - wide * 0.5,
                         s.x + wide * 0.5, s.z + wide * 0.5))
            arms.append((min(s.x, cx) - wide * 0.5, s.z - wide * 0.5,
                         max(s.x, cx) + wide * 0.5, s.z + wide * 0.5))

        # The crypt counts as part of the network: the spur runs
        # into it, and a wall across that join would seal the back
        # way at the one place it matters.
        rooms = arms + [(crypt.x, crypt.z, crypt.x1, crypt.z1)]
        for (x0, z0, x1, z1) in arms:
            self._tunnel(b, x0, z0, x1, z1, y, self.UNDER_H)
        self._room(b, crypt, self.UNDER_H)
        self._tunnel_walls(b, arms, rooms, y, self.UNDER_H)

    def _tunnel(self, b, x0, z0, x1, z1, y, head):
        t = self.WALL_T
        b.slot = CONCRETE
        self._slab(b, x0, z0, x1, z1, y - self.SLAB_T, y)
        self._slab(b, x0 - t, z0 - t, x1 + t, z1 + t, y + head, y + head + t,
                   self._holes_in(y + head, y + head + t, x0 - t, z0 - t,
                                  x1 + t, z1 + t))

    def _tunnel_walls(self, b, arms, rooms, y, head):
        """Wall the OUTSIDE of the drain network, and only that.

        Walling each arm on all four sides is the obvious thing and
        it bricks up every junction: where two arms cross, and where
        the spur runs into the crypt, there is a wall across the
        opening. The drain becomes four dead ends, the back way
        stops being a way, and nothing about the geometry looks
        wrong from any angle.

        So each side is split wherever another part of the network
        overlaps it, and only the pieces facing open ground are
        built.
        """
        t = self.WALL_T
        b.slot = CONCRETE
        for (x0, z0, x1, z1) in arms:
            for z in (z0, z1):
                for (a, c) in self._open_spans(x0, x1, z, rooms, True):
                    b.add_wall(wr.Vec3(a, y, z), wr.Vec3(c, y, z), head, t)
            for x in (x0, x1):
                for (a, c) in self._open_spans(z0, z1, x, rooms, False):
                    b.add_wall(wr.Vec3(x, y, a), wr.Vec3(x, y, c), head, t)

    @staticmethod
    def _open_spans(lo, hi, fixed, rooms, along_x):
        """The parts of a wall line that no other room covers."""
        eps = 0.05
        cuts = {lo, hi}
        for (rx0, rz0, rx1, rz1) in rooms:
            if along_x:
                if not (rz0 - eps < fixed < rz1 + eps):
                    continue
                a, c = rx0, rx1
            else:
                if not (rx0 - eps < fixed < rx1 + eps):
                    continue
                a, c = rz0, rz1
            for v in (a, c):
                if lo < v < hi:
                    cuts.add(v)
        order = sorted(cuts)
        out = []
        for i in range(len(order) - 1):
            a, c = order[i], order[i + 1]
            if c - a < 0.05:
                continue
            mid = (a + c) * 0.5
            covered = False
            for (rx0, rz0, rx1, rz1) in rooms:
                if along_x:
                    inside = (rx0 + eps < mid < rx1 - eps and
                              rz0 + eps < fixed < rz1 - eps)
                else:
                    inside = (rz0 + eps < mid < rz1 - eps and
                              rx0 + eps < fixed < rx1 - eps)
                if inside:
                    covered = True
                    break
            if not covered:
                out.append((a, c))
        return out

    def _room(self, b, zone, head):
        d = self.design
        y = d.floor_of(zone.key)
        t = self.WALL_T
        b.slot = CONCRETE
        self._slab(b, zone.x - t, zone.z - t, zone.x1 + t, zone.z1 + t,
                   y - self.SLAB_T, y)
        self._slab(b, zone.x - t, zone.z - t, zone.x1 + t, zone.z1 + t,
                   y + head, y + head + t,
                   self._holes_in(y + head, y + head + t, zone.x - t,
                                  zone.z - t, zone.x1 + t, zone.z1 + t))
        corners = [((zone.x, zone.z), (zone.x1, zone.z), "z"),
                   ((zone.x1, zone.z1), (zone.x, zone.z1), "z"),
                   ((zone.x, zone.z1), (zone.x, zone.z), "x"),
                   ((zone.x1, zone.z), (zone.x1, zone.z1), "x")]
        for (ax, az), (bx, bz), axis in corners:
            self._wall(b, zone, ax, az, bx, bz, axis, y, head, t)

    # ---------------------------------------------------- bridges
    #
    # A door between two zones that do not touch is not a hole in
    # anything -- it is a plank, a walkway, a gantry. The plan has
    # four, and three of them are the roof circuit: "motel roof ->
    # hardware -> east roofs -> down the diner's fire escape".
    #
    # Their width in the dump is 1.6 and 1.8 metres, which is a
    # plank a person walks and a plank the BAKE cannot walk: a body
    # of 0.4 m radius, eroded, needs about 1.6 m of clear width
    # before anything is left in the middle. They are widened here,
    # and that is a decision rather than a fix -- a plank nothing
    # can cross is scenery, and the design wants a route.

    BRIDGE_W = 2.8
    # The narrowest opening a 0.4 m body can be pathed through once
    # the bake has taken its inset off both sides.
    MIN_DOOR = 2.4

    def is_bridge(self, door):
        """Is this door a structure rather than a hole?

        Only if it is UP IN THE AIR. Two street zones with eighteen
        metres between them are joined by the ground in between and
        need nothing built; deciding "they do not touch, so build a
        bridge" lays a timber walkway down the middle of a road and,
        worse, a solid deck at head height across the next one.
        """
        if door.dir == "y" or door.at[1] < 2.0:
            return False
        za, zb = self.design.zones[door.a], self.design.zones[door.b]
        return not self._rects_touch(za, zb)

    @staticmethod
    def _rects_touch(a, b, slack=1.0):
        return (a.x - slack <= b.x1 and b.x - slack <= a.x1 and
                a.z - slack <= b.z1 and b.z - slack <= a.z1)

    def _bridges(self, b):
        d = self.design
        for door in d.doors:
            if not self.is_bridge(door):
                continue
            za, zb = d.zones[door.a], d.zones[door.b]
            ya = d.roof_of(door.a) or d.floor_of(door.a)
            yb = d.roof_of(door.b) or d.floor_of(door.b)
            # The plank's own height wins where the dump gives one:
            # it is the number the two roofs were authored against.
            y = door.at[1] if door.at[1] > 2.0 else max(ya, yb)
            p = self._nearest_on(za, door.at)
            q = self._nearest_on(zb, door.at)
            self._walkway(b, p, q, y)

    @staticmethod
    def _nearest_on(zone, at):
        x = min(max(at[0], zone.x), zone.x1)
        z = min(max(at[2], zone.z), zone.z1)
        return (x, z)

    def _walkway(self, b, p, q, y):
        dx, dz = q[0] - p[0], q[1] - p[1]
        length = math.hypot(dx, dz)
        if length < 0.2:
            return
        ux, uz = dx / length, dz / length
        # A little into each end, so the deck overlaps what it lands
        # on rather than stopping a hair short of it -- a gap of a
        # few centimetres is a drop as far as the bake is concerned.
        p = (p[0] - ux * 1.2, p[1] - uz * 1.2)
        q = (q[0] + ux * 1.2, q[1] + uz * 1.2)
        half = self.BRIDGE_W * 0.5
        nx, nz = -uz * half, ux * half

        b.slot = TIMBER
        b.add_quad(wr.Vec3(p[0] - nx, y, p[1] - nz),
                   wr.Vec3(q[0] - nx, y, q[1] - nz),
                   wr.Vec3(q[0] + nx, y, q[1] + nz),
                   wr.Vec3(p[0] + nx, y, p[1] + nz))
        # Underside, so the deck is solid to the bake rather than a
        # sheet with nothing below it.
        b.add_bounds(wr.AABB(
            wr.Vec3(min(p[0], q[0]) - half, y - 0.25, min(p[1], q[1]) - half),
            wr.Vec3(max(p[0], q[0]) + half, y, max(p[1], q[1]) + half)))
        b.slot = METAL
        for side in (-1.0, 1.0):
            b.add_wall(wr.Vec3(p[0] + nx * side, y, p[1] + nz * side),
                       wr.Vec3(q[0] + nx * side, y, q[1] + nz * side),
                       1.0, 0.08)

    # ------------------------------------------------ ladders and hatches

    def _climbs(self, b):
        """Something to look at where a shaft is.

        The link is what a body uses; this is what a player sees and
        aims at. A hatch that is an invisible hole in a floor is a
        hatch nobody finds.
        """
        for s in self.shafts:
            kind = s.door.kind
            if kind == "ladder":
                self._ladder(b, s)
            else:
                self._rim(b, s)

    def _ladder(self, b, s):
        b.slot = METAL
        x, z = s.x, s.z
        lo, hi = s.lo, s.hi
        side = s.half * 0.7
        for dx in (-side, side):
            b.add_box(wr.Vec3(x + dx, (lo + hi) * 0.5, z - s.half + 0.12),
                      wr.Vec3(0.07, hi - lo, 0.07))
        rungs = max(2, int((hi - lo) / 0.32))
        for i in range(rungs):
            y = lo + (hi - lo) * (i + 0.5) / rungs
            b.add_box(wr.Vec3(x, y, z - s.half + 0.12),
                      wr.Vec3(side * 2.0, 0.05, 0.05))

    def _rim(self, b, s):
        """A lip round a hatch or a hole, so the edge reads."""
        b.slot = METAL
        x0, z0, x1, z1 = s.rect()
        for y in (s.lo, s.hi):
            for a0, a1 in (((x0, z0), (x1, z0)), ((x1, z1), (x0, z1)),
                           ((x0, z1), (x0, z0)), ((x1, z0), (x1, z1))):
                b.add_wall(wr.Vec3(a0[0], y, a0[1]),
                           wr.Vec3(a1[0], y, a1[1]), 0.12, 0.12)

    def _place_links(self):
        """One off-mesh link per vertical door.

        Without these the town is a set of islands -- the street,
        each roof, the drain -- and nothing on a roof can plan a
        route to anything not on that roof.
        """
        self.links = []
        for s in self.shafts:
            link = wr.NavLink3D()
            link.name = f"{s.door.a}->{s.door.b}:{s.door.kind}"
            link.position = wr.Vec3(s.a[0], s.a[1], s.a[2])
            link.start = wr.Vec3()
            link.end = wr.Vec3(s.b[0] - s.a[0], s.b[1] - s.a[1],
                               s.b[2] - s.a[2])
            link.radius = max(1.0, s.half + 0.6)
            # Climbing costs more than the distance, or every route
            # in the town would rather go up a ladder than walk
            # round. A hatch is quicker than a ladder; a plain hole
            # you drop through is quickest.
            link.cost = {"ladder": 9.0, "hatch": 5.0}.get(s.door.kind, 2.0)
            self.region.add_child(link)
            self.links.append(link)

    # ----------------------------------------------------- the edge

    def _perimeter(self, b):
        """A wall round the town.

        Not scenery. Without it the navigation bake runs to the edge
        of its own volume, and a body steered at the boundary walks
        into the void at the seam.
        """
        d = self.design
        x0, _, z0, x1, _, z1 = d.bounds()
        m = self.MARGIN - 1.0
        h = d.wall_h * 1.8
        b.slot = BRICK
        ring = [(x0 - m, z0 - m), (x1 + m, z0 - m),
                (x1 + m, z1 + m), (x0 - m, z1 + m)]
        for i in range(4):
            ax, az = ring[i]
            bx, bz = ring[(i + 1) % 4]
            b.add_wall(wr.Vec3(ax, 0.0, az), wr.Vec3(bx, 0.0, bz), h, 0.8)

    # ---------------------------------------------------- the seed's job

    def _detail(self, b):
        """What the seed decides: rubbish, cars, crates. None of it
        changes where anything is, which is the rule -- the town is
        learnt and its contents are not."""
        d = self.design
        rng = self.rng
        for zone in d.zones.values():
            y = d.floor_of(zone.key)
            if zone.indoor or y < 0.0 or y > 2.0:
                continue
            n = max(2, int(zone.w * zone.h / 260.0))
            for _ in range(n):
                x = rng.uniform(zone.x + 2.0, zone.x1 - 2.0)
                z = rng.uniform(zone.z + 2.0, zone.z1 - 2.0)
                if self._occupied(x, z):
                    continue
                if rng.random() < 0.35:
                    b.slot = METAL
                    b.add_bounds(wr.AABB(wr.Vec3(x - 2.1, y, z - 0.9),
                                         wr.Vec3(x + 2.1, y + 1.45, z + 0.9)))
                else:
                    b.slot = TIMBER
                    s = rng.uniform(0.5, 0.9)
                    b.add_box(wr.Vec3(x, y + s * 0.5, z), wr.Vec3(s, s, s))

    def _occupied(self, x, z):
        pad = 2.4
        for zone in self.design.zones.values():
            if not zone.indoor:
                continue
            if (zone.x - pad <= x <= zone.x1 + pad and
                    zone.z - pad <= z <= zone.z1 + pad):
                return True
        for s in self.shafts:
            if abs(x - s.x) < s.half + 1.5 and abs(z - s.z) < s.half + 1.5:
                return True
        return False

    # ------------------------------------------------------ what is open

    def zone_box(self, zone, pad=0.0):
        """A zone's volume, tight in y.

        Tight on purpose: zones overlap in plan -- a roof over a
        courtyard, the drain under the whole town -- and only the
        height tells them apart.
        """
        y = self.design.floor_of(zone.key)
        return wr.AABB(wr.Vec3(zone.x - pad, y - 1.2, zone.z - pad),
                       wr.Vec3(zone.x1 + pad, y + 2.6, zone.z1 + pad))

    def _mark_zones(self):
        """Give every polygon its zone's bits.

        Applied outdoors first, then indoors, then the roofs, so that
        a building inside a street claims its own floor and a roof
        claims everything under it.
        """
        d = self.design
        order = ([z for z in d.zones.values()
                  if not z.indoor and d.floor_of(z.key) < 1.0] +
                 [z for z in d.zones.values() if z.indoor] +
                 [z for z in d.zones.values()
                  if not z.indoor and d.floor_of(z.key) >= 1.0])
        self.opened = set()
        for zone in order:
            bits = 0
            if d.floor_of(zone.key) < -1.0:
                bits |= AREA_SEWER
            if zone.indoor:
                bits |= AREA_INDOOR
            if not zone.indoor and d.floor_of(zone.key) >= 1.0:
                bits |= AREA_ROOF
            if zone.free:
                bits |= AREA_OPEN
                self.opened.add(zone.key)
            else:
                bits |= AREA_SEALED
            # Everything the zone is not, cleared -- a polygon claimed
            # by a later zone must not keep the earlier one's bits.
            self.region.set_area_in(self.zone_box(zone), bits, 0xffff & ~bits)

    def open_zone(self, key):
        """Buy a zone. One call, no geometry, no re-bake."""
        if key in self.opened:
            return False
        self.opened.add(key)
        self.region.set_area_in(self.zone_box(self.design.zones[key]),
                                AREA_OPEN, AREA_SEALED)
        return True

    def is_open(self, key):
        return key in self.opened

    def buyable(self):
        """The zones a player could pay for now: sealed, and next to
        somewhere already open. A door into nowhere is not an offer."""
        out = {}
        for door in self.design.doors:
            for near, far in ((door.a, door.b), (door.b, door.a)):
                if near in self.opened and far not in self.opened:
                    cost = door.cost or self.design.zones[far].cost
                    if far not in out or cost < out[far][0]:
                        out[far] = (cost, door)
        return out
