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

MAGIC = b"RHFIG2"
HEADER = 8

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


def convert(stem, out_path, texture_dir, scale=1.0):
    man, surfaces = read_figure(stem)
    bones = man.get("bones", [])
    if not bones:
        raise ValueError(f"{stem}: no bones")

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

        i_view = blob.add(struct.pack("<%dI" % ni, *[max(0, v)
                                                     for v in s["index"]]),
                          ELEMENT_ARRAY_BUFFER)
        a_i = accessor(i_view, ni, U32, "SCALAR")

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
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
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
            man, nv, nt = convert(stem, os.path.join(args.out, name + ".glb"),
                                  args.src, args.scale)
        except Exception as exc:
            print(f"  {name}: {exc}", file=sys.stderr)
            continue
        print(f"  {name}: {nv} verts, {nt} tris, "
              f"{len(man.get('bones', []))} bones")
        done += 1
    print(f"{done} figures -> {args.out}")


if __name__ == "__main__":
    main()
