#!/usr/bin/env python3
"""
ROTGRAVE -- the noise, synthesised.

The borrowed sound set (see ATTRIBUTION.md) is a fantasy game's: swords,
spells, a bow. What a shooter needs it does not have -- eleven guns, a
reload per action type, brass on concrete, and eight kinds of thing
groaning at you out of the dark. Those are generated here, on the same
principle the borrowed ones were: nothing is a binary nobody can edit, and
anything that sounds wrong is a line in this file.

A GUN IS THREE SOUNDS, NOT ONE. There is the crack -- the few milliseconds
of the round leaving, which is nearly all click and almost no pitch; the
body, which is the pressure wave and is what makes a rifle a rifle and a
pistol a pistol; and the tail, which is the street answering, and is the
only part that says whether you are indoors. They are layered per weapon
with different weights, and the tail is a separate file the game plays at
its own distance-dependent volume.

    pip install numpy scipy soundfile
    python3 tools/gen_sfx.py --out godot/assets/audio/sfx
"""

import argparse
from pathlib import Path

import numpy as np
from scipy import signal
import soundfile as sf

SR = 44100
rng = np.random.default_rng(0x5EED)


# --------------------------------------------------------------- utilities

def noise(n, colour="white"):
    x = rng.standard_normal(n).astype(np.float32)
    if colour == "pink":
        b, a = signal.butter(1, 0.06, "low")
        x = signal.lfilter(b, a, x).astype(np.float32) * 3.0
    elif colour == "brown":
        x = np.cumsum(x).astype(np.float32)
        x /= max(1e-6, np.max(np.abs(x)))
    return x


def env(n, attack=0.001, hold=0.0, decay=0.2, curve=2.5):
    a = max(1, int(SR * attack))
    h = int(SR * hold)
    d = max(1, n - a - h)
    e = np.concatenate([
        np.linspace(0, 1, a, dtype=np.float32),
        np.ones(h, np.float32),
        (np.linspace(1, 0, d, dtype=np.float32) ** curve),
    ])
    return e[:n] if len(e) >= n else np.pad(e, (0, n - len(e)))


def lp(x, hz, order=4):
    hz = min(hz, SR * 0.49)
    b, a = signal.butter(order, hz / (SR / 2), "low")
    return signal.lfilter(b, a, x).astype(np.float32)


def hp(x, hz, order=4):
    b, a = signal.butter(order, max(20, hz) / (SR / 2), "high")
    return signal.lfilter(b, a, x).astype(np.float32)


def bp(x, lo, hi, order=4):
    b, a = signal.butter(order, [max(20, lo) / (SR / 2), min(hi, SR * 0.49) / (SR / 2)], "band")
    return signal.lfilter(b, a, x).astype(np.float32)


def resonate(x, hz, q=12.0, gain=1.0):
    """One resonant peak -- a barrel, a chamber, a chest cavity."""
    w = hz / (SR / 2)
    b, a = signal.iirpeak(min(w, 0.98), q)
    return x + gain * signal.lfilter(b, a, x).astype(np.float32)


def drive(x, amount=3.0):
    return np.tanh(x * amount).astype(np.float32) / np.tanh(amount)


def verb(x, decay=0.35, size=0.05, taps=7):
    """A cheap room: a few delayed, filtered copies."""
    out = x.copy()
    for i in range(1, taps + 1):
        d = int(SR * size * i * (0.7 + 0.6 * rng.random()))
        if d >= len(x):
            break
        g = decay ** i
        out[d:] += lp(x[:len(x) - d], 6000 - 500 * i) * g
    return out


def sweep(n, f0, f1, curve=3.0):
    t = np.linspace(0, 1, n, dtype=np.float32)
    f = f1 + (f0 - f1) * (1 - t) ** curve
    return np.sin(2 * np.pi * np.cumsum(f) / SR).astype(np.float32)


def mix(*layers):
    """LAYERS OF DIFFERENT LENGTHS, SUMMED. A slam is a low rumble that
    outlasts the wet impact on top of it, so the layers are padded to the
    longest rather than required to agree."""
    n = max(len(x) for x in layers)
    out = np.zeros(n, np.float32)
    for x in layers:
        out[:len(x)] += x
    return out


def norm(x, peak=0.94):
    m = float(np.max(np.abs(x)))
    return x * (peak / m) if m > 1e-9 else x


def fade(x, ms_in=1.0, ms_out=12.0):
    a = int(SR * ms_in / 1000)
    b = int(SR * ms_out / 1000)
    if 0 < a < len(x):
        x[:a] *= np.linspace(0, 1, a)
    if 0 < b < len(x):
        x[-b:] *= np.linspace(1, 0, b)
    return x


def sec(s):
    return int(SR * s)


# -------------------------------------------------------------------- guns

def gunshot(body_hz, dur, crack=1.0, punch=1.0, bright=6000, grit=3.0,
            room=0.0, mech=0.0):
    """THE GUN ITSELF: crack, body, mechanism. No tail -- that is its own
    file, so the game can pay for the street separately from the weapon."""
    n = sec(dur)
    out = np.zeros(n, np.float32)

    # The crack: broadband, four milliseconds, almost no pitch in it at all.
    ck = noise(n) * env(n, 0.0002, 0.0005, 0.010, 4.0)
    out += hp(ck, 2200) * 1.15 * crack

    # The body: the pressure wave. A falling tone under a band of noise,
    # which is what separates a rifle report from a pistol's.
    bd = sweep(n, body_hz * 2.6, body_hz * 0.7, 2.2) * env(n, 0.0004, 0.004, dur * 0.33, 2.4)
    bd += bp(noise(n), body_hz * 0.5, bright) * env(n, 0.0004, 0.008, dur * 0.45, 2.0) * 0.9
    out += bd * punch

    # A resonance for the barrel, and one lower for the receiver.
    out = resonate(out, body_hz * 1.9, q=9, gain=0.35)
    out = resonate(out, body_hz * 0.62, q=6, gain=0.5)
    out = drive(out, grit)

    if mech > 0:
        # The action: a metallic click a few milliseconds behind the shot.
        d = sec(0.012)
        m = np.zeros(n, np.float32)
        k = bp(noise(n - d), 2600, 9000) * env(n - d, 0.0002, 0.0, 0.035, 3.0)
        m[d:] = k
        out += m * 0.5 * mech
    if room > 0:
        out += verb(out, 0.3, 0.018, 4) * room
    return fade(norm(out, 0.97))


def tail(dur, low, size, decay=0.42):
    """THE STREET ANSWERING. Played on top of every shot, quieter the more
    open the ground is; it is most of what makes a town sound like a town."""
    n = sec(dur)
    x = noise(n, "pink") * env(n, 0.004, 0.02, dur * 0.9, 1.6)
    x = lp(x, low)
    x = verb(x, decay, size, 9)
    x = bp(x, 90, low)
    return fade(norm(x, 0.55), 6.0, 180.0)


# ------------------------------------------------------------- mechanisms

def click(dur, lo, hi, n_clicks=1, spread=0.0, bright=1.0):
    n = sec(dur)
    out = np.zeros(n, np.float32)
    for i in range(n_clicks):
        at = int(spread * SR * (i / max(1, n_clicks - 1))) if n_clicks > 1 else 0
        if at >= n:
            break
        seg = n - at
        k = bp(noise(seg), lo, hi) * env(seg, 0.0002, 0.0, 0.020 + 0.01 * rng.random(), 3.2)
        out[at:] += k * (0.7 + 0.5 * rng.random())
    out = resonate(out, 3200 * bright, q=14, gain=0.6)
    return fade(norm(out, 0.8))


def magazine(dur=0.55, heavy=1.0):
    """A magazine out, or in: a scrape, a clack, a spring."""
    n = sec(dur)
    out = np.zeros(n, np.float32)
    scr = bp(noise(n), 900, 5200) * env(n, 0.006, 0.02, dur * 0.35, 2.0) * 0.5
    out += scr
    at = sec(dur * 0.45)
    seg = n - at
    out[at:] += bp(noise(seg), 500 * heavy, 4200) * env(seg, 0.0003, 0.0, 0.07, 2.6) * 1.0
    out = resonate(out, 1500 / heavy, q=8, gain=0.7)
    out = resonate(out, 4600, q=16, gain=0.35)
    return fade(norm(out, 0.85))


def shell(dur=0.7, glass=0.0):
    """Brass on concrete: three or four little bounces, getting closer."""
    n = sec(dur)
    out = np.zeros(n, np.float32)
    # A DROPPED CASE BOUNCES FIVE OR SIX TIMES, closer together each time
    # and quieter each time. COUNTED, not run to a clock: the gaps shrink
    # geometrically, so the sum converges well short of the file's length
    # and `while t < dur` never finishes.
    t = 0.0
    gap = 0.11
    amp = 1.0
    for _ in range(6):
        at = sec(t)
        if at >= n - 8:
            break
        seg = n - at
        k = bp(noise(seg), 3200, 12000) * env(seg, 0.0002, 0.0, 0.018, 3.5)
        k = resonate(k, 5200 + rng.random() * 2500, q=26, gain=1.8)
        out[at:] += k * amp
        t += gap
        gap *= 0.62
        amp *= 0.55
    if glass:
        out += bp(noise(n), 6000, 15000) * env(n, 0.001, 0.0, 0.25, 3.0) * 0.3 * glass
    return fade(norm(out, 0.7))


# ------------------------------------------------------------------ impact

def flesh(dur=0.30, wet=1.0, heavy=1.0, crunch=0.0):
    n = sec(dur)
    thud = lp(noise(n), 420 / heavy) * env(n, 0.0008, 0.004, dur * 0.5, 2.2)
    slap = bp(noise(n), 700, 3400) * env(n, 0.0004, 0.001, 0.05, 3.0) * 0.8 * wet
    out = thud * 1.2 + slap
    out += sweep(n, 180 * heavy, 55, 2.0) * env(n, 0.001, 0.003, dur * 0.4, 2.5) * 0.7
    if crunch:
        out += bp(noise(n), 1800, 8000) * env(n, 0.0003, 0.0, 0.06, 3.4) * 0.7 * crunch
    return fade(norm(drive(out, 2.0), 0.9))


def impact(dur, lo, hi, tone=0.0, dust=0.0):
    n = sec(dur)
    out = bp(noise(n), lo, hi) * env(n, 0.0003, 0.001, dur * 0.4, 3.0)
    if tone:
        out += sweep(n, tone * 2, tone * 0.6, 2.0) * env(n, 0.0004, 0.0, dur * 0.3, 3.0) * 0.6
    if dust:
        out += lp(noise(n), 1400) * env(n, 0.004, 0.01, dur * 0.8, 1.6) * 0.35 * dust
    return fade(norm(out, 0.85))


# ------------------------------------------------------------------ voices

def groan(dur=1.6, pitch=95.0, rasp=1.0, wet=0.6, growl=0.0):
    """A THROAT WITH NOTHING BEHIND IT.

    Two formants over a broken glottal buzz -- which is a vowel -- with the
    buzz's rate wandering, because nothing about this is steady. `growl`
    drops an octave of sub under it and is what the big ones have."""
    n = sec(dur)
    t = np.linspace(0, dur, n, dtype=np.float32)
    wob = 1.0 + 0.10 * np.sin(2 * np.pi * 0.7 * t + rng.random() * 6) \
              + 0.05 * np.sin(2 * np.pi * 2.3 * t)
    f0 = pitch * wob * (1.0 - 0.28 * (t / dur) ** 1.5)
    ph = 2 * np.pi * np.cumsum(f0) / SR
    buzz = signal.sawtooth(ph, 0.35).astype(np.float32)
    # A dying larynx does not close cleanly: cut chunks out of the buzz.
    hole = (noise(n, "pink") > -0.35 * rasp).astype(np.float32)
    hole = lp(hole, 60)
    buzz *= 0.35 + 0.65 * hole
    breath = bp(noise(n), 300, 5200) * (0.25 + 0.4 * rasp)
    src = buzz * 0.8 + breath * 0.5
    # Two formants: the vowel. Low and wide is an "uh"; this is that.
    out = resonate(src, 520 * (pitch / 95.0) ** 0.3, q=7, gain=2.2)
    out = resonate(out, 1180 * (pitch / 95.0) ** 0.25, q=9, gain=1.4)
    out = resonate(out, 2600, q=12, gain=0.5 * wet)
    if growl:
        sub = np.sin(2 * np.pi * np.cumsum(f0 * 0.5) / SR).astype(np.float32)
        out += sub * 0.8 * growl
    out *= env(n, 0.12 * dur, 0.15 * dur, dur * 0.65, 1.4)
    out = drive(out, 1.8)
    out += verb(out, 0.25, 0.03, 4) * 0.25
    return fade(norm(out, 0.82), 10.0, 120.0)


def shriek(dur=0.9, pitch=340.0, harsh=1.0):
    n = sec(dur)
    t = np.linspace(0, 1, n, dtype=np.float32)
    f = pitch * (1.0 + 0.7 * np.sin(np.pi * t) - 0.25 * t)
    ph = 2 * np.pi * np.cumsum(f) / SR
    src = (signal.sawtooth(ph, 0.5) * 0.6 + np.sin(ph * 2.01) * 0.3).astype(np.float32)
    src += bp(noise(n), 1500, 9000) * 0.55 * harsh
    out = resonate(src, 1400, q=8, gain=1.4)
    out = resonate(out, 3100, q=14, gain=0.9)
    out *= env(n, 0.02, 0.10, dur * 0.7, 1.8)
    out = drive(out, 2.6)
    out += verb(out, 0.3, 0.025, 5) * 0.3
    return fade(norm(out, 0.85))


def bark(dur=0.55, pitch=150.0):
    n = sec(dur)
    t = np.linspace(0, 1, n, dtype=np.float32)
    f = pitch * (2.2 - 1.5 * t ** 0.5)
    ph = 2 * np.pi * np.cumsum(f) / SR
    src = signal.sawtooth(ph, 0.25).astype(np.float32) * 0.8
    src += bp(noise(n), 800, 6000) * 0.6
    out = resonate(src, 700, q=6, gain=1.8)
    out = resonate(out, 1900, q=10, gain=0.9)
    out *= env(n, 0.004, 0.02, dur * 0.5, 2.2)
    return fade(norm(drive(out, 2.2), 0.9))


# ------------------------------------------------------------------- world

def board(dur=0.5):
    n = sec(dur)
    out = bp(noise(n), 300, 3800) * env(n, 0.001, 0.004, 0.10, 2.6)
    out = resonate(out, 420, q=6, gain=1.4)
    out = resonate(out, 1250, q=9, gain=0.7)
    at = sec(0.07)
    out[at:] += bp(noise(n - at), 900, 6000) * env(n - at, 0.0004, 0.0, 0.05, 3.0) * 0.6
    return fade(norm(out, 0.85))


def splinter(dur=0.8):
    n = sec(dur)
    out = np.zeros(n, np.float32)
    for _ in range(9):
        at = int(rng.random() * n * 0.5)
        seg = n - at
        k = bp(noise(seg), 600 + rng.random() * 2500, 9000) \
            * env(seg, 0.0004, 0.0, 0.03 + 0.08 * rng.random(), 3.0)
        out[at:] += k * (0.3 + 0.7 * rng.random())
    out += lp(noise(n), 500) * env(n, 0.001, 0.005, 0.18, 2.4) * 0.8
    return fade(norm(out, 0.9))


def chime(dur, freqs, decay=1.0, bell=0.0):
    n = sec(dur)
    t = np.linspace(0, dur, n, dtype=np.float32)
    out = np.zeros(n, np.float32)
    for i, f in enumerate(freqs):
        e = np.exp(-t * (2.2 / decay) * (1 + 0.35 * i))
        out += np.sin(2 * np.pi * f * t).astype(np.float32) * e / (i + 1)
        if bell:
            out += np.sin(2 * np.pi * f * 2.76 * t).astype(np.float32) * e * 0.25 * bell
    out *= env(n, 0.002, 0.01, dur * 0.95, 1.0)
    out += verb(out, 0.3, 0.04, 5) * 0.3
    return fade(norm(out, 0.8), 2.0, 200.0)


def whoosh(dur=0.45, lo=200, hi=4000):
    n = sec(dur)
    x = noise(n, "pink")
    t = np.linspace(0, 1, n, dtype=np.float32)
    out = np.zeros(n, np.float32)
    # A moving band: the thing going past.
    step = n // 24
    for i in range(24):
        a, b = i * step, min(n, (i + 1) * step)
        c = lo + (hi - lo) * np.sin(np.pi * (i / 23.0))
        out[a:b] = bp(x[a:b], c * 0.6, c * 1.8, 2) if b - a > 16 else x[a:b]
    out *= env(n, 0.05 * dur, 0.1 * dur, dur * 0.7, 1.6)
    return fade(norm(out, 0.7))


def rumble(dur=2.4, hz=38.0, shake=1.0):
    n = sec(dur)
    t = np.linspace(0, dur, n, dtype=np.float32)
    out = np.sin(2 * np.pi * hz * t * (1 - 0.2 * t / dur)).astype(np.float32)
    out += np.sin(2 * np.pi * hz * 1.51 * t).astype(np.float32) * 0.4
    out *= 0.5 + 0.5 * lp(np.abs(noise(n)), 8) * shake
    out += lp(noise(n, "brown"), 160) * 0.6
    out *= env(n, 0.05, 0.25, dur * 0.7, 1.5)
    return fade(norm(out, 0.9), 20.0, 300.0)


def electric(dur=0.6, hz=90.0, fizz=1.0):
    n = sec(dur)
    t = np.linspace(0, dur, n, dtype=np.float32)
    buzz = signal.square(2 * np.pi * hz * t, 0.3).astype(np.float32)
    buzz *= (noise(n) > -0.2).astype(np.float32)
    out = bp(buzz, 400, 9000) * 0.8
    out += bp(noise(n), 3000, 16000) * env(n, 0.0005, 0.01, dur * 0.5, 2.5) * fizz
    out = resonate(out, 2400, q=10, gain=0.8)
    out *= env(n, 0.001, 0.02, dur * 0.7, 2.0)
    return fade(norm(drive(out, 2.4), 0.88))


# ------------------------------------------------------------------ roster

def build():
    out = {}

    # ---- guns -------------------------------------------------------
    out["gun_pistol"] = gunshot(230, 0.26, crack=1.0, punch=0.9, bright=5200, grit=3.2, mech=0.8)
    out["gun_revolver"] = gunshot(150, 0.42, crack=1.2, punch=1.5, bright=6000, grit=4.0, mech=0.3)
    out["gun_smg"] = gunshot(300, 0.17, crack=0.95, punch=0.7, bright=6200, grit=3.4, mech=1.0)
    out["gun_shotgun"] = gunshot(110, 0.50, crack=1.1, punch=1.8, bright=4200, grit=4.5, mech=0.5)
    out["gun_rifle"] = gunshot(170, 0.36, crack=1.35, punch=1.3, bright=7200, grit=3.6, mech=0.6)
    out["gun_mg"] = gunshot(140, 0.30, crack=1.2, punch=1.5, bright=6400, grit=4.2, mech=0.9)
    out["gun_sniper"] = gunshot(95, 0.62, crack=1.6, punch=2.0, bright=8200, grit=4.8, mech=0.4)
    out["gun_thump"] = gunshot(70, 0.45, crack=0.6, punch=1.9, bright=2600, grit=3.0, mech=0.6)
    out["gun_arc"] = electric(0.34, 140, 1.4)
    out["gun_dry"] = click(0.12, 1800, 9000, 2, 0.02)

    out["tail_close"] = tail(0.85, 2600, 0.012, 0.36)
    out["tail_far"] = tail(1.9, 1500, 0.045, 0.50)
    out["tail_arc"] = tail(0.9, 5200, 0.020, 0.40)
    out["tail_indoor"] = tail(0.55, 3400, 0.006, 0.28)

    # ---- handling ---------------------------------------------------
    out["mag_out"] = magazine(0.50, 1.0)
    out["mag_in"] = magazine(0.42, 1.2)
    out["bolt_back"] = click(0.22, 900, 6000, 2, 0.06, 1.2)
    out["bolt_fwd"] = click(0.18, 700, 5200, 2, 0.045, 0.9)
    out["pump"] = click(0.30, 600, 5600, 3, 0.11, 1.0)
    out["shell_in"] = click(0.16, 1400, 7000, 1, 0.0, 1.3)
    out["shell_drop"] = shell(0.7)
    out["shell_drop_big"] = shell(0.9, 0.3)
    out["weapon_raise"] = whoosh(0.30, 300, 2400)
    out["weapon_swap"] = click(0.25, 500, 4000, 3, 0.09, 0.8)

    # ---- impacts ----------------------------------------------------
    out["hit_flesh"] = flesh(0.30, 1.0, 1.0)
    out["hit_flesh2"] = flesh(0.26, 1.2, 0.9)
    out["hit_head"] = flesh(0.36, 1.4, 0.8, crunch=1.0)
    out["hit_bone"] = impact(0.22, 1200, 7000, tone=380)
    out["hit_armour"] = impact(0.26, 900, 9000, tone=1400)
    out["hit_concrete"] = impact(0.24, 500, 6000, tone=0, dust=1.0)
    out["hit_wood"] = impact(0.22, 350, 4200, tone=260, dust=0.4)
    out["hit_metal"] = impact(0.30, 1500, 12000, tone=2600)
    out["hit_glass"] = impact(0.45, 3000, 15000, tone=5200)
    out["hit_dirt"] = impact(0.26, 200, 2200, dust=1.2)
    out["ricochet"] = whoosh(0.35, 2600, 9000)
    out["gore_burst"] = flesh(0.55, 1.6, 1.4, crunch=0.7)
    out["blast"] = rumble(1.4, 52, 1.2)
    out["explode"] = norm(mix(
        gunshot(60, 0.9, crack=0.8, punch=2.4, bright=3000, grit=5.0, room=0.5),
        np.pad(rumble(0.9, 44, 1.0), (sec(0.02), 0)) * 0.9))

    # ---- the risen --------------------------------------------------
    for i, (p, r, g) in enumerate([(92, 1.0, 0.0), (76, 1.2, 0.2), (110, 0.8, 0.0),
                                   (64, 1.4, 0.35), (128, 0.7, 0.0)]):
        out["groan%d" % (i + 1)] = groan(1.4 + 0.5 * rng.random(), p, r, 0.6, g)
    for i in range(3):
        out["hurt%d" % (i + 1)] = groan(0.55, 120 + 30 * i, 1.4, 0.8)
    for i in range(3):
        out["undead_die%d" % (i + 1)] = groan(1.3, 70 + 22 * i, 1.5, 0.5, 0.25)
    out["undead_attack1"] = shriek(0.55, 300, 1.1)
    out["undead_attack2"] = shriek(0.48, 380, 1.3)
    out["undead_spot"] = shriek(1.05, 260, 0.9)
    out["undead_spit"] = whoosh(0.5, 400, 3200)
    out["undead_bile"] = flesh(0.7, 1.8, 0.7)
    out["boss_roar"] = groan(2.6, 44, 1.5, 0.4, 1.0)
    out["boss_slam"] = norm(mix(rumble(1.2, 40, 1.4), flesh(0.4, 0.8, 2.0) * 0.8))
    out["boss_step"] = impact(0.45, 60, 900, tone=70, dust=1.0)
    out["hound_bark"] = bark(0.5, 155)
    out["hound_bark2"] = bark(0.42, 185)
    out["hound_growl"] = groan(1.1, 68, 1.3, 0.3, 0.6)
    out["hound_spawn"] = norm(mix(electric(0.8, 60, 0.8) * 0.6, rumble(1.0, 46, 1.0) * 0.8))

    # ---- the town ---------------------------------------------------
    out["board_place"] = board(0.5)
    out["board_hit"] = board(0.35)
    out["board_break"] = splinter(0.85)
    out["window_smash"] = impact(0.7, 2500, 15000, tone=4800)
    out["door_buy"] = norm(mix(splinter(0.6) * 0.7, impact(0.5, 200, 2000, dust=1.0) * 0.9))
    out["debris_move"] = impact(0.9, 150, 3000, dust=1.4)

    # ---- the machines -----------------------------------------------
    out["buy"] = chime(0.6, [660, 880, 1320], 0.5)
    out["deny"] = chime(0.35, [180, 190], 0.35)
    out["points_up"] = chime(0.3, [1320, 1760], 0.25)
    out["perk_buy"] = chime(1.1, [330, 440, 550, 660], 0.9, bell=0.5)
    out["perk_drink"] = norm(np.concatenate([
        flesh(0.25, 0.6, 0.5) * 0.5, flesh(0.25, 0.6, 0.5) * 0.5,
        flesh(0.3, 0.7, 0.6) * 0.6, chime(0.9, [520, 780], 0.7)]))
    out["box_open"] = norm(np.concatenate([board(0.4) * 0.8, chime(1.2, [392, 523, 659], 1.0, bell=0.8)]))
    out["box_close"] = board(0.55)
    out["box_teddy"] = chime(1.6, [523, 659, 784, 1046], 1.2, bell=1.0)
    out["pap_work"] = norm(np.concatenate([electric(0.9, 70, 1.0), rumble(1.6, 60, 1.0) * 0.8]))
    out["pap_done"] = chime(1.4, [440, 554, 659, 880], 1.1, bell=0.9)
    out["power_on"] = norm(np.concatenate([click(0.2, 400, 4000, 1) * 0.9,
                                           electric(1.6, 55, 0.7) * 0.8,
                                           chime(1.8, [110, 220, 330], 1.4) * 0.7]))
    out["machine_hum"] = electric(2.0, 62, 0.2)

    # ---- the survivor -----------------------------------------------
    out["hurt_player1"] = groan(0.42, 210, 0.5, 0.4)
    out["hurt_player2"] = groan(0.38, 240, 0.6, 0.4)
    out["hurt_player3"] = groan(0.5, 190, 0.5, 0.4)
    out["downed"] = groan(1.5, 165, 0.9, 0.5)
    out["revive"] = chime(1.3, [392, 523, 784], 1.0, bell=0.6)
    out["last_stand"] = chime(1.6, [196, 233, 294], 1.4)
    out["heartbeat_low"] = norm(np.concatenate([
        lp(noise(sec(0.12)), 120) * env(sec(0.12), 0.004, 0.01, 0.10, 2.0),
        np.zeros(sec(0.10), np.float32),
        lp(noise(sec(0.14)), 100) * env(sec(0.14), 0.004, 0.01, 0.12, 2.0) * 0.7,
        np.zeros(sec(0.45), np.float32)]))
    out["breath_hard"] = norm(bp(noise(sec(1.2)), 260, 3200)
                              * env(sec(1.2), 0.15, 0.1, 0.8, 1.4) * 0.8)

    # ---- rounds -----------------------------------------------------
    out["round_start"] = norm(np.concatenate([
        chime(1.0, [110, 165], 0.9) * 0.8, rumble(1.6, 42, 0.8) * 0.7]))
    out["round_end"] = chime(2.0, [147, 196, 294], 1.6, bell=0.4)
    out["round_final"] = norm(np.concatenate([chime(0.8, [98, 131], 0.7),
                                              rumble(2.2, 33, 1.3)]))
    out["game_over"] = norm(np.concatenate([chime(1.2, [131, 156], 1.0),
                                            rumble(2.6, 30, 1.0) * 0.9]))

    # ---- powerups ---------------------------------------------------
    out["drop_spawn"] = chime(0.7, [880, 1174], 0.6)
    out["drop_nuke"] = norm(np.concatenate([rumble(1.8, 36, 1.4), chime(1.0, [220, 330], 0.8) * 0.6]))
    out["drop_maxammo"] = chime(1.1, [523, 659, 880, 1046], 0.9, bell=0.4)
    out["drop_instakill"] = chime(1.2, [330, 415, 554], 1.0, bell=0.7)
    out["drop_double"] = chime(1.0, [659, 830, 988], 0.8, bell=0.3)
    out["drop_carpenter"] = norm(np.concatenate([board(0.4), board(0.4), chime(0.8, [440, 587], 0.7)]))

    # ---- ui ---------------------------------------------------------
    out["ui_move"] = click(0.07, 2200, 7000, 1, 0.0, 1.4)
    out["ui_pick"] = chime(0.28, [880, 1320], 0.25)
    out["ui_back"] = chime(0.26, [440, 330], 0.25)
    out["ui_deny"] = chime(0.3, [220, 208], 0.3)

    # ---- footsteps and cloth ---------------------------------------
    for i in range(3):
        out["step_road%d" % (i + 1)] = impact(0.16, 250, 4200 + 800 * i, dust=0.5)
        out["step_wood%d" % (i + 1)] = impact(0.18, 180, 3000 + 600 * i, tone=200 + 30 * i)
        out["step_grass%d" % (i + 1)] = norm(bp(noise(sec(0.20)), 900, 7000)
                                             * env(sec(0.20), 0.001, 0.004, 0.14, 2.6) * 0.7)
        out["step_gravel%d" % (i + 1)] = norm(bp(noise(sec(0.22)), 1400, 9000)
                                              * env(sec(0.22), 0.001, 0.003, 0.16, 3.0))
    out["land_hard"] = norm(impact(0.3, 120, 2600, tone=90, dust=1.0) * 1.0)
    out["jump_off"] = whoosh(0.22, 300, 1800)
    out["vault"] = norm(np.concatenate([whoosh(0.25, 400, 2600) * 0.7, impact(0.2, 200, 3000)]))

    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="godot/assets/audio/sfx")
    ap.add_argument("--only", default="")
    args = ap.parse_args()
    outdir = Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)
    bank = build()
    n = 0
    for name, data in sorted(bank.items()):
        if args.only and args.only not in name:
            continue
        x = np.clip(np.nan_to_num(data), -1.0, 1.0).astype(np.float32)
        sf.write(outdir / ("%s.wav" % name), x, SR, subtype="PCM_16")
        n += 1
    print("%d sounds -> %s" % (n, outdir))


if __name__ == "__main__":
    main()
