#!/usr/bin/env python3
"""The rendered hands' models (ft-handview, screens/handskin.h) from their glTF files.

    screens/tools/hand-asset.py [--check]

Reads each model's left and right hand (MODELS below, under screens/assets/hands; see the
README there and each model's own) and writes screens/handasset.h: per hand the vertices
(positions in 0.01 mm steps, normals in 1/127 steps), each vertex's four joints and weights
(out of 255), the triangles, and the 25 WebXR hand joints' positions in the bind pose (WebXR's
order). Each model's skeleton is mapped onto WebXR's joints by name; the program needs no
glTF code: the header is compiled in. --check only says whether the header is up to date
(exit 1 if not).

A model's joints: the bind pose comes from the skin's inverse bind matrices (node transforms
aren't used, as glTF skinning ignores the mesh node's), so the vertices and joints share one
space, in metres. A WebXR joint the model lacks (a finger's metacarpal) is put a third of the
way from the wrist to its knuckle, with no weights; a joint of the model's WebXR lacks (an
extra root, the palm) gives its weights to the one EXTRA names. A tip that isn't one of the
skin's joints (a bone's end node) is placed from its parent joint's bind pose. A hand marked
"mirror" is the other side's file mirrored across x.

Standard library only. The glb is read as data (JSON and little-endian arrays), nothing in
it is run.
"""
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets", "hands")
OUT = os.path.join(ROOT, "handasset.h")

# WebXR's hand joints, in its order.
FINGERS = ("index-finger", "middle-finger", "ring-finger", "pinky-finger")
JOINTS = ["wrist",
          "thumb-metacarpal", "thumb-phalanx-proximal", "thumb-phalanx-distal", "thumb-tip"]
for f in FINGERS:
    JOINTS += [f + "-metacarpal", f + "-phalanx-proximal", f + "-phalanx-intermediate", f + "-phalanx-distal", f + "-tip"]
POS_STEP = 1e-5   # metres per unit of a quantized position


def webxr_names():
    return {j: j for j in JOINTS}


def finger_names(wrist, thumb, fingers, parts):
    """A model's names for WebXR's joints: `thumb` its four, `fingers` four finger names each
    with `parts` (five: metacarpal to tip; None for a joint it lacks)."""
    m = {"wrist": wrist}
    for w, n in zip(JOINTS[1:5], thumb):
        m[w] = n
    for f, name in zip(FINGERS, fingers):
        for k, part in enumerate(parts):
            m[JOINTS[5 + 5 * FINGERS.index(f) + k]] = part.format(f=name) if part else None
    return m


def suffixed(names, side):
    return {k: (v + side if v else None) for k, v in names.items()}


def mrtk2_names(side):
    m = finger_names("Wrist", ["Thumb_1", "Thumb_2", "Thumb_3", "Thumb_end"], ["Pointer", "Middle", "Ring", "Pinky"],
                     [None, "{f}_1", "{f}_2", "{f}_3", "{f}_end"])
    m["pinky-finger-metacarpal"] = "WristPadding"
    return {k: (side + "_" + v if v else None) for k, v in m.items()}


# name, folder under ASSETS, files (left, right; "mirror" for the other side's mirrored),
# names(side letter "L"/"R") -> {WebXR joint: model node or None}, extra joints' weights.
MODELS = [
    # WebXR Input Profiles' generic-hand (MIT): the default
    ("asset", "", ("left.glb", "right.glb"), lambda s: webxr_names(), {}),
    # Babylon.js hand meshes (CC BY 4.0)
    ("babylon", "babylon", ("left.glb", "right.glb"),
     lambda s: suffixed(finger_names("wrist_", ["thumb_metacarpal_", "thumb_proxPhalanx_", "thumb_distPhalanx_", "thumb_tip_"],
                                     ["index", "middle", "ring", "little"],
                                     ["{f}_metacarpal_", "{f}_proxPhalanx_", "{f}_intPhalanx_", "{f}_distPhalanx_", "{f}_tip_"]), s),
     {"root_{s}": "wrist"}),
    # MRTK3's OpenXR hand (BSD-3-Clause), right only. Its joints are named for the bone that
    # starts at them, a joint along from WebXR's: its "index_proximal" sits at the metacarpal's
    # base, and the bones' ends (index_tip_end) are the tips. Its metacarpal bones run from the wrist.
    ("mrtk3", "mrtk3", ("mirror", "right.glb"),
     lambda s: finger_names("middle_metacarpal", ["thumb_proximal", "thumb_distal", "thumb_tip", "thumb_tip_end"],
                            ["index", "middle", "ring", "little"],
                            ["{f}_proximal", "{f}_intermediate", "{f}_distal", "{f}_tip", "{f}_tip_end"]),
     {"thumb_metacarpal": "wrist", "index_metacarpal": "wrist", "ring_metacarpal": "wrist", "little_metacarpal": "wrist"}),
    # Godot XR Tools' hands (CC0), OpenXR's joints
    ("godot", "godot", ("left.glb", "right.glb"),
     lambda s: suffixed(finger_names("Wrist_", ["Thumb_Metacarpal_", "Thumb_Proximal_", "Thumb_Distal_", "Thumb_Tip_"],
                                     ["Index", "Middle", "Ring", "Little"],
                                     ["{f}_Metacarpal_", "{f}_Proximal_", "{f}_Intermediate_", "{f}_Distal_", "{f}_Tip_"]), s),
     {"Palm_{s}": "wrist"}),
    # MRTK2's rigged hand (MIT): three bones and an end per finger, no metacarpals but the
    # little finger's ("WristPadding")
    ("mrtk2", "mrtk2", ("left.glb", "right.glb"),
     lambda s: mrtk2_names(s),
     {}),
]

def read_glb(path):
    data = open(path, "rb").read()
    magic, version, length = struct.unpack_from("<4sII", data, 0)
    if magic != b"glTF" or version != 2 or length != len(data):
        sys.exit(f"{path}: not a glTF 2 binary")
    off, chunks = 12, []
    while off < length:
        size, kind = struct.unpack_from("<II", data, off)
        chunks.append((kind, data[off + 8:off + 8 + size]))
        off += 8 + ((size + 3) & ~3)
    if len(chunks) < 2 or chunks[0][0] != 0x4E4F534A or chunks[1][0] != 0x004E4942:
        sys.exit(f"{path}: expected a JSON and a BIN chunk")
    gltf, blob = json.loads(chunks[0][1]), chunks[1][1]

    def accessor(i):
        a = gltf["accessors"][i]
        view = gltf["bufferViews"][a["bufferView"]]
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[a["type"]]
        fmt = {5126: "f", 5121: "B", 5123: "H", 5125: "I"}[a["componentType"]]
        step = view.get("byteStride", struct.calcsize(fmt) * n)
        base = view.get("byteOffset", 0) + a.get("byteOffset", 0)
        return [struct.unpack_from("<" + fmt * n, blob, base + k * step) for k in range(a["count"])]
    return gltf, accessor


def inverse_affine(m):
    """A column-major 4x4 affine transform's inverse, as rows (3x4)."""
    r = [[m[c * 4 + row] for c in range(3)] for row in range(3)]
    t = [m[12], m[13], m[14]]
    det = (r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
           r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]))
    inv = [[(r[(j + 1) % 3][(i + 1) % 3] * r[(j + 2) % 3][(i + 2) % 3] -
             r[(j + 1) % 3][(i + 2) % 3] * r[(j + 2) % 3][(i + 1) % 3]) / det for j in range(3)] for i in range(3)]
    return [inv[i] + [-sum(inv[i][k] * t[k] for k in range(3))] for i in range(3)]


def node_matrix(n):
    """A node's local transform, as rows (3x4)."""
    if "matrix" in n:
        m = n["matrix"]
        return [[m[c * 4 + r] for c in range(4)] for r in range(3)]
    x, y, z, w = n.get("rotation", [0, 0, 0, 1])
    sx, sy, sz = n.get("scale", [1, 1, 1])
    t = n.get("translation", [0, 0, 0])
    r = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    return [[r[i][0] * sx, r[i][1] * sy, r[i][2] * sz, t[i]] for i in range(3)]


def convert(model, side):
    """One hand of a model: (vertices, normals, joints, weights, triangles, bind) as read."""
    name, folder, files, names, extra = model
    letter = "LR"[side]
    path, mirror = files[side], False
    if path == "mirror":
        path, mirror = files[1 - side], True
        letter = "LR"[1 - side]
    path = os.path.join(ASSETS, folder, path)
    gltf, accessor = read_glb(path)
    if len(gltf.get("meshes", [])) != 1 or len(gltf.get("skins", [])) != 1:
        sys.exit(f"{path}: expected one mesh and one skin")
    skin = gltf["skins"][0]
    nodes = gltf["nodes"]
    node_of = {n.get("name"): i for i, n in enumerate(nodes)}
    parent = {c: i for i, n in enumerate(nodes) for c in n.get("children", [])}
    # each skin joint's bind pose (inverse bind matrix's inverse), rows 3x4
    ibm = accessor(skin["inverseBindMatrices"])
    bind_of = {j: inverse_affine(ibm[k]) for k, j in enumerate(skin["joints"])}

    def position(node):
        if node in bind_of:
            return [bind_of[node][i][3] for i in range(3)]
        p = parent.get(node)
        if p not in bind_of:
            sys.exit(f"{path}: {nodes[node].get('name')} is neither a joint nor a joint's child")
        local = node_matrix(nodes[node])
        return [sum(bind_of[p][i][k] * local[k][3] for k in range(3)) + bind_of[p][i][3] for i in range(3)]

    want = names(letter)
    extra = {k.format(s=letter): v for k, v in extra.items()}
    bind, target = {}, {}   # WebXR joint -> position; skin joint index -> WebXR joint
    for w in JOINTS:
        n = want.get(w)
        if n is None:
            continue
        if n not in node_of:
            sys.exit(f"{path}: no node {n} (for {w})")
        bind[w] = position(node_of[n])
    for k, j in enumerate(skin["joints"]):
        n = nodes[j].get("name")
        mapped = [w for w in JOINTS if want.get(w) == n]
        if mapped:
            target[k] = JOINTS.index(mapped[0])
        elif n in extra:
            target[k] = JOINTS.index(extra[n])
        else:
            sys.exit(f"{path}: the skin's joint {n} isn't mapped to WebXR's")
    for f in FINGERS:   # a missing metacarpal: a third of the way to the knuckle
        m, k = f + "-metacarpal", f + "-phalanx-proximal"
        if m not in bind:
            bind[m] = [a + (b - a) / 3 for a, b in zip(bind["wrist"], bind[k])]
    prim = gltf["meshes"][0]["primitives"]
    if len(prim) != 1 or prim[0].get("mode", 4) != 4:
        sys.exit(f"{path}: expected one primitive of triangles")
    attrs = prim[0]["attributes"]
    pos, nrm = accessor(attrs["POSITION"]), accessor(attrs["NORMAL"])
    joints, weights = accessor(attrs["JOINTS_0"]), accessor(attrs["WEIGHTS_0"])
    idx = [i[0] for i in accessor(prim[0]["indices"])]
    if "JOINTS_1" in attrs or len(pos) > 65535 or len(idx) % 3:
        sys.exit(f"{path}: more than four joints a vertex, too many vertices, or not whole triangles")
    # the model's joints onto WebXR's: a vertex's weights on joints that land on the same one add up
    vj, vw = [], []
    for j, w in zip(joints, weights):
        acc = {}
        for jj, ww in zip(j, w):
            if ww > 0:
                acc[target[jj]] = acc.get(target[jj], 0) + ww
        top = sorted(acc.items(), key=lambda kv: -kv[1])[:4]
        top += [(0, 0)] * (4 - len(top))
        vj.append([t for t, _ in top])
        vw.append([x for _, x in top])
    bind = [bind[w] for w in JOINTS]
    if mirror:
        pos = [(-x, y, z) for x, y, z in pos]
        nrm = [(-x, y, z) for x, y, z in nrm]
        bind = [[-p[0], p[1], p[2]] for p in bind]
        idx = [idx[k + d] for k in range(0, len(idx), 3) for d in (0, 2, 1)]
    return pos, nrm, vj, vw, idx, bind


def quantize(model, side):
    path = f"{model[0]} {'left' if side == 0 else 'right'}"
    pos, nrm, joints, weights, idx, bind = convert(model, side)
    qpos = []
    for p in pos:
        q = [round(c / POS_STEP) for c in p]
        if any(abs(c) > 32767 for c in q):
            sys.exit(f"{path}: a vertex is out of range")
        qpos += q
    qnrm = []
    for n in nrm:
        length = sum(c * c for c in n) ** 0.5 or 1
        qnrm += [max(-127, min(127, round(127 * c / length))) for c in n]
    qj, qw = [], []
    for j, w in zip(joints, weights):
        total = sum(w) or 1
        exact = [255 * x / total for x in w]
        q = [int(x) for x in exact]
        # largest remainders first, so the four add up to exactly 255
        for k in sorted(range(4), key=lambda k: exact[k] - q[k], reverse=True)[:255 - sum(q)]:
            q[k] += 1
        qj += [jj if ww else 0 for jj, ww in zip(j, q)]
        qw += q
    # a hand, in metres: wrist to middle fingertip 12-25 cm
    length = sum((a - b) ** 2 for a, b in zip(bind[0], bind[14])) ** 0.5
    if not 0.12 < length < 0.25:
        sys.exit(f"{path}: wrist to middle fingertip {length:.3f} m: not a hand in metres")
    return {"pos": qpos, "nrm": qnrm, "joints": qj, "weights": qw, "idx": idx,
            "bind": bind, "verts": len(pos), "tris": len(idx) // 3}


def array(ctype, name, values):
    lines, line = [], ""
    for v in values:
        s = f"{v},"
        if len(line) + len(s) > 118:
            lines.append(line)
            line = ""
        line += s
    lines.append(line)
    return f"inline const {ctype} {name}[] = {{\n" + "\n".join(lines) + "\n};\n"


def header():
    out = ["// The rendered hands' models, generated by screens/tools/hand-asset.py from the glTF files",
           "// in screens/assets/hands (their sources and licences: the README.md and LICENSE files there).",
           "// Don't edit: run the script again.",
           "#pragma once", "", "#include <cstdint>", "", "namespace handasset {", "",
           "constexpr int kJoints = 25;   // WebXR's hand joints, in its order",
           f"constexpr float kPosStep = {POS_STEP}f;   // metres per unit of pos", "",
           "struct Hand {",
           "    int verts, tris;",
           "    const int16_t *pos;       // verts x 3, kPosStep each",
           "    const int8_t *nrm;        // verts x 3, out of 127",
           "    const uint8_t *joints;    // verts x 4: joints (WebXR order)",
           "    const uint8_t *weights;   // verts x 4: their weights, out of 255 (they add up to 255)",
           "    const uint16_t *idx;      // tris x 3",
           "    float bind[kJoints][3];   // the joints in the bind pose, metres",
           "};", "",
           "struct Model {",
           "    const char *name;   // HANDS_VIEW_MODEL",
           "    Hand hands[2];      // left, right",
           "};", ""]
    entries = []
    for model in MODELS:
        name = model[0]
        sides = []
        for side in (0, 1):
            h = quantize(model, side)
            p = name.capitalize() + ("Left", "Right")[side]
            out += [array("int16_t", f"k{p}Pos", h["pos"]), array("int8_t", f"k{p}Nrm", h["nrm"]),
                    array("uint8_t", f"k{p}Joints", h["joints"]), array("uint8_t", f"k{p}Weights", h["weights"]),
                    array("uint16_t", f"k{p}Idx", h["idx"])]
            bind = ", ".join("{" + ", ".join(f"{c:.6f}f" for c in j) + "}" for j in h["bind"])
            sides.append(f"{{{h['verts']}, {h['tris']}, k{p}Pos, k{p}Nrm, k{p}Joints, k{p}Weights, k{p}Idx, {{{bind}}}}}")
        entries.append(f'    {{"{name}", {{{sides[0]},\n' + " " * 17 + f"{sides[1]}}}}},")
    out.append(f"constexpr int kModels = {len(MODELS)};")
    out.append("inline const Model kModel[kModels] = {")
    out += entries
    out += ["};", "", "}  // namespace handasset", ""]
    return "\n".join(out)


def main():
    text = header()
    if "--check" in sys.argv[1:]:
        same = os.path.exists(OUT) and open(OUT).read() == text
        print(f"{OUT}: {'up to date' if same else 'out of date'}")
        return 0 if same else 1
    with open(OUT, "w") as f:
        f.write(text)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
