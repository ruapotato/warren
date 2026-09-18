"""ROTGRAVE -- what the screen tells you.

A CROSSHAIR IS NOT DECORATION. Without one a player cannot tell
where a shot went, and the first thing they conclude is that the
gun does not work -- which is exactly what happened the first time
this was played. Four ticks round a gap, so the centre stays clear
and the reticle does not hide what it is aimed at, and a dot in
the middle because at a distance the ticks alone are ambiguous.

The hit marker is the other half of the same argument. A body that
takes a round and keeps walking has to say so, or every shot that
does not kill reads as a miss.
"""

import warren as wr


def _rect(parent, colour, w, h, dx, dy):
    """A box anchored to the middle of the screen, offset from it."""
    r = wr.ColorRect()
    r.colour = colour
    r.anchor_left = r.anchor_right = 0.5
    r.anchor_top = r.anchor_bottom = 0.5
    r.offset_left = dx - w * 0.5
    r.offset_right = dx + w * 0.5
    r.offset_top = dy - h * 0.5
    r.offset_bottom = dy + h * 0.5
    parent.add_child(r)
    return r


def _label(parent, x, y, w=320.0, align=0):
    t = wr.Label()
    t.anchor_left = t.anchor_right = 0.0 if x >= 0 else 1.0
    t.anchor_top = t.anchor_bottom = 0.0 if y >= 0 else 1.0
    t.offset_left = x if x >= 0 else x - w
    t.offset_right = t.offset_left + w
    t.offset_top = y if y >= 0 else y - 22.0
    t.offset_bottom = t.offset_top + 22.0
    t.use_theme_colour = False
    t.align = align
    parent.add_child(t)
    return t


class Hud:
    GAP = 7.0        # clear space either side of the centre
    TICK = 9.0       # how long each tick is
    THICK = 2.0

    WHITE = wr.Color(0.92, 0.94, 0.96, 0.85)
    HIT = wr.Color(1.0, 0.35, 0.25, 1.0)
    LOW = wr.Color(0.85, 0.20, 0.16, 1.0)

    def __init__(self, parent):
        self.root = wr.Control()
        self.root.anchor_right = 1.0
        self.root.anchor_bottom = 1.0
        parent.add_child(self.root)

        g, t, k = self.GAP, self.TICK, self.THICK
        self.ticks = [
            _rect(self.root, self.WHITE, k, t, 0.0, -(g + t * 0.5)),
            _rect(self.root, self.WHITE, k, t, 0.0, (g + t * 0.5)),
            _rect(self.root, self.WHITE, t, k, -(g + t * 0.5), 0.0),
            _rect(self.root, self.WHITE, t, k, (g + t * 0.5), 0.0),
        ]
        self.dot = _rect(self.root, self.WHITE, 2.0, 2.0, 0.0, 0.0)

        # The hit marker: the same four ticks, further out and
        # turned, shown for a moment.
        self.marks = [
            _rect(self.root, self.HIT, k, t * 0.8, 0.0, -(g + t * 1.7)),
            _rect(self.root, self.HIT, k, t * 0.8, 0.0, (g + t * 1.7)),
            _rect(self.root, self.HIT, t * 0.8, k, -(g + t * 1.7), 0.0),
            _rect(self.root, self.HIT, t * 0.8, k, (g + t * 1.7), 0.0),
        ]
        for m in self.marks:
            m.visible = False
        self._mark_until = 0.0

        # A red edge when hurt, which reads faster than a number.
        self.harm = wr.ColorRect()
        self.harm.colour = wr.Color(0.6, 0.05, 0.05, 0.0)
        self.harm.anchor_right = 1.0
        self.harm.anchor_bottom = 1.0
        self.root.add_child(self.harm)

        self.round = _label(self.root, 24.0, 20.0)
        self.points = _label(self.root, 24.0, 44.0)
        self.health = _label(self.root, 24.0, -32.0)
        # Left-aligned in a box that ENDS near the right edge,
        # rather than right-aligned: the alignment enum is the
        # engine's and the text ran off the screen with it, and a
        # box placed where the text should start needs no enum.
        self.ammo = _label(self.root, -30.0, -32.0, w=420.0)
        self.hint = _label(self.root, 24.0, 68.0, w=520.0)

    def hit(self, now, killed=False):
        self._mark_until = now + (0.22 if killed else 0.12)
        for m in self.marks:
            m.colour = wr.Color(1.0, 0.85, 0.3, 1.0) if killed else self.HIT

    def update(self, now, player, director, hint=""):
        show = now < self._mark_until
        for m in self.marks:
            m.visible = show

        # The crosshair opens up while moving, because the shot
        # does. A reticle that lies about the spread is worse than
        # none.
        spread = 0.0
        held = player.held
        if held is not None and hasattr(held, "spec"):
            spread = float(held.spec.get("spread", 1.0))
            if player.moving:
                spread *= float(held.spec.get("moving", 1.0))
            if player.aiming:
                spread = float(held.spec.get("spread_ads", spread))
        push = self.GAP + spread * 3.0 + self.TICK * 0.5
        for i, (dx, dy) in enumerate(((0.0, -push), (0.0, push),
                                      (-push, 0.0), (push, 0.0))):
            r = self.ticks[i]
            w = self.THICK if i < 2 else self.TICK
            h = self.TICK if i < 2 else self.THICK
            r.offset_left = dx - w * 0.5
            r.offset_right = dx + w * 0.5
            r.offset_top = dy - h * 0.5
            r.offset_bottom = dy + h * 0.5

        hurt = max(0.0, 1.0 - (now - player.last_hurt) / 0.6)
        low = max(0.0, 1.0 - player.health / player.MAX_HEALTH)
        a = max(hurt * 0.45, low * 0.35)
        self.harm.colour = wr.Color(0.6, 0.05, 0.05, a)

        r = director.round
        self.round.text = (f"ROUND {director.round_no}"
                           + (f"   {len(director.alive)} up" if director.alive
                              else "   clear"))
        self.points.text = f"{player.points} points"
        self.health.text = f"{player.health:.0f}"
        self.health.colour = self.LOW if player.health < 40 else self.WHITE
        if held is None:
            self.ammo.text = "-"
        elif not hasattr(held, "mag"):
            self.ammo.text = held.name
        else:
            self.ammo.text = (f"{held.name}   {held.mag} / {held.reserve}"
                              + ("   reloading" if held.reloading else ""))
        self.hint.text = hint
