"""ROTGRAVE -- the bodies that were modelled rather than grown.

The old version's people are MakeHuman meshes: a base body morphed
per character, dressed, rigged to nineteen bones and exported by
`tools/rhfig_to_gltf.py` into `assets/figures`. They are the reason
a shambler reads as a person who used to be somebody and a box
stack does not, and getting them in was the largest single thing
missing from the port.

WHAT THIS DOES NOT DO is decide who looks like what. That is in
`undead.py`, from the species' own tags -- `shambler` carries three
and picks one per body, which is why a crowd of them is a crowd and
not a rank of identical men.

SCALE IS NOT ONE. The design gives each species a height and every
figure has a height of its own, and they disagree by up to twenty
centimetres. The figure is scaled from the second to the first, so
a species that is meant to be head and shoulders over you is, no
matter which body it drew.

IF THE FILES ARE NOT THERE the game still runs, on the built bodies
in `undead.py`, with a line in the log saying so. They are large,
and a checkout that skipped them should not be a checkout that does
not start.
"""

import json
import os

import warren as wr

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIGURES = os.path.join(HERE, "assets", "figures")


class Wardrobe:
    """Every figure on disk, loaded at most once each.

    Thirty bodies on screen is thirty instances of eleven scenes.
    The meshes inside them are shared by the resource loader; what
    is per-body is a handful of nodes and a transform.
    """

    def __init__(self, path=FIGURES):
        self.path = path
        self.index = {}
        self._scenes = {}
        self._missing = set()
        manifest = os.path.join(path, "figures.json")
        if os.path.exists(manifest):
            try:
                with open(manifest) as f:
                    self.index = json.load(f)
            except ValueError as exc:
                wr.log(f"figures: {manifest} is not readable ({exc})")

    def has(self, name):
        return os.path.exists(os.path.join(self.path, name + ".glb"))

    def height_of(self, name):
        """What the figure itself measures, or zero if nobody said.

        The converted figures carry it in the sidecar. The four
        beasts are plain glTF from elsewhere and do not, so they are
        left at whatever scale they were authored at -- which is the
        honest answer, not a guess dressed as a measurement.
        """
        return float(self.index.get(name, {}).get("height", 0.0))

    def scene(self, name):
        """The loaded scene, or None. Complains once per name."""
        if name in self._scenes:
            return self._scenes[name]
        if name in self._missing:
            return None
        full = os.path.join(self.path, name + ".glb")
        res = wr.load(full) if os.path.exists(full) else None
        if res is None:
            self._missing.add(name)
            wr.log(f"figures: no {name}.glb -- falling back to a built body")
            return None
        self._scenes[name] = res
        return res

    def make(self, name, height=0.0):
        """One body, scaled to `height` if both heights are known."""
        res = self.scene(name)
        if res is None:
            return None
        node = res.instantiate()
        if node is None:
            self._missing.add(name)
            wr.log(f"figures: {name}.glb would not instantiate")
            return None
        node.name = name
        own = self.height_of(name)
        if height > 0.1 and own > 0.1:
            node.scale = height / own
        return node
