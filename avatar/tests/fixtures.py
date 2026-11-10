"""Made-up models for the importer's tests: an arm or two with a hand of boxes, written as a
GLB (plain, or a VRM 0.x or 1.0), or as an ASCII FBX. No model from anywhere else is needed in
the repo, and the test knows where every joint is.

A rig is in metres, y up, the left arm along +x. Each bone moves the tube around it; the chest
moves a box that must stay out of a hand, the forearm a tube whose end is the hand's cuff."""
import json
import math
import struct

FINGERS = ('thumb', 'index', 'middle', 'ring', 'little')
PARTS = ('proximal', 'intermediate', 'distal')
# How rigs name a hand's bones, by who made them: (hand, forearm, finger bone), each from the
# side ('left', 'right'), the finger and the bone's number (0 a metacarpal, 1-3, 4 a tip).
SCHEMES = {
    'mixamo': (lambda s: f'mixamorig:{s.title()}Hand', lambda s: f'mixamorig:{s.title()}ForeArm',
               lambda s, f, k: f'mixamorig:{s.title()}Hand{ {"little": "Pinky"}.get(f, f.title()) }{k}'),
    'unity': (lambda s: f'{s.title()}Hand', lambda s: f'{s.title()}LowerArm',
              lambda s, f, k: f'{s.title()}{f.title()}{("Metacarpal", "Proximal", "Intermediate", "Distal", "Tip")[k]}'),
    'blender': (lambda s: f'hand.{s[0].upper()}', lambda s: f'forearm.{s[0].upper()}',
                lambda s, f, k: (f'thumb.0{k}.{s[0].upper()}' if f == 'thumb' else
                                 f'f_{ {"little": "pinky"}.get(f, f) }.0{k}.{s[0].upper()}')),
    'unreal': (lambda s: f'hand_{s[0]}', lambda s: f'lowerarm_{s[0]}',
               lambda s, f, k: f'{ {"little": "pinky"}.get(f, f) }_{"metacarpal" if k == 0 else "0%d" % k}_{s[0]}'),
    'opaque': (lambda s: f'bone_{s}_h', lambda s: f'bone_{s}_f', lambda s, f, k: f'bone_{s}_{FINGERS.index(f)}{k}'),
}


class Rig:
    def __init__(self):
        self.nodes = []      # (name, parent, (x, y, z)) in the rest pose
        self.verts = []      # ((x, y, z), [(node, weight), ...], its normal)
        self.tris = []
        self.humanoid = {}   # VRM 1.0's bone name -> node
        self.joints = {}     # side -> {WebXR joint number: (x, y, z)}: where the test expects them

    def node(self, name, parent, at):
        self.nodes.append((name, parent, tuple(at)))
        return len(self.nodes) - 1

    def tube(self, node, a, b, half, sides=4, rings=1, cap=True):
        """A tube from a to b along x (its ends closed when `cap`), moved by one node."""
        first = len(self.verts)
        for r in range(rings + 1):
            x = a[0] + (b[0] - a[0]) * r / rings
            for k in range(sides):
                t = 2 * math.pi * (k + 0.5) / sides
                self.verts.append(((x, a[1] + half * math.sin(t), a[2] + half * math.cos(t)), [(node, 1.0)],
                                   (0.0, math.sin(t), math.cos(t))))
        flip = b[0] < a[0]   # the right arm's tubes run the other way: keep the faces outward
        def tri(p, q, s):
            self.tris.append((p, s, q) if flip else (p, q, s))
        for r in range(rings):
            for k in range(sides):
                p, q = first + r * sides + k, first + r * sides + (k + 1) % sides
                tri(p, q + sides, q), tri(p, p + sides, q + sides)
        if cap:
            for ring, turn in ((0, True), (rings, False)):
                base = first + ring * sides
                for k in range(1, sides - 1):
                    tri(base, base + k, base + k + 1) if turn else tri(base, base + k + 1, base + k)


def humanoid_name(side, finger, k):
    """VRM 1.0's name for a finger's bone number k (0 the metacarpal: only the thumb has one)."""
    if finger == 'thumb':
        return side + 'Thumb' + ('Metacarpal', 'Proximal', 'Distal')[k - 1]
    return side + finger.title() + ('Proximal', 'Intermediate', 'Distal')[k - 1]


def make(scheme='unity', sides=('left', 'right'), metacarpals=False, tips=False, scale=1.0, dense=False,
         without=(), finger_bones=3, palm_bones=False):
    """A rig. without: fingers left out. finger_bones: how many each finger but the thumb has (3 as
    people's do). palm_bones: a bone named "palm" between the hand and each finger (Rigify's)."""
    hand_name, fore_name, bone_name = SCHEMES[scheme]
    rig = Rig()
    S = scale
    root = rig.node('Armature', -1, (0, 0, 0))
    hips = rig.node('Hips', root, (0, 1.0 * S, 0))
    chest = rig.node('Chest', hips, (0, 1.3 * S, 0))
    rig.humanoid.update(hips=hips, chest=chest)
    rig.tube(chest, (-0.15 * S, 1.3 * S, 0), (0.15 * S, 1.3 * S, 0), 0.12 * S)
    tube = dict(sides=32, rings=12) if dense else dict(sides=6, rings=2)
    for side in sides:
        sx = 1 if side == 'left' else -1
        at = lambda x, y=0.0, z=0.0: ((0.66 + x) * sx * S, (1.4 + y) * S, z * S)   # from the wrist
        upper = rig.node(f'{side.title()}UpperArm', chest, at(-0.5))
        fore = rig.node(fore_name(side), upper, at(-0.24))
        hand = rig.node(hand_name(side), fore, at(0))
        rig.humanoid[side + 'UpperArm'], rig.humanoid[side + 'LowerArm'], rig.humanoid[side + 'Hand'] = upper, fore, hand
        rig.tube(upper, at(-0.5), at(-0.24), 0.04 * S, cap=False)
        rig.tube(fore, at(-0.24), at(0), 0.03 * S, sides=8, rings=12, cap=False)   # a ring every 2 cm: the cuff's
        rig.tube(hand, at(0), at(0.09), 0.035 * S)
        joints = rig.joints.setdefault(side, {0: at(0)})
        for f, finger in enumerate(FINGERS):
            if finger in without:
                continue
            z = (0.07, 0.03, 0.01, -0.01, -0.03)[f]
            if finger == 'thumb':
                xs, numbers, base = (0.025, 0.055, 0.085, 0.11), (1, 2, 3), 1
            else:
                xs, numbers, base = (0.09, 0.13, 0.155, 0.175), (1, 2, 3)[:finger_bones], 5 * f + 1
            parent = hand
            if palm_bones and finger != 'thumb':
                parent = rig.node(f'palm.0{f}.{side[0].upper()}', hand, at(0.03, 0, z))
            if metacarpals and finger != 'thumb':
                parent = rig.node(bone_name(side, finger, 0), parent, at(0.0225, 0, z))
                joints[5 * f] = at(0.0225, 0, z)
            elif finger != 'thumb':
                joints[5 * f] = tuple(w + (k - w) / 3 for w, k in zip(at(0), at(xs[0], 0, z)))
            for i, k in enumerate(numbers):
                parent = rig.node(bone_name(side, finger, k), parent, at(xs[i], 0, z))
                joints[base + i] = at(xs[i], 0, z)
                if finger_bones == 3 or finger == 'thumb':
                    rig.humanoid[humanoid_name(side, finger, k)] = parent
                rig.tube(parent, at(xs[i], 0, z), at(xs[i + 1], 0, z), 0.008 * S, **tube)
            joints[base + len(numbers)] = at(xs[len(numbers)], 0, z)   # the tip: where the last tube ends
            if tips:
                rig.node(bone_name(side, finger, 4), parent, at(xs[len(numbers)], 0, z))
    return rig


def _pad(b, fill=b'\0'):
    return b + fill * (-len(b) % 4)


def glb(rig, vrm=None, meta=None, normals=True):
    """The rig as a GLB's bytes. vrm: None, 0 or 1 (the humanoid and `meta` in that version's
    extension, with VRM 0.x's own names for the thumb's bones)."""
    world = [n[2] for n in rig.nodes]
    nodes = []
    for i, (name, parent, at) in enumerate(rig.nodes):
        local = at if parent < 0 else tuple(a - b for a, b in zip(at, world[parent]))
        node = {'name': name, 'translation': list(local)}
        kids = [k for k, n in enumerate(rig.nodes) if n[1] == i]
        if kids:
            node['children'] = kids
        nodes.append(node)
    mesh_node = len(nodes)
    nodes.append({'name': 'Body', 'mesh': 0, 'skin': 0})
    blob, views, accessors = b'', [], []

    def add(data, component, kind, count, target=None, bounds=None):
        nonlocal blob
        view = {'buffer': 0, 'byteOffset': len(blob), 'byteLength': len(data)}
        if target:
            view['target'] = target
        views.append(view)
        blob = _pad(blob + data)
        acc = {'bufferView': len(views) - 1, 'componentType': component, 'count': count, 'type': kind}
        if bounds:
            acc['min'], acc['max'] = bounds
        accessors.append(acc)
        return len(accessors) - 1

    pos = [v[0] for v in rig.verts]
    attributes = {'POSITION': add(b''.join(struct.pack('<3f', *p) for p in pos), 5126, 'VEC3', len(pos), 34962,
                                  ([min(p[k] for p in pos) for k in range(3)], [max(p[k] for p in pos) for k in range(3)]))}
    if normals:
        attributes['NORMAL'] = add(b''.join(struct.pack('<3f', *v[2]) for v in rig.verts), 5126, 'VEC3', len(pos), 34962)
    joints = b''.join(struct.pack('<4H', *([n for n, _ in v[1]] + [0] * 4)[:4]) for v in rig.verts)
    weights = b''.join(struct.pack('<4f', *([w for _, w in v[1]] + [0.0] * 4)[:4]) for v in rig.verts)
    attributes['JOINTS_0'] = add(joints, 5123, 'VEC4', len(pos), 34962)
    attributes['WEIGHTS_0'] = add(weights, 5126, 'VEC4', len(pos), 34962)
    indices = add(b''.join(struct.pack('<3I', *t) for t in rig.tris), 5125, 'SCALAR', len(rig.tris) * 3, 34963)
    ibm = add(b''.join(struct.pack('<16f', 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -w[0], -w[1], -w[2], 1) for w in world),
              5126, 'MAT4', len(world))
    doc = {
        'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': [0, mesh_node]}], 'nodes': nodes,
        'meshes': [{'primitives': [{'attributes': attributes, 'indices': indices}]}],
        'skins': [{'joints': list(range(len(world))), 'inverseBindMatrices': ibm}],
        'buffers': [{'byteLength': len(blob)}], 'bufferViews': views, 'accessors': accessors,
    }
    meta = meta or {}
    if vrm == 0:
        def old(name):   # 0.x's thumb names are one bone along from 1.0's
            return name.replace('ThumbProximal', 'ThumbIntermediate').replace('ThumbMetacarpal', 'ThumbProximal')
        doc['extensionsUsed'] = ['VRM']
        doc['extensions'] = {'VRM': {'specVersion': '0.0', 'meta': meta, 'humanoid': {
            'humanBones': [{'bone': old(name), 'node': node} for name, node in rig.humanoid.items()]}}}
    elif vrm == 1:
        doc['extensionsUsed'] = ['VRMC_vrm']
        doc['extensions'] = {'VRMC_vrm': {'specVersion': '1.0', 'meta': meta, 'humanoid': {
            'humanBones': {name: {'node': node} for name, node in rig.humanoid.items()}}}}
    text = _pad(json.dumps(doc).encode(), b' ')
    return (struct.pack('<4sII', b'glTF', 2, 12 + 8 + len(text) + 8 + len(blob)) +
            struct.pack('<I4s', len(text), b'JSON') + text + struct.pack('<I4s', len(blob), b'BIN\0') + blob)


def fbx(rig, unit_cm=True):
    """The rig as an ASCII FBX 7.4 file's text: in centimetres when unit_cm, as most are."""
    k = 100.0 if unit_cm else 1.0
    num = lambda v: ('%.6f' % v).rstrip('0').rstrip('.')
    out = ['; FBX 7.4.0 project file', 'FBXHeaderExtension:  {', '\tFBXHeaderVersion: 1003', '\tFBXVersion: 7400', '}',
           'GlobalSettings:  {', '\tVersion: 1000', '\tProperties70:  {',
           '\t\tP: "UpAxis", "int", "Integer", "",1', '\t\tP: "UpAxisSign", "int", "Integer", "",1',
           '\t\tP: "FrontAxis", "int", "Integer", "",2', '\t\tP: "FrontAxisSign", "int", "Integer", "",1',
           '\t\tP: "CoordAxis", "int", "Integer", "",0', '\t\tP: "CoordAxisSign", "int", "Integer", "",1',
           '\t\tP: "UnitScaleFactor", "double", "Number", "",%s' % (1 if unit_cm else 100), '\t}', '}', 'Objects:  {']
    flat = [num(c * k) for v in rig.verts for c in v[0]]
    index = [str(i if n < 2 else -i - 1) for t in rig.tris for n, i in enumerate(t)]
    out += ['\tGeometry: 1000, "Geometry::Body", "Mesh" {',
            '\t\tVertices: *%d {' % len(flat), '\t\t\ta: ' + ','.join(flat), '\t\t}',
            '\t\tPolygonVertexIndex: *%d {' % len(index), '\t\t\ta: ' + ','.join(index), '\t\t}', '\t}',
            '\tModel: 2000, "Model::Body", "Mesh" {', '\t\tVersion: 232', '\t}',
            '\tDeformer: 5000, "Deformer::Skin", "Skin" {', '\t\tVersion: 101', '\t}']
    links = ['\tC: "OO",2000,0', '\tC: "OO",1000,2000', '\tC: "OO",5000,1000']
    world = [n[2] for n in rig.nodes]
    for i, (name, parent, at) in enumerate(rig.nodes):
        local = at if parent < 0 else tuple(a - b for a, b in zip(at, world[parent]))
        out += ['\tModel: %d, "Model::%s", "LimbNode" {' % (3000 + i, name), '\t\tVersion: 232', '\t\tProperties70:  {',
                '\t\t\tP: "Lcl Translation", "Lcl Translation", "", "A",' + ','.join(num(c * k) for c in local), '\t\t}', '\t}']
        links.append('\tC: "OO",%d,%d' % (3000 + i, 0 if parent < 0 else 3000 + parent))
        moved = [(v, w) for v, vert in enumerate(rig.verts) for n, w in vert[1] if n == i]
        if not moved:
            continue
        there = ','.join(num(c * k) for c in at)
        back = ','.join(num(-c * k) for c in at)
        out += ['\tDeformer: %d, "SubDeformer::%s", "Cluster" {' % (6000 + i, name), '\t\tVersion: 100',
                '\t\tIndexes: *%d {' % len(moved), '\t\t\ta: ' + ','.join(str(v) for v, _ in moved), '\t\t}',
                '\t\tWeights: *%d {' % len(moved), '\t\t\ta: ' + ','.join(num(w) for _, w in moved), '\t\t}',
                '\t\tTransform: *16 {', '\t\t\ta: 1,0,0,0,0,1,0,0,0,0,1,0,%s,1' % back, '\t\t}',
                '\t\tTransformLink: *16 {', '\t\t\ta: 1,0,0,0,0,1,0,0,0,0,1,0,%s,1' % there, '\t\t}', '\t}']
        links += ['\tC: "OO",%d,5000' % (6000 + i), '\tC: "OO",%d,%d' % (3000 + i, 6000 + i)]
    out += ['}', 'Connections:  {'] + links + ['}', '']
    return '\n'.join(out)
