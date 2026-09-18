#!/usr/bin/env python3
"""
ROTGRAVE -- the town's surfaces.

A procedurally assembled town is a great many quads, and quads with a flat
colour on them read as a diagram of a town rather than a town. So every
material the generator can ask for is made here as a TILEABLE pair: an
albedo and a normal map derived from the same heightfield that shaped it.

TILEABLE IS THE WHOLE CONSTRAINT. The generator lays a wall by stretching
one quad and letting the UV repeat, so a texture with a seam is a town with
a stripe down every wall. Every function here works in wrapped space: the
noise wraps, the brick courses wrap, the plank ends wrap, and the normal is
taken with a wrapped gradient.

THE NORMAL IS DERIVED, NOT DRAWN. Mortar sits below brick, planks sit
proud of their gaps, a dent in corrugated iron is a dent -- all of that is
one heightfield per material, and the normal falls out of its slope. It is
the difference between a lit town and a painted one.

WHAT COMES OUT, per material, is three files: `<name>_c.png` (albedo),
`<name>_n.png` (the derived normal) and `<name>_orm.png` -- occlusion in
red, roughness in green, metalness in blue, which is glTF's packing and
what Warren's shader samples. The material's own roughness and metallic
scalars MULTIPLY the texture, so a material using these sets both to 1.0
and lets the map be the whole story.

    python3 tools/gen_world_tex.py --out assets/world
"""

import argparse
import math
from pathlib import Path

import numpy as np
from PIL import Image

N = 512


def rng(seed):
    return np.random.default_rng(seed)


def wrap_noise(size, cells, seed, octaves=4, gain=0.5):
    """Tileable value noise: the lattice wraps, so the image does."""
    out = np.zeros((size, size), np.float32)
    amp, tot, c = 1.0, 0.0, max(1, cells)
    for _ in range(octaves):
        if c > size:
            break
        g = rng(seed + c * 2654435761 % 100003).random((c, c)).astype(np.float32)
        g = np.concatenate([g, g[:1]], 0)
        g = np.concatenate([g, g[:, :1]], 1)
        t = np.linspace(0, c, size, endpoint=False, dtype=np.float32)
        i0 = np.floor(t).astype(int)
        f = t - i0
        f = f * f * (3 - 2 * f)
        gy0 = g[i0]
        gy1 = g[i0 + 1]
        a = gy0[:, i0] * (1 - f)[None, :] + gy0[:, i0 + 1] * f[None, :]
        b = gy1[:, i0] * (1 - f)[None, :] + gy1[:, i0 + 1] * f[None, :]
        out += amp * (a * (1 - f)[:, None] + b * f[:, None])
        tot += amp
        amp *= gain
        c *= 2
    return out / max(tot, 1e-6)


def ridged(size, cells, seed, octaves=4):
    return 1.0 - np.abs(wrap_noise(size, cells, seed, octaves) - 0.5) * 2.0


def worley(size, cells, seed, jitter=0.85):
    """Tileable cellular noise -- gravel, cobbles, flakes of paint."""
    g = rng(seed)
    pts = (np.stack(np.meshgrid(np.arange(cells), np.arange(cells), indexing="ij"), -1)
           .astype(np.float32) + 0.5)
    pts += (g.random((cells, cells, 2)).astype(np.float32) - 0.5) * jitter
    pts = pts.reshape(-1, 2) / cells
    u = (np.arange(size, dtype=np.float32) + 0.5) / size
    U, V = np.meshgrid(u, u, indexing="ij")
    best = np.full((size, size), 10.0, np.float32)
    second = np.full((size, size), 10.0, np.float32)
    for px, py in pts:
        dx = np.abs(U - px)
        dy = np.abs(V - py)
        dx = np.minimum(dx, 1 - dx)
        dy = np.minimum(dy, 1 - dy)
        d = np.sqrt(dx * dx + dy * dy)
        np.minimum(second, np.maximum(best, d), out=second)
        np.minimum(best, d, out=best)
    return best * cells, second * cells


def normal_from(height, strength=1.0):
    """Slope of a wrapped heightfield, as a tangent-space normal map."""
    dx = (np.roll(height, -1, 1) - np.roll(height, 1, 1)) * 0.5
    dy = (np.roll(height, -1, 0) - np.roll(height, 1, 0)) * 0.5
    nx = -dx * strength * 8.0
    ny = dy * strength * 8.0
    nz = np.ones_like(height)
    ln = np.sqrt(nx * nx + ny * ny + nz * nz)
    return np.dstack([nx / ln, ny / ln, nz / ln]).astype(np.float32)


## THE ALBEDO IS A DETAIL MAP, NOT A COLOUR.
##
## Each material is tinted by Mats.SPEC when it is built, and a texture that
## already carries its own colour is then multiplied by that tint -- brick
## at 0.4 times a tint at 0.55 is an albedo of 0.22, which at dusk is black.
## Every surface in the town came out unreadable for exactly this reason.
##
## So the albedo is normalised here to a mean luminance of TARGET, keeping
## all of its relative variation and its hue relationships and throwing away
## its absolute level. The engine-side tint then sets what colour the thing
## actually is, which is the usual way round and is the only way a material
## can be re-tinted without going dark.
TARGET = 0.74


def save_rgb(path, rgb, target=TARGET):
    rgb = np.clip(rgb, 0, 1)
    lum = float((rgb * np.array([0.2126, 0.7152, 0.0722], np.float32)).sum(-1).mean())
    if lum > 1e-4:
        rgb = np.clip(rgb * (target / lum), 0, 1)
    Image.fromarray((rgb * 255 + 0.5).astype(np.uint8), "RGB").save(path)


def save_normal(path, nrm):
    img = (np.clip(nrm * 0.5 + 0.5, 0, 1) * 255 + 0.5).astype(np.uint8)
    Image.fromarray(img, "RGB").save(path)


def save_orm(path, rough, metal=0.0, ao=None):
    """Occlusion, roughness, metalness in one image.

    glTF's packing and Warren's: the shader does `rough *= orm.g` and
    `metallic *= orm.b`, so this is not a convenience -- it is the
    only channel layout the renderer will read.

    Occlusion is left at white. These are tiling surface textures and
    the occlusion that matters in this town is between surfaces, not
    within one; a baked-in darkening would tile with the pattern and
    read as dirt in a grid.
    """
    r = np.full((N, N), 1.0, np.float32) if ao is None else np.clip(ao, 0, 1)
    g = np.clip(rough, 0, 1)
    b = np.clip(metal, 0, 1)
    if np.ndim(g) == 0:
        g = np.full((N, N), float(g), np.float32)
    if np.ndim(b) == 0:
        b = np.full((N, N), float(b), np.float32)
    img = np.dstack([r, g, b])
    Image.fromarray((img * 255 + 0.5).astype(np.uint8), "RGB").save(path)


def tint(v, colour, variation=None):
    """A value field into a colour, with an optional per-pixel hue wander."""
    c = np.array(colour, np.float32)
    out = v[..., None] * c[None, None, :]
    if variation is not None:
        out = out * (1.0 + variation[..., None] * np.array([1.0, 0.55, 0.3], np.float32))
    return out


# ------------------------------------------------------------- materials

def brick(seed, colour=(0.52, 0.26, 0.20), courses=24, mortar=(0.62, 0.60, 0.56)):
    """Running bond, every other course offset by half a brick."""
    y = np.linspace(0, courses, N, endpoint=False, dtype=np.float32)
    row = np.floor(y)
    # THE COURSES RUN ACROSS, NOT UP. `y` indexes rows of the image, so it
    # has to broadcast DOWN the first axis; left as a bare 1-D array it
    # broadcasts along the second instead and the whole bond comes out
    # rotated a quarter turn -- every brick wall in the town was stripes.
    fy = (y - row)[:, None]
    # A brick is about three times as long as it is tall.
    per_row = max(2, int(round(courses * 0.34)))
    x = np.linspace(0, per_row, N, endpoint=False, dtype=np.float32)
    X = x[None, :] + (row % 2)[:, None] * 0.5
    fx = X - np.floor(X)
    bid = np.floor(X) + row[:, None] * 977

    gap_y = 0.055
    gap_x = 0.030
    inb = ((fy > gap_y) & (fy < 1 - gap_y) & (fx > gap_x) & (fx < 1 - gap_x))
    edge = (np.clip((np.minimum(fy, 1 - fy) - gap_y) / 0.05, 0, 1)
            * np.clip((np.minimum(fx, 1 - fx) - gap_x) / 0.03, 0, 1))

    per_brick = rng(seed).random(int(bid.max()) + 2).astype(np.float32)
    shade = per_brick[bid.astype(int) % len(per_brick)]

    grain = wrap_noise(N, 96, seed + 3, 3)
    grime = wrap_noise(N, 5, seed + 9, 3)

    h = edge * (0.75 + 0.25 * shade) + 0.10 * grain * inb
    v = np.where(inb, 0.72 + 0.46 * shade, 1.0)
    v = v * (0.86 + 0.26 * grain)
    v = v * (0.82 + 0.30 * grime)

    col = np.where(inb[..., None], np.array(colour, np.float32)[None, None, :],
                   np.array(mortar, np.float32)[None, None, :])
    rgb = col * v[..., None]
    # Damp and moss creeping up from the bottom of the course pattern.
    moss = np.clip((wrap_noise(N, 7, seed + 21, 3) - 0.56) * 4, 0, 1) * (1 - inb * 0.4)
    rgb[..., 0] *= 1 - 0.30 * moss
    rgb[..., 1] *= 1 + 0.12 * moss
    rgb[..., 2] *= 1 - 0.34 * moss
    rough = 0.78 + 0.18 * (1 - inb) + 0.06 * grain
    return rgb, normal_from(h, 1.3), rough


def plaster(seed, colour=(0.74, 0.71, 0.64), crack=1.0, peel=0.6):
    base = wrap_noise(N, 8, seed, 4)
    fine = wrap_noise(N, 120, seed + 5, 2)
    h = base * 0.4 + fine * 0.1
    v = 0.86 + 0.22 * base + 0.10 * fine

    # Cracks: the ridges of a low-frequency field, thresholded hard.
    cr = np.clip((ridged(N, 9, seed + 17, 4) - 0.88) * 10, 0, 1) * crack
    h -= cr * 0.5
    v *= 1 - 0.45 * cr

    # Paint coming off in patches, showing the render under it.
    pl = np.clip((wrap_noise(N, 6, seed + 31, 3) - 0.55) * 5, 0, 1) * peel
    under = np.array([0.46, 0.42, 0.38], np.float32)
    rgb = tint(v, colour)
    rgb = rgb * (1 - pl[..., None]) + under[None, None, :] * v[..., None] * pl[..., None]
    h -= pl * 0.25
    stain = np.clip((wrap_noise(N, 4, seed + 41, 3) - 0.5) * 2.2, 0, 1)
    rgb *= (1 - 0.30 * stain)[..., None]
    return rgb, normal_from(h, 1.0), 0.86 + 0.10 * pl


def asphalt(seed):
    d1, d2 = worley(N, 26, seed, 1.0)
    grit = wrap_noise(N, 140, seed + 3, 3)
    broad = wrap_noise(N, 5, seed + 7, 3)
    h = grit * 0.35 + (1 - np.clip(d1, 0, 1)) * 0.2
    v = 0.30 + 0.34 * grit + 0.16 * broad
    # Cracks, and the tar somebody poured into them.
    cr = np.clip((ridged(N, 7, seed + 13, 4) - 0.86) * 9, 0, 1)
    v *= 1 - 0.55 * cr
    h -= cr * 0.4
    rgb = tint(v, (0.90, 0.89, 0.92))
    # The odd pale aggregate stone showing through the binder.
    stone = np.clip((grit - 0.72) * 5, 0, 1)
    rgb += stone[..., None] * 0.22
    return rgb, normal_from(h, 1.1), 0.90 - 0.15 * stone


def concrete(seed, colour=(0.66, 0.65, 0.62), wet=0.0):
    base = wrap_noise(N, 10, seed, 4)
    fine = wrap_noise(N, 160, seed + 11, 2)
    d1, _ = worley(N, 40, seed + 5, 0.9)
    pits = np.clip((0.35 - d1) * 3, 0, 1)
    h = base * 0.3 + fine * 0.12 - pits * 0.5
    v = 0.84 + 0.26 * base + 0.10 * fine - 0.25 * pits
    stain = np.clip((wrap_noise(N, 4, seed + 23, 3) - 0.48) * 2.4, 0, 1)
    v *= 1 - 0.28 * stain
    rgb = tint(v, colour)
    if wet:
        rgb *= 1 - 0.25 * wet * stain[..., None]
    return rgb, normal_from(h, 1.0), 0.88 - 0.35 * wet * stain


def planks(seed, colour=(0.44, 0.31, 0.19), n_planks=7, across=False, worn=0.6):
    u = np.linspace(0, n_planks, N, endpoint=False, dtype=np.float32)
    P, Q = np.meshgrid(u, np.linspace(0, 1, N, endpoint=False, dtype=np.float32),
                       indexing="ij" if across else "xy")
    pid = np.floor(P)
    fp = P - pid
    # Each plank its own length offset, so the ends do not line up.
    r = rng(seed)
    off = r.random(n_planks + 1).astype(np.float32)
    # ONE BUTT JOINT PER TILE AT MOST. Two made every board a quarter as
    # long as it is wide times four, which reads as brickwork in a wood
    # colour rather than as a floor.
    seg = np.floor(Q + off[pid.astype(int) % (n_planks + 1)])
    bid = pid * 131 + seg * 17

    per = r.random(int(abs(bid).max()) + 3).astype(np.float32)
    shade = per[np.abs(bid).astype(int) % len(per)]

    # Grain runs along the plank.
    grain = wrap_noise(N, 20, seed + 3, 4)
    grain = (grain if across else grain.T)
    fibre = np.sin((Q * 34 + grain * 9) * math.pi * 2) * 0.5 + 0.5

    gap = np.clip((np.minimum(fp, 1 - fp)) / 0.045, 0, 1)
    endgap = np.clip(np.abs(((Q + off[pid.astype(int) % (n_planks + 1)]) % 1.0) - 0.5)
                     / 0.485, 0, 1)
    solid = gap * np.clip(endgap * 14, 0, 1)

    h = solid * (0.6 + 0.4 * shade) + fibre * 0.12
    v = (0.66 + 0.40 * shade) * (0.84 + 0.26 * fibre)
    v = v * (0.9 + 0.2 * gap)
    scuff = np.clip((wrap_noise(N, 9, seed + 29, 3) - 0.52) * 4, 0, 1) * worn
    v *= 1 - 0.34 * scuff
    rgb = tint(v, colour)
    rgb = rgb * solid[..., None] + rgb * 0.22 * (1 - solid[..., None])
    return rgb, normal_from(h, 1.4), 0.80 + 0.15 * scuff


def corrugated(seed, colour=(0.56, 0.57, 0.58), ribs=14, rust=0.6):
    u = np.linspace(0, ribs, N, endpoint=False, dtype=np.float32)
    U, _ = np.meshgrid(u, u)
    wave = 0.5 + 0.5 * np.cos(U * 2 * math.pi)
    h = wave
    v = 0.58 + 0.52 * wave
    dirt = wrap_noise(N, 12, seed, 3)
    v *= 0.88 + 0.24 * dirt
    rgb = tint(v, colour)
    rs = np.clip((wrap_noise(N, 7, seed + 13, 4) - 0.50) * 3.0, 0, 1) * rust
    rust_col = np.array([0.42, 0.20, 0.09], np.float32)
    rgb = rgb * (1 - rs[..., None]) + rust_col[None, None, :] * (0.7 + 0.6 * dirt[..., None]) * rs[..., None]
    h -= rs * 0.15
    return rgb, normal_from(h, 1.2), 0.42 + 0.48 * rs


def tiles(seed, a=(0.80, 0.79, 0.76), b=(0.18, 0.18, 0.20), n=8):
    u = np.linspace(0, n, N, endpoint=False, dtype=np.float32)
    U, V = np.meshgrid(u, u)
    cell = (np.floor(U) + np.floor(V)) % 2
    fu = U - np.floor(U)
    fv = V - np.floor(V)
    grout = np.clip(np.minimum(np.minimum(fu, 1 - fu), np.minimum(fv, 1 - fv)) / 0.05, 0, 1)
    wearing = wrap_noise(N, 6, seed, 3)
    dirt = wrap_noise(N, 40, seed + 7, 2)
    base = np.where(cell > 0.5, 1.0, 0.0)[..., None] * np.array(a, np.float32) \
        + np.where(cell > 0.5, 0.0, 1.0)[..., None] * np.array(b, np.float32)
    v = (0.88 + 0.22 * dirt) * (0.78 + 0.35 * wearing)
    rgb = base * v[..., None]
    grout_col = np.array([0.40, 0.38, 0.35], np.float32)
    rgb = rgb * grout[..., None] + grout_col[None, None, :] * v[..., None] * (1 - grout[..., None])
    crack = np.clip((ridged(N, 11, seed + 19, 4) - 0.90) * 12, 0, 1)
    rgb *= (1 - 0.6 * crack)[..., None]
    h = grout * 0.7 - crack * 0.4
    return rgb, normal_from(h, 1.0), 0.34 + 0.45 * (1 - grout) + 0.2 * wearing


def gravel(seed, colour=(0.52, 0.50, 0.46)):
    d1, d2 = worley(N, 34, seed, 1.0)
    stones = np.clip(1.0 - d1 * 1.6, 0, 1)
    edge = np.clip((d2 - d1) * 3, 0, 1)
    per = rng(seed + 3).random((N, N)).astype(np.float32)
    shade = wrap_noise(N, 34, seed + 5, 1)
    h = stones * 0.9 + wrap_noise(N, 150, seed + 9, 2) * 0.1
    v = (0.70 + 0.28 * shade) * (0.80 + 0.30 * stones) * (0.94 + 0.12 * edge)
    rgb = tint(v, colour, (per - 0.5) * 0.10)
    return rgb, normal_from(h, 1.5), 0.86 + 0.1 * shade


def deadgrass(seed):
    blade = wrap_noise(N, 200, seed, 2)
    clump = wrap_noise(N, 9, seed + 5, 4)
    patch = wrap_noise(N, 4, seed + 11, 3)
    h = blade * 0.4 + clump * 0.4
    v = 0.56 + 0.44 * blade * (0.5 + 0.7 * clump)
    green = np.clip((patch - 0.48) * 3, 0, 1)
    dry = np.array([0.46, 0.41, 0.24], np.float32)
    alive = np.array([0.22, 0.32, 0.16], np.float32)
    col = dry[None, None, :] * (1 - green[..., None]) + alive[None, None, :] * green[..., None]
    rgb = col * v[..., None] * 1.5
    dirt = np.clip((0.40 - clump) * 3, 0, 1)
    rgb = rgb * (1 - dirt[..., None]) + np.array([0.30, 0.24, 0.18], np.float32)[None, None, :] * v[..., None] * dirt[..., None]
    return rgb, normal_from(h, 0.9), 0.92


def dirtground(seed):
    lump = wrap_noise(N, 14, seed, 4)
    fine = wrap_noise(N, 130, seed + 7, 2)
    d1, _ = worley(N, 22, seed + 3, 1.0)
    pebble = np.clip(1 - d1 * 2.2, 0, 1)
    h = lump * 0.6 + fine * 0.15 + pebble * 0.3
    v = 0.60 + 0.40 * lump + 0.16 * fine + 0.2 * pebble
    rgb = tint(v, (0.40, 0.31, 0.22))
    wetp = np.clip((wrap_noise(N, 5, seed + 23, 3) - 0.58) * 4, 0, 1)
    rgb *= (1 - 0.35 * wetp)[..., None]
    return rgb, normal_from(h, 1.2), 0.92 - 0.4 * wetp


def shingle(seed, colour=(0.26, 0.24, 0.24)):
    rows = 18
    y = np.linspace(0, rows, N, endpoint=False, dtype=np.float32)
    row = np.floor(y)
    fy = y - row
    x = np.linspace(0, rows * 0.6, N, endpoint=False, dtype=np.float32)
    X = x[None, :] + (row % 2)[:, None] * 0.5
    fx = X - np.floor(X)
    tid = np.floor(X) + row[:, None] * 313
    per = rng(seed).random(int(abs(tid).max()) + 2).astype(np.float32)
    shade = per[np.abs(tid).astype(int) % len(per)]
    # fy runs down the image and must broadcast along rows, not columns.
    fy2 = fy[:, None]
    lip = np.clip((1 - fy2) / 0.16, 0, 1)
    seam = np.clip(np.minimum(fx, 1 - fx) / 0.03, 0, 1)
    h = ((1 - fy2) * 0.5 + lip * 0.5) * seam
    v = (0.66 + 0.42 * shade) * (0.75 + 0.4 * (1 - fy2))
    grime = wrap_noise(N, 8, seed + 11, 3)
    v *= 0.86 + 0.26 * grime
    rgb = tint(v, colour)
    moss = np.clip((wrap_noise(N, 6, seed + 29, 3) - 0.60) * 5, 0, 1)
    rgb[..., 1] *= 1 + 0.5 * moss
    rgb[..., 0] *= 1 - 0.2 * moss
    return rgb, normal_from(h, 1.3), 0.88


def wallpaper(seed, colour=(0.58, 0.52, 0.42)):
    stripe = 0.5 + 0.5 * np.cos(np.linspace(0, 22, N, endpoint=False, dtype=np.float32) * 2 * math.pi)
    S = np.tile(stripe, (N, 1))
    base = wrap_noise(N, 90, seed, 2)
    v = (0.88 + 0.16 * S) * (0.9 + 0.2 * base)
    rgb = tint(v, colour)
    # Damp, and paper hanging off in sheets.
    damp = np.clip((wrap_noise(N, 5, seed + 13, 3) - 0.45) * 3, 0, 1)
    rgb[..., 0] *= 1 - 0.30 * damp
    rgb[..., 1] *= 1 - 0.38 * damp
    rgb[..., 2] *= 1 - 0.45 * damp
    tear = np.clip((wrap_noise(N, 7, seed + 31, 4) - 0.62) * 6, 0, 1)
    under = np.array([0.42, 0.38, 0.33], np.float32)
    rgb = rgb * (1 - tear[..., None]) + under[None, None, :] * v[..., None] * tear[..., None]
    h = -tear * 0.4 + base * 0.1
    return rgb, normal_from(h, 0.7), 0.90


def metalplate(seed, colour=(0.44, 0.45, 0.47), rust=0.35):
    base = wrap_noise(N, 40, seed, 3)
    scratch = np.clip((wrap_noise(N, 220, seed + 3, 1) - 0.72) * 7, 0, 1)
    dent = wrap_noise(N, 8, seed + 7, 3)
    h = dent * 0.5 + base * 0.15
    v = 0.80 + 0.26 * base + 0.2 * scratch + 0.2 * (dent - 0.5)
    rgb = tint(v, colour)
    rs = np.clip((wrap_noise(N, 6, seed + 19, 4) - 0.55) * 4, 0, 1) * rust
    rgb = rgb * (1 - rs[..., None]) + np.array([0.40, 0.19, 0.08], np.float32)[None, None, :] * rs[..., None]
    return rgb, normal_from(h, 1.0), 0.30 + 0.6 * rs + 0.1 * base


def glassbroken(seed):
    cracks = np.clip((ridged(N, 5, seed, 5) - 0.80) * 8, 0, 1)
    dust = wrap_noise(N, 60, seed + 3, 2)
    v = 0.55 + 0.4 * cracks + 0.18 * dust
    rgb = tint(v, (0.62, 0.70, 0.72))
    return rgb, normal_from(cracks * 0.6, 1.4), 0.10 + 0.6 * cracks


MATERIALS = {
    "asphalt":      lambda: asphalt(101),
    "sidewalk":     lambda: concrete(202, (0.62, 0.61, 0.58)),
    "concrete":     lambda: concrete(203, (0.58, 0.58, 0.56)),
    "concrete_wet": lambda: concrete(204, (0.46, 0.47, 0.48), wet=0.8),
    "brick_red":    lambda: brick(301, (0.50, 0.24, 0.18)),
    "brick_buff":   lambda: brick(302, (0.60, 0.48, 0.34), courses=14),
    "brick_grey":   lambda: brick(303, (0.42, 0.42, 0.42), courses=18),
    "plaster":      lambda: plaster(401, (0.74, 0.71, 0.63)),
    "plaster_green":lambda: plaster(402, (0.50, 0.56, 0.48), peel=0.8),
    "plaster_blue": lambda: plaster(403, (0.44, 0.52, 0.58), peel=0.7),
    "wallpaper":    lambda: wallpaper(501, (0.56, 0.48, 0.38)),
    "wallpaper2":   lambda: wallpaper(502, (0.40, 0.46, 0.44)),
    "plank":        lambda: planks(601, (0.42, 0.30, 0.19), 7),
    "plank_floor":  lambda: planks(602, (0.38, 0.26, 0.16), 9, across=True, worn=0.8),
    "plank_pale":   lambda: planks(603, (0.58, 0.48, 0.33), 6),
    "corrugated":   lambda: corrugated(701),
    "metal":        lambda: metalplate(702),
    "metal_rust":   lambda: metalplate(703, (0.40, 0.36, 0.33), rust=0.85),
    "tile":         lambda: tiles(801),
    "tile_clinic":  lambda: tiles(802, (0.72, 0.76, 0.74), (0.52, 0.58, 0.56), 10),
    # Big worn slabs. A church floor is not a chequerboard.
    "flagstone":    lambda: tiles(803, (0.60, 0.58, 0.54), (0.54, 0.53, 0.50), 4),
    "gravel":       lambda: gravel(901),
    "dirt":         lambda: dirtground(902),
    "grass":        lambda: deadgrass(903),
    "shingle":      lambda: shingle(1001),
    "shingle_grey": lambda: shingle(1002, (0.34, 0.34, 0.36)),
    "glass":        lambda: glassbroken(1101),
}


# THE NINE THE TOWN LAYS. Kept in step with _SURFACE in
# rotgrave/town.py by hand, which is fine while it is nine and one
# file; the day it is not, the table moves and this reads it.
IN_USE = {"asphalt", "sidewalk", "brick_red", "plaster", "plank",
          "concrete", "shingle_grey", "metal_rust", "grass"}


# WHAT IS ACTUALLY METAL. Everything else is a dielectric, and
# guessing otherwise is how a plaster wall ends up looking like
# pewter: metalness is not "shiny", it is a different BRDF.
METAL = {
    "corrugated": 0.9,
    "metal": 0.95,
    "metal_rust": 0.35,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="assets/world")
    ap.add_argument("--only", default="")
    ap.add_argument("--all", action="store_true",
                    help="every material, not just the ones in use")
    args = ap.parse_args()
    # WHAT IS COMMITTED IS WHAT THE TOWN ASKS FOR. All 27 materials
    # generate in a couple of seconds, but three 512-pixel images
    # each is nineteen megabytes of build output, and the eighteen
    # nobody has wired up yet are eighteen nobody can look at. Run
    # with --all to make the lot; the nine in rotgrave/town.py's
    # _SURFACE table are what ships.
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    n = 0
    for name, fn in MATERIALS.items():
        if args.only and args.only not in name:
            continue
        if not args.all and not args.only and name not in IN_USE:
            continue
        rgb, nrm, rough = fn()
        save_rgb(out / ("%s_c.png" % name), rgb)
        save_normal(out / ("%s_n.png" % name), nrm)
        save_orm(out / ("%s_orm.png" % name), rough, METAL.get(name, 0.0))
        n += 1
        print("  %-16s" % name)
    print("%d materials -> %s" % (n, out))


if __name__ == "__main__":
    main()
