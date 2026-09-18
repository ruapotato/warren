"""ROTGRAVE -- the noise.

Every sound is synthesised by `tools/gen_sfx.py`, which is pure
Python and numpy and knows nothing about any engine. Nothing here
is a binary nobody can edit: anything that sounds wrong is a line
in that file.

A POOL, NOT A NODE PER SHOT. A rifle at six hundred rounds a
minute is ten sounds a second, and a node created and freed for
each one is ten allocations, ten tree insertions and ten frees a
second for the whole of a firefight. A fixed ring of players
reused in order costs none of that, and the only thing it gives up
is that the eldest sound is cut off when the ring wraps -- which,
at thirty-two voices, is a sound that finished a while ago.
"""

import os
import random

import warren as wr

_HERE = os.path.dirname(os.path.abspath(__file__))
SFX_DIR = os.path.normpath(os.path.join(_HERE, "..", "assets", "sfx"))

# How many one-shots can overlap. Past this the oldest is stolen.
VOICES_3D = 28
VOICES_2D = 8


class Sound:
    """The bank, and the voices that play it."""

    def __init__(self, parent, rng=None):
        self.rng = rng or random.Random(7)
        self.clips = {}
        self.missing = set()
        self._root = wr.Node3D()
        self._root.name = "Sound"
        parent.add_child(self._root)

        self._voices = []
        for i in range(VOICES_3D):
            p = wr.AudioPlayer3D()
            p.name = f"v{i}"
            p.max_distance = 70.0
            p.reference_distance = 4.0
            self._root.add_child(p)
            self._voices.append(p)
        self._next = 0

        self._flat = []
        for i in range(VOICES_2D):
            p = wr.AudioPlayer()
            p.name = f"f{i}"
            self._root.add_child(p)
            self._flat.append(p)
        self._next_flat = 0

    def clip(self, name):
        """Loaded once and shared. A miss is reported once, not
        every time something tries to play it."""
        if name in self.clips:
            return self.clips[name]
        path = os.path.join(SFX_DIR, name + ".wav")
        c = wr.load(path) if os.path.exists(path) else None
        if c is None and name not in self.missing:
            self.missing.add(name)
            wr.warn(f"sound: no clip '{name}'")
        self.clips[name] = c
        return c

    def at(self, name, where, volume=1.0, pitch=1.0, jitter=0.06):
        """A sound in the world, at a place.

        Pitch is jittered a little by default, because a sound
        played twenty times a minute at exactly one pitch stops
        being a sound and becomes a tick.
        """
        c = self.clip(name)
        if c is None:
            return None
        v = self._voices[self._next]
        self._next = (self._next + 1) % len(self._voices)
        v.stop()
        v.clip = c
        v.position = where
        v.volume = volume
        v.pitch = pitch * (1.0 + self.rng.uniform(-jitter, jitter))
        v.play()
        return v

    def flat(self, name, volume=1.0, pitch=1.0, jitter=0.0):
        """A sound with no place: the interface, and anything that
        happens to the player rather than near them."""
        c = self.clip(name)
        if c is None:
            return None
        v = self._flat[self._next_flat]
        self._next_flat = (self._next_flat + 1) % len(self._flat)
        v.stop()
        v.clip = c
        v.volume = volume
        v.pitch = pitch * (1.0 + self.rng.uniform(-jitter, jitter))
        v.play()
        return v

    def pick(self, names, where=None, **kw):
        """One of several, chosen at random -- which is most of what
        stops a groan from being a ringtone."""
        name = self.rng.choice(names)
        return self.at(name, where, **kw) if where is not None \
            else self.flat(name, **kw)


def available():
    """Which sounds the bank actually has, for a game that wants to
    check its names rather than discover a typo in a firefight."""
    if not os.path.isdir(SFX_DIR):
        return set()
    return {f[:-4] for f in os.listdir(SFX_DIR) if f.endswith(".wav")}
