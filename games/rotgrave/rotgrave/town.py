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

import warren as wr


# ---------------------------------------------------------------- paint
#
# One mesh for the whole town with a submesh per material, because
# that is one upload and a handful of draws rather than four thousand
# nodes. The slots are named so the geometry code reads as intent --
# ROAD, BRICK -- rather than as numbers.

ROAD, KERB, BRICK, PLASTER, TIMBER, CONCRETE, ROOF, METAL, GRASS = range(9)

_PAINT = {
    ROAD:     (0x38393c, 0.95, 0.0),
    KERB:     (0x6a6a66, 0.90, 0.0),
    BRICK:    (0x6b4a3c, 0.88, 0.0),
    PLASTER:  (0x9a9384, 0.92, 0.0),
    TIMBER:   (0x6b543a, 0.85, 0.0),
    CONCRETE: (0x54565a, 0.93, 0.0),
    ROOF:     (0x2f3134, 0.85, 0.0),
    METAL:    (0x7c8087, 0.42, 0.75),
    GRASS:    (0x3c4a2e, 0.97, 0.0),
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
# filter. See NavMesh::set_area_in -- re-baking a town of this size
# whenever a door opens costs a fraction of a second, spent at the
# exact moment a player has committed to a doorway with a crowd
# behind them.

AREA_OPEN = 1      # bought, or free from the start
AREA_SEALED = 2    # behind a door nobody has paid for
AREA_SEWER = 4     # underground, so a hound can be kept out of it


class Town:
    """The town's geometry, its navigation, and which of it is open."""

    # The wall a body cannot see past, and the thickness of everything.
    WALL_T = 0.32
    # Ground slabs are thick enough that the navigation bake sees a
    # solid to stand on rather than a sheet with air under it.
    SLAB_T = 0.6
    # How far past the outermost zone the ground and the perimeter go.
    MARGIN = 8.0

    def __init__(self, design, seed=1):
        self.design = design
        self.rng = random.Random(seed)
        self.root = None
        self.region = None
        self.opened = set()
        self.links = {}          # door index -> NavLink3D
        self._mesh_node = None

    # ------------------------------------------------------- building

    def build(self, parent):
        """Put the whole town under `parent` and bake it."""
        d = self.design
        self.root = wr.Node3D()
        self.root.name = "HollowRidge"
        parent.add_child(self.root)

        self.region = wr.NavRegion3D()
        self.region.name = "Nav"
        self.region.radius = 0.4
        self.root.add_child(self.region)

        b = wr.MeshBuilder()
        b.uv_scale = 0.5
        self._ground(b)
        self._buildings(b)
        self._roofs(b)
        self._underground(b)
        self._perimeter(b)
        self._detail(b)

        node = wr.MeshInstance3D()
        node.name = "Shell"
        mesh = b.build()
        node.mesh = mesh
        # Indexed by SLOT, not by the order the submeshes came out
        # in: the renderer looks a submesh's material up by its
        # material_slot, so handing it a list in submesh order paints
        # the town with whatever happened to be first.
        mats = materials()
        for slot in b.get_slots():
            node.set_material(slot, mats[slot])
        self.region.add_child(node)
        self._mesh_node = node

        # COLLISION IS THE SAME MESH. The old version kept a coarser
        # one, which is the right call when the visual mesh has
        # mouldings and window frames in it. This one is boxes; a
        # second, coarser set of boxes would be the same boxes.
        wr.physics().add_mesh(mesh, wr.Transform3D(), 1)
        return self

    # ------------------------------------------------------ the ground

    def _ground(self, b):
        d = self.design
        x0, y0, z0, x1, y1, z1 = d.bounds()
        m = self.MARGIN

        # One slab under everything. Streets, pavements and the
        # patches of grass are painted on top of it rather than
        # butted against it: two slabs meeting exactly edge to edge
        # is a seam the navigation bake reads as a drop.
        b.slot = ROAD
        b.add_bounds(wr.AABB(wr.Vec3(x0 - m, -self.SLAB_T, z0 - m),
                             wr.Vec3(x1 + m, 0.0, z1 + m)))

        # Raised ground -- the churchyard sits a little above the road
        # it is reached from, which is what makes it read as a place
        # rather than as more street.
        for zone in d.zones.values():
            if zone.indoor or zone.y <= 0.01 or zone.y > 2.0:
                continue
            b.slot = GRASS
            b.add_bounds(wr.AABB(wr.Vec3(zone.x, 0.0, zone.z),
                                 wr.Vec3(zone.x1, zone.y, zone.z1)))
            # A step up all round it, so a body can get on. A kerb
            # taller than the climb height is a wall with grass on
            # top, and every path to the church would go the long way.
            self._kerb_steps(b, zone)

        # Pavement in front of every building, which is the one piece
        # of street furniture that changes how the place plays: it is
        # where a player runs and where a crowd bunches.
        b.slot = KERB
        for zone in d.zones.values():
            if not zone.indoor or zone.y < 0.0:
                continue
            pad = 1.6
            b.add_bounds(wr.AABB(wr.Vec3(zone.x - pad, 0.0, zone.z - pad),
                                 wr.Vec3(zone.x1 + pad, 0.12, zone.z1 + pad)))

    def _kerb_steps(self, b, zone):
        """Two shallow steps round a raised patch of ground."""
        rise = zone.y
        if rise <= 0.01:
            return
        steps = max(1, int(math.ceil(rise / 0.22)))
        b.slot = KERB
        for i in range(steps):
            out = (steps - i) * 0.45
            top = rise * (i + 1) / steps
            b.add_bounds(wr.AABB(wr.Vec3(zone.x - out, 0.0, zone.z - out),
                                 wr.Vec3(zone.x1 + out, top, zone.z1 + out)))

    # -------------------------------------------------- the buildings

    def _buildings(self, b):
        d = self.design
        for zone in d.zones.values():
            if not zone.indoor or zone.y < -1.0:
                continue  # the crypt and the drain are built below
            self._building(b, zone)

    def _building(self, b, zone):
        d = self.design
        y = zone.y
        h = d.wall_h
        t = self.WALL_T

        # The floor, with its top at the zone's height.
        b.slot = CONCRETE
        b.add_bounds(wr.AABB(wr.Vec3(zone.x - t, y - self.SLAB_T, zone.z - t),
                             wr.Vec3(zone.x1 + t, y, zone.z1 + t)))

        # Four walls, each with the openings that belong to it.
        corners = [
            ((zone.x, zone.z), (zone.x1, zone.z), "z"),   # south
            ((zone.x1, zone.z1), (zone.x, zone.z1), "z"),  # north
            ((zone.x, zone.z1), (zone.x, zone.z), "x"),   # west
            ((zone.x1, zone.z), (zone.x1, zone.z1), "x"),  # east
        ]
        b.slot = BRICK
        for (ax, az), (bx, bz), axis in corners:
            self._wall(b, zone, ax, az, bx, bz, axis, y, h, t)

        # A ceiling, which is also the roof unless a roof zone covers
        # this one -- in which case that zone's slab is the roof and
        # this would be a second one a few centimetres away, which is
        # how a ceiling comes to z-fight with a floor.
        if not self._roofed_over(zone):
            b.slot = ROOF
            b.add_bounds(wr.AABB(wr.Vec3(zone.x - t, y + h, zone.z - t),
                                 wr.Vec3(zone.x1 + t, y + h + 0.3,
                                         zone.z1 + t)))

    def _roofed_over(self, zone):
        """Is there a roof zone directly above this building?"""
        for other in self.design.zones.values():
            if other is zone or other.indoor:
                continue
            if other.y < zone.y + 1.0:
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
        leave the sliver of a face behind that a subtraction does.
        """
        run = math.hypot(bx - ax, bz - az)
        if run <= 0.01:
            return
        # Position along the run, 0..run.
        ux, uz = (bx - ax) / run, (bz - az) / run

        cuts = []
        for door in self.design.doors_of(zone.key):
            if door.dir != axis:
                continue
            px, _, pz = door.at
            # Distance from the wall's line, and how far along it.
            along = (px - ax) * ux + (pz - az) * uz
            across = abs((px - ax) * -uz + (pz - az) * ux)
            if across > t + 0.6 or along < -0.5 or along > run + 0.5:
                continue  # belongs to one of the other three sides
            cuts.append((along, door))

        cuts.sort()
        at = 0.0
        for along, door in cuts:
            half = door.w * 0.5
            lo, hi = max(0.0, along - half), min(run, along + half)
            if lo > at:
                self._wall_piece(b, ax, az, ux, uz, at, lo, base_y, height, t)
            # The lintel over the opening, which is what stops a
            # doorway being a slot all the way to the roof.
            head = min(door.h, height)
            if head < height:
                self._wall_piece(b, ax, az, ux, uz, lo, hi,
                                 base_y + head, height - head, t)
            at = max(at, hi)
        if at < run:
            self._wall_piece(b, ax, az, ux, uz, at, run, base_y, height, t)

    def _wall_piece(self, b, ax, az, ux, uz, t0, t1, base_y, height, t):
        if t1 - t0 <= 0.01 or height <= 0.01:
            return
        b.add_wall(wr.Vec3(ax + ux * t0, base_y, az + uz * t0),
                   wr.Vec3(ax + ux * t1, base_y, az + uz * t1), height, t)

    # ------------------------------------------------------- the roofs

    def _roofs(self, b):
        d = self.design
        for zone in d.zones.values():
            if zone.indoor or zone.y < 1.0:
                continue
            t = self.WALL_T
            b.slot = ROOF
            b.add_bounds(wr.AABB(wr.Vec3(zone.x, zone.y - 0.35, zone.z),
                                 wr.Vec3(zone.x1, zone.y, zone.z1)))
            # A parapet, and the reason it is only waist high: a
            # roof is a place to be shot at from and a place to be
            # cornered on, and a chest-high wall does both. Make it
            # tall enough to hide behind and the roofs become the
            # only sensible place to stand.
            b.slot = BRICK
            p = d.roof_parapet
            for (a0, a1) in (((zone.x, zone.z), (zone.x1, zone.z)),
                             ((zone.x1, zone.z1), (zone.x, zone.z1)),
                             ((zone.x, zone.z1), (zone.x, zone.z)),
                             ((zone.x1, zone.z), (zone.x1, zone.z1))):
                b.add_wall(wr.Vec3(a0[0], zone.y, a0[1]),
                           wr.Vec3(a1[0], zone.y, a1[1]), p, 0.22)

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
        y = sewer.y
        head = 3.0
        wide = 5.0

        cx = (sewer.x + sewer.x1) * 0.5
        cz = (sewer.z + sewer.z1) * 0.5
        arms = [
            (sewer.x, cz - wide * 0.5, sewer.x1, cz + wide * 0.5),
            (cx - wide * 0.5, sewer.z, cx + wide * 0.5, sewer.z1),
        ]
        # A spur to the crypt, so the back way is a route and not a
        # dead end with the upgrade machine at the bottom of it.
        crypt = d.zones["crypt"]
        arms.append((cx - wide * 0.5, crypt.z + crypt.h * 0.5 - wide * 0.5,
                     crypt.x, crypt.z + crypt.h * 0.5 + wide * 0.5))

        for (x0, z0, x1, z1) in arms:
            self._tunnel(b, x0, z0, x1, z1, y, head)
        self._room(b, crypt, head)

    def _tunnel(self, b, x0, z0, x1, z1, y, head):
        t = self.WALL_T
        b.slot = CONCRETE
        b.add_bounds(wr.AABB(wr.Vec3(x0, y - self.SLAB_T, z0),
                             wr.Vec3(x1, y, z1)))
        b.add_bounds(wr.AABB(wr.Vec3(x0 - t, y + head, z0 - t),
                             wr.Vec3(x1 + t, y + head + t, z1 + t)))
        for (a0, a1) in (((x0, z0), (x1, z0)), ((x1, z1), (x0, z1)),
                         ((x0, z1), (x0, z0)), ((x1, z0), (x1, z1))):
            b.add_wall(wr.Vec3(a0[0], y, a0[1]), wr.Vec3(a1[0], y, a1[1]),
                       head, t)

    def _room(self, b, zone, head):
        t = self.WALL_T
        b.slot = CONCRETE
        b.add_bounds(wr.AABB(wr.Vec3(zone.x - t, zone.y - self.SLAB_T,
                                     zone.z - t),
                             wr.Vec3(zone.x1 + t, zone.y, zone.z1 + t)))
        b.add_bounds(wr.AABB(wr.Vec3(zone.x - t, zone.y + head, zone.z - t),
                             wr.Vec3(zone.x1 + t, zone.y + head + t,
                                     zone.z1 + t)))
        corners = [((zone.x, zone.z), (zone.x1, zone.z), "z"),
                   ((zone.x1, zone.z1), (zone.x, zone.z1), "z"),
                   ((zone.x, zone.z1), (zone.x, zone.z), "x"),
                   ((zone.x1, zone.z), (zone.x1, zone.z1), "x")]
        for (ax, az), (bx, bz), axis in corners:
            self._wall(b, zone, ax, az, bx, bz, axis, zone.y, head, t)

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
            if zone.indoor or zone.y < 0.0:
                continue
            n = max(2, int(zone.w * zone.h / 260.0))
            for _ in range(n):
                x = rng.uniform(zone.x + 2.0, zone.x1 - 2.0)
                z = rng.uniform(zone.z + 2.0, zone.z1 - 2.0)
                if self._inside_a_building(x, z):
                    continue
                if rng.random() < 0.35:
                    b.slot = METAL
                    b.add_bounds(wr.AABB(
                        wr.Vec3(x - 2.1, zone.y, z - 0.9),
                        wr.Vec3(x + 2.1, zone.y + 1.45, z + 0.9)))
                else:
                    b.slot = TIMBER
                    s = rng.uniform(0.5, 0.9)
                    b.add_box(wr.Vec3(x, zone.y + s * 0.5, z),
                              wr.Vec3(s, s, s))

    def _inside_a_building(self, x, z):
        pad = 2.4
        for zone in self.design.zones.values():
            if not zone.indoor:
                continue
            if (zone.x - pad <= x <= zone.x1 + pad and
                    zone.z - pad <= z <= zone.z1 + pad):
                return True
        return False
