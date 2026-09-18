#!/usr/bin/env python3
"""ROTGRAVE -- the figures, out of Rhabdos and into glTF.

The bodies are MakeHuman meshes (CC0, see design/ATTRIBUTION.md)
saved in Rhabdos's own `RHFIG2` format: a JSON manifest beside a
binary blob. Warren does not read that format and has no business
learning to -- it is one game's private container, and the engine
already reads glTF, which is the format everything else in the
world also reads.

So this converts once, offline, and what goes in the repository is
standard glTF that any tool can open. The licence terms travel with
the files, not with a loader.

THE FORMAT, since nothing else documents it:

    "RHFIG2"            6 bytes, then 2 of padding
    u32                 surface count
    per surface:
        u32 nv, u32 ni
        f32 position[nv * 3]
        f32 normal  [nv * 3]
        f32 uv      [nv * 2]
        i32 joints  [nv * 4]
        f32 weights [nv * 4]
        i32 index   [ni]

The manifest carries the bone hierarchy -- name, parent, and a
REST POSITION IN WORLD SPACE -- and one record per surface giving
its texture and whether it needs alpha.

    python3 tools/rhfig_to_gltf.py --in <dir> --out assets/figures
"""

import argparse
import base64
import json
import os
import struct
import sys

try:
    import numpy as np
except ImportError:      # the masking pass needs it; nothing else does
    np = None

MAGIC = b"RHFIG2"
HEADER = 8

# HOW FAR OFF THE SKIN EACH LAYER SITS, in metres.
#
# A few surfaces really are coincident with the face -- the brows
# and lashes are drawn a hair above it and nothing else separates
# them -- so each one is pushed out along its own normals by a
# fraction of a millimetre, in the order somebody dresses in. This
# is only about the depth buffer being unable to choose between two
# surfaces at the same depth. It is NOT what fixes the body showing
# through the clothes; see hidden_skin below for that.
LAYER = {
    "skin": 0.0,
    "eyes": 0.0,
    "lashes": 0.0008,
    "brows": 0.0010,
    "breeches": 0.0020,
    "boots": 0.0030,
    "mail": 0.0032,
    "leathers": 0.0040,
    "robe": 0.0044,
    "coat": 0.0048,
    "beard": 0.0050,
    "hair": 0.0054,
    "hood": 0.0062,
    "helm": 0.0066,
}
DEFAULT_LAYER = 0.0035

# Which surfaces count as clothing for the purpose of hiding the
# body underneath. Brows, lashes and eyes sit ON the face and would
# mask it away; hair and beards are sparse and alpha-cut, and the
# scalp behind them still needs to be there.
CLOTHING = {"breeches", "boots", "mail", "leathers", "robe", "coat",
            "hood", "helm"}

# How far the body is allowed to be from a garment and still count
# as being under it, in metres. Loose clothing hangs further off
# than this in places, but those are places where the skin could
# never poke through anyway.
COVER_REACH = 0.07

# And how far BEHIND a piece of skin cloth may be for that skin to
# count as having burst out through it. Shorter, because looking
# backwards through your own body finds things that are not over
# you -- the far wall of a sleeve, the other leg.
BURST_REACH = 0.03

# glTF component types
F32, U16, U32 = 5126, 5123, 5125
ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963


def read_figure(stem):
    """The manifest and every surface's arrays."""
    with open(stem + ".json") as f:
        man = json.load(f)
    with open(stem + ".bin", "rb") as f:
        raw = f.read()
    if raw[:6] != MAGIC:
        raise ValueError(f"{stem}.bin is not a {MAGIC.decode()} file")

    at = HEADER
    (nsurf,) = struct.unpack_from("<I", raw, at)
    at += 4
    surfaces = []
    for i in range(nsurf):
        nv, ni = struct.unpack_from("<II", raw, at)
        at += 8

        def take(count, fmt, size):
            nonlocal at
            out = struct.unpack_from("<%d%s" % (count, fmt), raw, at)
            at += count * size
            return out

        pos = take(nv * 3, "f", 4)
        nrm = take(nv * 3, "f", 4)
        uv = take(nv * 2, "f", 4)
        joints = take(nv * 4, "i", 4)
        weights = take(nv * 4, "f", 4)
        index = take(ni, "i", 4)
        spec = man["surfaces"][i] if i < len(man.get("surfaces", [])) else {}
        surfaces.append(dict(nv=nv, ni=ni, pos=pos, nrm=nrm, uv=uv,
                             joints=joints, weights=weights, index=index,
                             spec=spec))
    return man, surfaces


class Blob:
    """The one buffer, and the views into it.

    glTF wants every accessor's data aligned to its component size,
    so each view is padded up. Getting that wrong produces a file
    that loads in some viewers and not others, which is a worse
    outcome than one that loads nowhere.
    """

    def __init__(self):
        self.data = bytearray()
        self.views = []

    def add(self, payload, target=None, stride=None):
        while len(self.data) % 4:
            self.data.append(0)
        offset = len(self.data)
        self.data.extend(payload)
        view = {"buffer": 0, "byteOffset": offset, "byteLength": len(payload)}
        if target is not None:
            view["target"] = target
        if stride is not None:
            view["byteStride"] = stride
        self.views.append(view)
        return len(self.views) - 1


def _grid_of(tri_p, cell):
    """Triangles bucketed by the cells their bounds touch."""
    lo = tri_p.min(axis=1)
    hi = tri_p.max(axis=1)
    cells = {}
    for t in range(len(tri_p)):
        a = np.floor(lo[t] / cell).astype(int)
        b = np.floor(hi[t] / cell).astype(int)
        for x in range(a[0], b[0] + 1):
            for y in range(a[1], b[1] + 1):
                for z in range(a[2], b[2] + 1):
                    cells.setdefault((x, y, z), []).append(t)
    return cells


def hidden_skin(surfaces, reach=COVER_REACH):
    """The body triangles that no one can see, because cloth is over them.

    MakeHuman models its clothing ON the body and then deletes the
    body underneath -- the trousers come with a list of the vertices
    they cover, and the exporter is meant to drop them. Rhabdos's
    exporter kept the whole body, so the thighs are still inside the
    trousers, and they do not stay inside: the body is a separate
    surface posed by the same skeleton, and wherever it is modelled
    a little fuller than the cloth, it comes through. Measured on
    the zombie, a ninth of the trouser surface has skin outside it,
    by up to five centimetres. That reads as dirty texture work or
    as z-fighting and is neither -- it is one mesh sticking through
    another, and no depth bias or layer offset touches it.

    So the delete groups are worked out here instead of being read,
    by asking the only question that matters: standing on this piece
    of skin and looking straight out, is there cloth in the way? A
    short ray along the vertex normal answers it, and answers it
    correctly for loose clothing (still covered, just further away)
    and for gaps between garments (nothing in the way -- keep it).

    A triangle goes only when all three of its corners are covered,
    which leaves a rim of body standing under every hem rather than
    a hole at the edge of it.
    """
    skin = [s for s in surfaces if s["spec"].get("role") == "skin"]
    cloth = [s for s in surfaces if s["spec"].get("role") in CLOTHING]
    if not skin or not cloth:
        return 0

    tri = []
    for c in cloth:
        p = np.asarray(c["pos"], dtype=np.float64).reshape(-1, 3)
        idx = np.asarray(c["index"], dtype=np.int64).reshape(-1, 3)
        tri.append(p[idx])
    tri_p = np.concatenate(tri)
    grid = _grid_of(tri_p, reach)

    v0 = tri_p[:, 0]
    e1 = tri_p[:, 1] - v0
    e2 = tri_p[:, 2] - v0

    dropped = 0
    for s in skin:
        p = np.asarray(s["pos"], dtype=np.float64).reshape(-1, 3)
        n = np.asarray(s["nrm"], dtype=np.float64).reshape(-1, 3)
        covered = np.zeros(len(p), dtype=bool)

        def strikes(a, d, span):
            """Does the ray from a along d, of length span, meet cloth?"""
            b = a + d * span
            lo = np.floor(np.minimum(a, b) / reach).astype(int)
            hi = np.floor(np.maximum(a, b) / reach).astype(int)
            near = set()
            for x in range(lo[0], hi[0] + 1):
                for y in range(lo[1], hi[1] + 1):
                    for z in range(lo[2], hi[2] + 1):
                        near.update(grid.get((x, y, z), ()))
            if not near:
                return False
            k = np.fromiter(near, dtype=np.int64, count=len(near))
            # Moller-Trumbore, every candidate at once.
            h = np.cross(d, e2[k])
            det = np.einsum("ij,ij->i", e1[k], h)
            live = np.abs(det) > 1e-12
            if not live.any():
                return False
            inv = np.where(live, 1.0 / np.where(live, det, 1.0), 0.0)
            sv = a - v0[k]
            u = np.einsum("ij,ij->i", sv, h) * inv
            q = np.cross(sv, e1[k])
            w = (q @ d) * inv
            t = np.einsum("ij,ij->i", e2[k], q) * inv
            return bool((live & (u >= 0) & (w >= 0) & (u + w <= 1)
                         & (t > 0) & (t <= span)).any())

        for i in range(len(p)):
            # Cloth in front of this skin: it is underneath, drop it.
            # Cloth behind it: it has come out through the garment it
            # belongs inside, which is the whole complaint -- drop it
            # as well, and the cloth closes over the gap.
            if (strikes(p[i] + n[i] * 1e-4, n[i], reach)
                    or strikes(p[i] - n[i] * 1e-4, -n[i], BURST_REACH)):
                covered[i] = True

        idx = np.asarray(s["index"], dtype=np.int64).reshape(-1, 3)
        keep = ~covered[idx].all(axis=1)
        dropped += int((~keep).sum())
        idx = idx[keep]

        # Compact: a vertex no triangle names still costs a skinning
        # transform every frame, so it should not survive the trip.
        used = np.unique(idx)
        remap = np.full(len(p), -1, dtype=np.int64)
        remap[used] = np.arange(len(used))
        for key, width in (("pos", 3), ("nrm", 3), ("uv", 2),
                           ("joints", 4), ("weights", 4)):
            a = np.asarray(s[key]).reshape(-1, width)[used]
            s[key] = tuple(a.reshape(-1).tolist())
        s["index"] = tuple(remap[idx].reshape(-1).tolist())
        s["nv"] = int(len(used))
        s["ni"] = int(idx.size)
    return dropped


def convert(stems, out_path, texture_dir, scale=1.0):
    """One figure, or several pieces worn as one.

    The wearables are exported on their own -- a figure file holding
    nothing but the trousers -- and every piece is rigged to the same
    nineteen bones as the body, so dressing somebody is concatenating
    surfaces. That is how the player is assembled: a bare body, the
    clothes chosen for them, and a haircut.
    """
    if isinstance(stems, str):
        stems = [stems]
    man, surfaces = read_figure(stems[0])
    bones = man.get("bones", [])
    if not bones:
        raise ValueError(f"{stems[0]}: no bones")
    for extra in stems[1:]:
        piece_man, piece = read_figure(extra)
        worn = [b.get("name") for b in piece_man.get("bones", [])]
        if worn != [b.get("name") for b in bones]:
            raise ValueError(f"{extra}: rigged to a different skeleton")
        surfaces.extend(piece)
    if np is not None:
        hidden_skin(surfaces)

    blob = Blob()
    accessors = []
    images, samplers, textures, materials = [], [], [], []
    tex_index = {}

    def accessor(view, count, ctype, kind, mn=None, mx=None):
        a = {"bufferView": view, "componentType": ctype, "count": count,
             "type": kind}
        if mn is not None:
            a["min"], a["max"] = mn, mx
        accessors.append(a)
        return len(accessors) - 1

    def texture_for(name):
        if not name:
            return None
        if name in tex_index:
            return tex_index[name]
        path = os.path.join(texture_dir, name)
        if not os.path.exists(path):
            tex_index[name] = None
            return None
        with open(path, "rb") as f:
            png = f.read()
        view = blob.add(png)
        images.append({"bufferView": view, "mimeType": "image/png"})
        if not samplers:
            samplers.append({"magFilter": 9729, "minFilter": 9987,
                             "wrapS": 10497, "wrapT": 10497})
        textures.append({"sampler": 0, "source": len(images) - 1})
        tex_index[name] = len(textures) - 1
        return tex_index[name]

    # --- the primitives
    primitives = []
    for s in surfaces:
        nv, ni = s["nv"], s["ni"]
        # Lifted off the skin by its layer, along its own normals.
        lift = LAYER.get(s["spec"].get("role", ""), DEFAULT_LAYER) * scale
        if lift > 0.0:
            pos = [(s["pos"][i] * scale) + s["nrm"][i] * lift
                   for i in range(len(s["pos"]))]
        else:
            pos = [v * scale for v in s["pos"]]
        pv = blob.add(struct.pack("<%df" % len(pos), *pos), ARRAY_BUFFER)
        mn = [min(pos[i::3]) for i in range(3)]
        mx = [max(pos[i::3]) for i in range(3)]
        a_pos = accessor(pv, nv, F32, "VEC3", mn, mx)

        nv_view = blob.add(struct.pack("<%df" % len(s["nrm"]), *s["nrm"]),
                           ARRAY_BUFFER)
        a_nrm = accessor(nv_view, nv, F32, "VEC3")

        uv_view = blob.add(struct.pack("<%df" % len(s["uv"]), *s["uv"]),
                           ARRAY_BUFFER)
        a_uv = accessor(uv_view, nv, F32, "VEC2")

        # Joints as unsigned shorts, clamped into the bone list: a
        # stray index in the source would otherwise be a read past
        # the end of the skin at draw time.
        nb = len(bones)
        j = [max(0, min(nb - 1, int(v))) for v in s["joints"]]
        j_view = blob.add(struct.pack("<%dH" % len(j), *j), ARRAY_BUFFER)
        a_j = accessor(j_view, nv, U16, "VEC4")

        # Weights normalised. A vertex whose weights do not sum to
        # one is a vertex that shrinks or explodes when the skeleton
        # moves, and the source is not guaranteed to be tidy.
        w = list(s["weights"])
        for k in range(0, len(w), 4):
            total = w[k] + w[k + 1] + w[k + 2] + w[k + 3]
            if total > 1e-6:
                for m in range(4):
                    w[k + m] /= total
            else:
                w[k] = 1.0
        w_view = blob.add(struct.pack("<%df" % len(w), *w), ARRAY_BUFFER)
        a_w = accessor(w_view, nv, F32, "VEC4")

        # THE WINDING IS REVERSED ON THE WAY OUT.
        #
        # RHFIG was written for Godot, whose ArrayMesh treats a
        # clockwise triangle as front-facing. glTF says the opposite:
        # counter-clockwise is the front, and every renderer that
        # reads the spec agrees. Copying the indices across unchanged
        # produced figures whose fronts were culled and whose backs
        # were not -- from in front of the survivor you saw through
        # her to the inside of her own back, with the braid hanging
        # down over her chest. It reads as a hole in the model rather
        # than as the whole model being inside out, which is why it
        # survived a dozen screenshots taken from behind.
        #
        # Two-sided surfaces hid it further: the hair is two-sided in
        # the manifest, so the braid drew in both views and made the
        # picture look almost right.
        flipped = []
        src_index = s["index"]
        for t in range(0, ni - 2, 3):
            flipped.append(max(0, src_index[t]))
            flipped.append(max(0, src_index[t + 2]))
            flipped.append(max(0, src_index[t + 1]))
        i_view = blob.add(struct.pack("<%dI" % len(flipped), *flipped),
                          ELEMENT_ARRAY_BUFFER)
        a_i = accessor(i_view, len(flipped), U32, "SCALAR")

        spec = s["spec"]
        mat = {"name": spec.get("name", "surface"),
               "pbrMetallicRoughness": {"metallicFactor": 0.0,
                                        "roughnessFactor": 0.9}}
        t = texture_for(spec.get("texture"))
        if t is not None:
            mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": t}
        if spec.get("alpha"):
            mat["alphaMode"] = "MASK"
            mat["alphaCutoff"] = 0.5
        if spec.get("two_sided"):
            mat["doubleSided"] = True
        materials.append(mat)

        primitives.append({
            "attributes": {"POSITION": a_pos, "NORMAL": a_nrm,
                           "TEXCOORD_0": a_uv, "JOINTS_0": a_j,
                           "WEIGHTS_0": a_w},
            "indices": a_i, "material": len(materials) - 1})

    # --- the skeleton
    #
    # The manifest gives each bone a rest position in WORLD space.
    # glTF wants a local transform per node, so each is its parent's
    # position subtracted; and the inverse bind matrix is the
    # inverse of the world rest, which for a translation-only rest
    # is a translation by its negative.
    nodes = []
    joint_nodes = []
    for i, b in enumerate(bones):
        at = [float(v) * scale for v in b["at"]]
        parent = int(b["parent"])
        local = at if parent < 0 else [
            at[k] - float(bones[parent]["at"][k]) * scale for k in range(3)]
        nodes.append({"name": b.get("name", "bone%d" % i),
                      "translation": local})
        joint_nodes.append(i)
    for i, b in enumerate(bones):
        parent = int(b["parent"])
        if parent >= 0:
            nodes[parent].setdefault("children", []).append(i)

    ibm = bytearray()
    for b in bones:
        at = [float(v) * scale for v in b["at"]]
        # Column-major 4x4, as glTF requires.
        m = [1, 0, 0, 0,
             0, 1, 0, 0,
             0, 0, 1, 0,
             -at[0], -at[1], -at[2], 1]
        ibm.extend(struct.pack("<16f", *m))
    ibm_view = blob.add(bytes(ibm))
    a_ibm = accessor(ibm_view, len(bones), F32, "MAT4")

    mesh_node = len(nodes)
    nodes.append({"name": man.get("name", "figure"), "mesh": 0, "skin": 0})
    roots = [i for i, b in enumerate(bones) if int(b["parent"]) < 0]

    gltf = {
        "asset": {"version": "2.0",
                  "generator": "rotgrave rhfig_to_gltf"},
        "scene": 0,
        "scenes": [{"nodes": roots + [mesh_node]}],
        "nodes": nodes,
        "meshes": [{"name": man.get("name", "figure"),
                    "primitives": primitives}],
        "skins": [{"joints": joint_nodes, "inverseBindMatrices": a_ibm,
                   "skeleton": roots[0] if roots else 0}],
        "materials": materials,
        "accessors": accessors,
        "bufferViews": blob.views,
        "buffers": [{"byteLength": len(blob.data)}],
    }
    if images:
        gltf["images"] = images
        gltf["samplers"] = samplers
        gltf["textures"] = textures

    write_glb(out_path, gltf, bytes(blob.data))
    return man, sum(s["nv"] for s in surfaces), sum(s["ni"] for s in surfaces) // 3


def write_glb(path, gltf, binary):
    js = json.dumps(gltf, separators=(",", ":")).encode()
    js += b" " * ((4 - len(js) % 4) % 4)
    bin_pad = binary + b"\0" * ((4 - len(binary) % 4) % 4)
    total = 12 + 8 + len(js) + 8 + len(bin_pad)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A))
        f.write(js)
        f.write(struct.pack("<II", len(bin_pad), 0x004E4942))
        f.write(bin_pad)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", required=True,
                    help="directory of .json/.bin figures")
    ap.add_argument("--out", default="assets/figures")
    ap.add_argument("--only", default="",
                    help="substring filter on the file stem")
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--compose", action="append", default=[],
                    metavar="NAME=STEM+STEM",
                    help="one figure worn out of several pieces")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    # A sidecar, because glTF has nowhere sensible to record how tall
    # the thing in it is and the game needs to know: a species has a
    # height in the design and the figure has one of its own, and the
    # body has to be scaled from the second to the first.
    index_path = os.path.join(args.out, "figures.json")
    index = {}
    if os.path.exists(index_path):
        with open(index_path) as f:
            index = json.load(f)

    def note(name, man, nv, nt):
        index[name] = {"height": round(float(man.get("height", 0.0)), 4),
                       "verts": nv, "tris": nt}
    stems = sorted({os.path.join(args.src, f[:-5])
                    for f in os.listdir(args.src) if f.endswith(".json")})
    done = 0
    for stem in stems:
        name = os.path.basename(stem)
        if args.only and args.only not in name:
            continue
        if not os.path.exists(stem + ".bin"):
            continue
        try:
            man, nv, nt = convert([stem], os.path.join(args.out, name + ".glb"),
                                  args.src, args.scale)
        except Exception as exc:
            print(f"  {name}: {exc}", file=sys.stderr)
            continue
        note(name, man, nv, nt)
        print(f"  {name}: {nv} verts, {nt} tris, "
              f"{len(man.get('bones', []))} bones")
        done += 1
    for spec in args.compose:
        name, _, pieces = spec.partition("=")
        stems = [os.path.join(args.src, q) for q in pieces.split("+") if q]
        try:
            man, nv, nt = convert(stems, os.path.join(args.out, name + ".glb"),
                                  args.src, args.scale)
        except Exception as exc:
            print(f"  {name}: {exc}", file=sys.stderr)
            continue
        note(name, man, nv, nt)
        print(f"  {name}: {nv} verts, {nt} tris, "
              f"{len(man.get('bones', []))} bones  <- "
              + " + ".join(os.path.basename(q) for q in stems))
        done += 1

    with open(index_path, "w") as f:
        json.dump(index, f, indent=1, sort_keys=True)
    print(f"{done} figures -> {args.out}")


if __name__ == "__main__":
    main()
