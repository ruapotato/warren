"""ROTGRAVE -- the rounds, and who comes out of what.

The forty rounds in the dump are SAMPLED, not formulas: crowd size,
spawn interval, alive cap, the special, and the exact order the
species arrive in, written out by the code that was running the
tuned game. Nothing here recomputes any of it. Past forty the last
round's shape repeats, which is the honest thing to do -- forty was
where the tuning stopped and the run is long over by then.

WHAT A ROUND ACTUALLY IS: a queue and a tap. The queue is the
round's crowd list; the tap lets one out every `gap` seconds while
fewer than `cap` are alive. The round ends when the queue is empty
and the last of them is down.

Spawn points are the plan's forty-six, filtered to the zones that
are open -- which is what makes buying ground a decision rather
than a purchase. Open the motel and you have bought yourself four
more windows to watch.
"""

import os
import random

import warren as wr

_TRACE = bool(os.environ.get("ROTGRAVE_TRACE_UNDEAD"))

from .undead import Zombie, pick_target


class Director:
    """Runs the rounds."""

    # How long between a round ending and the next starting. Long
    # enough to board a window and buy something, short enough that
    # nobody stands around.
    BREATH = 9.0
    # A body that cannot reach anybody for this long is a body stuck
    # somewhere the bake did not expect. Removed rather than left,
    # because a round that will not end is worse than a body that
    # vanished.
    LOST_AFTER = 25.0

    def __init__(self, design, town, bodies, parent, seed=1, sound=None):
        self.design = design
        self.town = town
        self.bodies = bodies
        self.parent = parent
        self.sound = sound
        self.rng = random.Random(seed)

        self.round_no = 0
        self.queue = []
        self.alive = []
        self.next_spawn = 0.0
        self.resting_until = 0.0
        self.kills = 0
        self.points = 0
        self._stuck_since = {}
        self._players = []
        # Only the spawn points a body can actually stand on. Asked
        # once here rather than guessed at every spawn.
        good, bad = town.usable_spawns()
        self.spawn_points = [(s, on) for s, on in good]
        if bad:
            wr.warn(f"{len(bad)} of {len(design.spawns)} spawn points are "
                    f"not on the navmesh and will never fire: "
                    + ", ".join(sorted({s.zone for s, _ in bad})))

    # ------------------------------------------------------ the round

    @property
    def round(self):
        return self.design.round_at(max(1, self.round_no))

    @property
    def between_rounds(self):
        return not self.queue and not self.alive

    def begin(self, now, number=None):
        self.round_no = number if number is not None else self.round_no + 1
        r = self.design.round_at(self.round_no)
        self.queue = list(r.crowd)
        self.next_spawn = now + 1.5
        if self.sound:
            self.sound.flat("round_final" if r.boss else "round_start",
                            volume=0.85)
        wr.log(f"round {self.round_no}: {len(self.queue)} of them, "
               f"{r.gap:.2f}s apart, at most {r.cap} at once"
               + (f" -- {r.special}" if r.special else ""))

    def update(self, dt, now, players):
        self._players = players
        if self.between_rounds:
            if self.resting_until == 0.0:
                self.resting_until = now + (self.BREATH if self.round_no
                                            else 3.0)
            elif now >= self.resting_until:
                self.resting_until = 0.0
                self.begin(now)
            elif self.round_no and self.sound and self.resting_until - now \
                    > self.BREATH - 0.2:
                self.sound.flat("round_end", volume=0.8)
            return

        r = self.round
        if self.queue and now >= self.next_spawn and len(self.alive) < r.cap:
            if self._spawn(self.queue[0], now, players):
                self.queue.pop(0)
            self.next_spawn = now + r.gap

        self._drive(dt, now, players)

    # ------------------------------------------------------ spawning

    def _spawn_points(self, players):
        """The plan's spawn points, in open zones, preferring the
        ones near somebody.

        Near, but not in the room: a body that appears at the window
        you are standing at is a cheap shock and an unfair one. The
        nearest few are dropped and the choice is made from what is
        left, so the crowd comes from the edges of where you are
        rather than from your feet.
        """
        usable = [(s, on) for s, on in self.spawn_points
                  if self.town.is_open(s.zone)]
        if not usable or not players:
            return usable
        here = players[0].position

        def near(pair):
            on = pair[1]
            dx, dz = on.x - here.x, on.z - here.z
            return dx * dx + dz * dz

        usable.sort(key=near)
        # DROP WHAT IS UNDER YOUR FEET, BY DISTANCE, NOT BY COUNT.
        #
        # A body that appears at the window you are standing at is a
        # cheap shock and an unfair one, so the nearest few metres
        # are off limits. Expressing that as "drop the closest two"
        # was wrong in the one case that matters: at the start only
        # seven points are open, the two nearest are the crossroads'
        # own, and dropping them sent every body on a fifty-metre
        # walk from the motel. The first round was four minutes of
        # nothing.
        #
        # By metres it does the right thing at both ends, and if
        # that leaves nothing at all the nearest is used anyway --
        # a close spawn beats a round that never arrives.
        far_enough = [p for p in usable if near(p) > 8.0 * 8.0]
        return (far_enough or usable)[:10]

    def _spawn(self, species_key, now, players):
        points = self._spawn_points(players)
        if not points:
            return False
        s, on = self.rng.choice(points)
        if _TRACE:
            wr.log(f"    spawn {species_key} from {s.zone} ({s.kind}) at "
                   f"({on.x:.1f},{on.y:.1f},{on.z:.1f})")
        z = Zombie(self.design, self.bodies, species_key, self.round_no,
                   on, self.parent, self.rng)
        z.sound = self.sound
        if self.sound:
            self.sound.at("hound_spawn" if z.species.kind == "scene"
                          else "undead_spot", on, volume=0.6)
        self.alive.append(z)
        self._stuck_since[id(z)] = now
        return True

    # ------------------------------------------------------- driving

    def _drive(self, dt, now, players):
        still = []
        for z in self.alive:
            if z.dead:
                z.free()
                continue
            target = pick_target(z, players, now)
            hit = z.update(dt, target.position if target else None, now)
            if hit and target is not None:
                target.wound(hit)

            # Stuck, or unable to reach anybody at all.
            if z.agent.path_partial() or z.agent.stuck_time() > 2.0:
                if now - self._stuck_since.get(id(z), now) > self.LOST_AFTER:
                    wr.warn(f"round {self.round_no}: a {z.species.key} could "
                            f"not reach anybody; removing it")
                    z.free()
                    continue
            else:
                self._stuck_since[id(z)] = now
            still.append(z)
        self.alive = still

    # --------------------------------------------------------- score

    def killed(self, zombie, points):
        self.kills += 1
        self.points += points

    def report(self):
        line = (f"round {self.round_no}  "
                f"{len(self.alive)} up, {len(self.queue)} to come  "
                f"{self.points} points, {self.kills} killed")
        if _TRACE and self.alive:
            for z in self.alive[:4]:
                p = z.position
                line += (f"\n      {z.species.key} at "
                         f"({p.x:.1f},{p.y:.1f},{p.z:.1f}) "
                         f"v={z.agent.velocity().length():.2f} "
                         f"partial={z.agent.path_partial()} "
                         f"stuck={z.agent.stuck_time():.1f} "
                         f"target={z.agent.has_target()}")
                if self._players:
                    tp = self._players[0].position
                    ok = self.town.region.is_reachable(p, tp)
                    n = len(self.town.region.find_path(p, tp))
                    line += (f" | to player ({tp.x:.1f},{tp.z:.1f}) "
                             f"reach={ok} path={n}")
        return line
