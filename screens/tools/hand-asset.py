#!/usr/bin/env python3
"""The rendered hands' model (ft-handview, screens/handskin.h) from its glTF files.

    screens/tools/hand-asset.py [--check]

Reads screens/assets/hands/left.glb and right.glb (the WebXR Input Profiles' generic-hand,
MIT; see screens/assets/hands/README.md) and writes screens/handasset.h: per hand the
vertices (positions in 0.01 mm steps, normals in 1/127 steps), each vertex's four joints and
weights (out of 255), the triangles, and the 25 joints' positions in the bind pose (WebXR's
joint order). The program needs no glTF code: the header is compiled in. --check only says
whether the header is up to date (exit 1 if not).

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

# WebXR's hand joints, in its order (the skin's joints are matched to these by name).
JOINTS = ["wrist",
          "thumb-metacarpal", "thumb-phalanx-proximal", "thumb-phalanx-distal", "thumb-tip"]
for f in ("index-finger", "middle-finger", "ring-finger", "pinky-finger"):
    JOINTS += [f + "-metacarpal", f + "-phalanx-proximal", f + "-phalanx-intermediate", f + "-phalanx-distal", f + "-tip"]
POS_STEP = 1e-5   # metres per unit of a quantized position


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
    """A column-major 4x4 rigid-ish transform's inverse, as rows (3x4)."""
    r = [[m[c * 4 + row] for c in range(3)] for row in range(3)]
    t = [m[12], m[13], m[14]]
    det = (r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) +
           r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]))
    inv = [[(r[(j + 1) % 3][(i + 1) % 3] * r[(j + 2) % 3][(i + 2) % 3] -
             r[(j + 1) % 3][(i + 2) % 3] * r[(j + 2) % 3][(i + 1) % 3]) / det for j in range(3)] for i in range(3)]
    return [inv[i] + [-sum(inv[i][k] * t[k] for k in range(3))] for i in range(3)]


def convert(path):
    gltf, accessor = read_glb(path)
    if len(gltf.get("meshes", [])) != 1 or len(gltf.get("skins", [])) != 1:
        sys.exit(f"{path}: expected one mesh and one skin")
    skin = gltf["skins"][0]
    names = [gltf["nodes"][j].get("name") for j in skin["joints"]]
    if sorted(names) != sorted(JOINTS):
        sys.exit(f"{path}: the skin's joints aren't WebXR's 25: {names}")
    # the mesh's node and the joints sit directly under one untransformed root: the vertices,
    # the joints and the bind matrices are all in the same (model) space
    for node in gltf["nodes"]:
        if node.get("mesh") is not None and any(k in node for k in ("translation", "rotation", "scale", "matrix")):
            sys.exit(f"{path}: the mesh's node is transformed")
        if node.get("children") and any(k in node for k in ("translation", "rotation", "scale", "matrix")):
            sys.exit(f"{path}: a parent node is transformed")
    ibm = accessor(skin["inverseBindMatrices"])
    bind = {}
    for k, name in enumerate(names):
        world = inverse_affine(ibm[k])
        bind[name] = [world[0][3], world[1][3], world[2][3]]
    remap = [JOINTS.index(n) for n in names]   # the skin's joint k -> WebXR order
    prim = gltf["meshes"][0]["primitives"][0]
    if prim.get("mode", 4) != 4:
        sys.exit(f"{path}: not triangles")
    attrs = prim["attributes"]
    pos, nrm = accessor(attrs["POSITION"]), accessor(attrs["NORMAL"])
    joints, weights = accessor(attrs["JOINTS_0"]), accessor(attrs["WEIGHTS_0"])
    idx = [i[0] for i in accessor(prim["indices"])]
    if len(pos) > 65535 or len(idx) % 3:
        sys.exit(f"{path}: too many vertices, or not whole triangles")
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
        qj += [remap[jj] if ww else 0 for jj, ww in zip(j, q)]
        qw += q
    return {"pos": qpos, "nrm": qnrm, "joints": qj, "weights": qw, "idx": idx,
            "bind": [bind[n] for n in JOINTS], "verts": len(pos), "tris": len(idx) // 3}


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
    out = ["// The rendered hands' model, generated by screens/tools/hand-asset.py from",
           "// screens/assets/hands/left.glb and right.glb (WebXR Input Profiles' generic-hand, MIT,",
           "// screens/assets/hands/README.md and LICENSE). Don't edit: run the script again.",
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
           "};", ""]
    hands = {}
    for side in ("left", "right"):
        h = hands[side] = convert(os.path.join(ASSETS, side + ".glb"))
        p = side.capitalize()
        out += [array("int16_t", f"k{p}Pos", h["pos"]), array("int8_t", f"k{p}Nrm", h["nrm"]),
                array("uint8_t", f"k{p}Joints", h["joints"]), array("uint8_t", f"k{p}Weights", h["weights"]),
                array("uint16_t", f"k{p}Idx", h["idx"])]
    out.append("inline const Hand kHands[2] = {   // left, right")
    for side in ("left", "right"):
        h, p = hands[side], side.capitalize()
        bind = ", ".join("{" + ", ".join(f"{c:.6f}f" for c in j) + "}" for j in h["bind"])
        out.append(f"    {{{h['verts']}, {h['tris']}, k{p}Pos, k{p}Nrm, k{p}Joints, k{p}Weights, k{p}Idx, {{{bind}}}}},")
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
