"""A made-up whole person for the importer's tests: fixtures.py's arms and hands with a spine, a
head, legs and feet of boxes, materials with small pictures, and face shapes. The test knows
where every bone is (Person.bones) and where the body's file must have it (Person.placed).

The person is built in metres, y up, standing on y = 0 with the hips over the origin and the
left arm along +x (so it faces +z), then lifted, moved and turned as asked: the importer has to
undo all of that."""
import math
import struct
import zlib

import fixtures

SIDES = ('left', 'right')
# How rigs name a body's bones, by who made them. {S} is Left or Right, {s} is l or r.
NAMES = {
    'unity': dict(hips='Hips', spine='Spine', chest='Chest', upperChest='UpperChest', neck='Neck', head='Head', eye='{S}Eye',
                  shoulder='{S}Shoulder', upperArm='{S}UpperArm', upperLeg='{S}UpperLeg', lowerLeg='{S}LowerLeg', foot='{S}Foot',
                  toes='{S}Toes'),
    'mixamo': dict(hips='mixamorig:Hips', spine='mixamorig:Spine', chest='mixamorig:Spine1', upperChest='mixamorig:Spine2',
                   neck='mixamorig:Neck', head='mixamorig:Head', eye='mixamorig:{S}Eye', shoulder='mixamorig:{S}Shoulder',
                   upperArm='mixamorig:{S}Arm', upperLeg='mixamorig:{S}UpLeg', lowerLeg='mixamorig:{S}Leg', foot='mixamorig:{S}Foot',
                   toes='mixamorig:{S}ToeBase'),
    'blender': dict(hips='hips', spine='spine', chest='chest', upperChest='upper_chest', neck='neck', head='head', eye='eye.{s}',
                    shoulder='shoulder.{s}', upperArm='upper_arm.{s}', upperLeg='thigh.{s}', lowerLeg='shin.{s}', foot='foot.{s}',
                    toes='toe.{s}'),
    'unreal': dict(hips='pelvis', spine='spine_01', chest='spine_02', upperChest='spine_03', neck='neck_01', head='head',
                   eye='eye_{s}', shoulder='clavicle_{s}', upperArm='upperarm_{s}', upperLeg='thigh_{s}', lowerLeg='calf_{s}',
                   foot='foot_{s}', toes='ball_{s}'),
    'opaque': {role: 'bone_{s}_b%d' % n for n, role in enumerate(
        'hips spine chest upperChest neck head eye shoulder upperArm upperLeg lowerLeg foot toes'.split())},
}


def png(width, height, pixel):
    """A PNG file's bytes: pixel(x, y) is its (r, g, b, a), or `pixel` is the one colour of all."""
    if callable(pixel):
        rows = b''.join(b'\0' + bytes(c for x in range(width) for c in pixel(x, y)) for y in range(height))
    else:
        rows = (b'\0' + bytes(pixel) * width) * height

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(rows, 1)) + chunk(b'IEND', b''))


class Person(fixtures.Rig):
    def __init__(self):
        super().__init__()
        self.bones = {}      # the body file's role -> the bone's place, as built (standing at the origin)
        self.blocks = {}     # a block's name -> (its vertices, its triangles): two ranges
        self.built = []      # each vertex's place as built, and each shape's moves (by Frametop's names)
        self.moved = {}
        self.pictures = {}   # a picture's file name -> its bytes (an FBX names its pictures, it doesn't hold them)
        self.lift, self.shift, self.turn = 0.0, (0.0, 0.0), 0.0

    def placed(self, at):
        """Where the body's file has a place of the person as built: its right toward +x, facing -z."""
        return (-at[0], at[1], -at[2])

    def block(self, name, weights, lo, hi, cuts=1):
        """A box from lo to hi, cut across y, moved by `weights`: [(node, weight)], or a function
        of how far up the box a vertex is (0 to 1) that gives them."""
        first, first_tri = len(self.verts), len(self.tris)
        corners = ((lo[0], lo[2]), (hi[0], lo[2]), (hi[0], hi[2]), (lo[0], hi[2]))
        for r in range(cuts + 1):
            t = r / cuts
            for x, z in corners:
                nx, nz = (-1 if x == lo[0] else 1), (-1 if z == lo[2] else 1)
                self.verts.append(((x, lo[1] + (hi[1] - lo[1]) * t, z), list(weights(t) if callable(weights) else weights),
                                   (nx / math.sqrt(2), 0.0, nz / math.sqrt(2))))
                self.uv[len(self.verts) - 1] = ((x - lo[0]) / (hi[0] - lo[0]) if z == hi[2] else 0.0, 1 - t)
        for r in range(cuts):
            for k in range(4):
                p, q = first + r * 4 + k, first + r * 4 + (k + 1) % 4
                self.tris += [(p, q, q + 4), (p, q + 4, p + 4)]
        top = first + cuts * 4
        self.tris += [(first, first + 1, first + 2), (first, first + 2, first + 3), (top, top + 2, top + 1), (top, top + 3, top + 2)]
        centre = [(a + b) / 2 for a, b in zip(lo, hi)]
        for n in range(first_tri, len(self.tris)):   # every face outward
            p, q, s = (self.verts[i][0] for i in self.tris[n])
            u, v = [q[k] - p[k] for k in range(3)], [s[k] - p[k] for k in range(3)]
            normal = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
            if sum(normal[k] * ((p[k] + q[k] + s[k]) / 3 - centre[k]) for k in range(3)) < 0:
                self.tris[n] = (self.tris[n][0], self.tris[n][2], self.tris[n][1])
        self.blocks[name] = (range(first, len(self.verts)), range(first_tri, len(self.tris)))

    def paint(self, name, material):
        for n in self.blocks[name][1]:
            self.tri_material[n] = material

    def move(self, lift=0.0, shift=(0.0, 0.0), turn=0.0):
        """Lift the person off the floor, move it across it, and turn it about y (degrees)."""
        self.lift, self.shift, self.turn = lift, shift, turn
        c, s = math.cos(math.radians(turn)), math.sin(math.radians(turn))

        def turned(v):
            return (c * v[0] + s * v[2], v[1], -s * v[0] + c * v[2])

        def there(p):
            t = turned(p)
            return (t[0] + shift[0], t[1] + lift, t[2] + shift[1])

        self.nodes = [(name, parent, there(at)) for name, parent, at in self.nodes]
        self.verts = [(there(p), w, turned(n)) for p, w, n in self.verts]
        self.shapes = {name: {v: turned(d) for v, d in moves.items()} for name, moves in self.shapes.items()}
        return self


def person(scheme='unity', upper_chest=False, eyes=True, shoulders=True, toes=True, fingers=True, dense=False, extras=True,
           look=True, without=()):
    """A whole person. without: body roles left out (the bone isn't there at all). fingers: False
    for hands without any. extras: bones a humanoid doesn't have (hair, a helper beside the
    forearm), with meshes of their own, and a stand under the feet that the root moves. look:
    materials, pictures and face shapes."""
    names = NAMES[scheme]
    arms = fixtures.make(scheme, dense=dense, without=() if fingers else fixtures.FINGERS)
    rig = Person()
    for field in ('nodes', 'verts', 'tris', 'humanoid', 'joints'):
        setattr(rig, field, getattr(arms, field))

    def name(role, side='none'):
        return names[role].format(S=side.title(), s=side[0].upper() if scheme == 'blender' else side[0])

    def rename(node, new):
        rig.nodes[node] = (new, *rig.nodes[node][1:])

    def hang(node, parent):
        rig.nodes[node] = (rig.nodes[node][0], parent, rig.nodes[node][2])

    def bone(role, node_name, parent, at):
        if role in without:
            return parent
        node = rig.node(node_name, parent, at)
        rig.humanoid[role] = node
        return node

    hips, chest = rig.humanoid['hips'], rig.humanoid['chest']
    rename(hips, name('hips')), rename(chest, name('chest'))
    spine = bone('spine', name('spine'), hips, (0, 1.1, 0))
    hang(chest, spine)
    top = bone('upperChest', name('upperChest'), chest, (0, 1.38, 0)) if upper_chest else chest
    neck = bone('neck', name('neck'), top, (0, 1.5, 0))
    head = bone('head', name('head'), neck, (0, 1.58, 0))
    for side in SIDES:
        sx = 1 if side == 'left' else -1
        if eyes:
            bone(side + 'Eye', name('eye', side), head, (0.03 * sx, 1.64, 0.08))
        upper = rig.humanoid[side + 'UpperArm']
        rename(upper, name('upperArm', side))
        hang(upper, bone(side + 'Shoulder', name('shoulder', side), top, (0.04 * sx, 1.4, 0)) if shoulders else top)
        thigh = bone(side + 'UpperLeg', name('upperLeg', side), hips, (0.09 * sx, 0.95, 0))
        shin = bone(side + 'LowerLeg', name('lowerLeg', side), thigh, (0.09 * sx, 0.5, 0))
        foot = bone(side + 'Foot', name('foot', side), shin, (0.09 * sx, 0.08, 0))
        toe = bone(side + 'Toes', name('toes', side), foot, (0.09 * sx, 0.02, 0.12)) if toes else foot
        x = (0.03, 0.15) if side == 'left' else (-0.15, -0.03)
        rig.block(side + 'Thigh', lambda t: [(thigh, 1.0)] if t > 0 else [(thigh, 0.5), (shin, 0.5)], (x[0], 0.5, -0.06), (x[1], 0.95, 0.06), 3)
        rig.block(side + 'Shin', lambda t: [(shin, 1.0)] if t < 1 else [(thigh, 0.5), (shin, 0.5)], (x[0], 0.08, -0.05), (x[1], 0.5, 0.05), 3)
        rig.block(side + 'Foot', [(foot, 1.0)], (x[0], 0.0, -0.06), (x[1], 0.08, 0.12))
        rig.block(side + 'Toes', [(toe, 1.0)], (x[0] + 0.01, 0.0, 0.12), (x[1] - 0.01, 0.04, 0.2))
        if extras:   # a bone beside the forearm that turns half of it (Unreal's twist bones do)
            fore = rig.humanoid[side + 'LowerArm']
            twist = rig.node(f'lowerarm_twist_01_{side[0]}', fore, (0.5 * sx, 1.4, 0))
            rig.block(side + 'Band', [(twist, 1.0)], (min(0.48 * sx, 0.52 * sx), 1.36, -0.04), (max(0.48 * sx, 0.52 * sx), 1.44, 0.04))
    rig.block('pelvis', [(hips, 1.0)], (-0.13, 0.9, -0.08), (0.13, 1.1, 0.08))
    rig.block('belly', [(spine, 1.0)], (-0.12, 1.1, -0.08), (0.12, 1.2, 0.08))
    rig.block('neck', [(neck, 1.0)], (-0.04, 1.42, -0.04), (0.04, 1.52, 0.04))
    rig.block('head', [(head, 1.0)], (-0.08, 1.52, -0.09), (0.08, 1.76, 0.09), 4)
    if scheme == 'mixamo':   # the ends Mixamo gives a head and a foot: neither is the bone
        rig.node('mixamorig:HeadTop_End', head, (0, 1.76, 0))
    if extras:
        hair = rig.node('Hair', head, (0, 1.7, -0.08))
        rig.block('hair', [(hair, 1.0)], (-0.09, 1.4, -0.13), (0.09, 1.78, -0.09), 2)
        rig.block('stand', [(0, 1.0)], (-0.3, -0.06, -0.3), (0.3, -0.03, 0.3))
    rig.bones = {role: rig.nodes[node][2] for role, node in rig.humanoid.items()}
    if look:
        skin = png(4, 4, lambda x, y: (200 + x, 150 + y, 120, 255))
        face = png(8, 8, lambda x, y: (250, 220, 200, 255 if 1 < x < 6 else 0))
        strands = png(2, 2, lambda x, y: (60, 40, 20, 128))
        rig.materials = [{'color': (1, 0.9, 0.8, 1), 'image': skin, 'toon': True, 'file': 'skin.png'},
                         {'color': (1, 1, 1, 0.5), 'image': strands, 'alpha': 'BLEND', 'double_sided': True, 'file': 'hair.png'},
                         {'image': face, 'alpha': 'MASK', 'file': 'face.png'},
                         {'color': (0.2, 0.2, 0.25, 1)},
                         {'color': (1, 0.9, 0.8, 1), 'image': skin, 'toon': True, 'file': 'skin.png'}]   # the first again
        rig.pictures = {'skin.png': skin, 'hair.png': strands, 'face.png': face}
        if extras:
            rig.paint('hair', 1)
        rig.paint('head', 2)
        for side in SIDES:
            rig.paint(side + 'Foot', 3), rig.paint(side + 'Toes', 3), rig.paint(side + 'Shin', 4)
        first = rig.blocks['head'][0][0]
        mouth, lids = [first + 6, first + 7], {'left': first + 14, 'right': first + 15}   # the head's front, low and high
        rig.shapes = {'aa': {v: (0.0, -0.012, 0.0) for v in mouth}, 'oh': {v: (0.0, -0.006, 0.004) for v in mouth},
                      'blink': {v: (0.0, -0.008, 0.0) for v in lids.values()},
                      'blinkLeft': {lids['left']: (0.0, -0.008, 0.0)}, 'blinkRight': {lids['right']: (0.0, -0.008, 0.0)},
                      'smile': {v: (0.0, 0.005, 0.0) for v in mouth}}
        rig.expressions = {'aa': 'aa', 'oh': 'oh', 'blink': 'blink', 'blinkLeft': 'blinkLeft', 'blinkRight': 'blinkRight', 'happy': 'smile'}
    rig.built = [v[0] for v in rig.verts]
    rig.moved = {name: dict(moves) for name, moves in rig.shapes.items()}
    return rig


def named_shapes(rig, names):
    """The rig's face shapes under other names (what a program that isn't VRM's calls them)."""
    rig.shapes = {names.get(name, name): moves for name, moves in rig.shapes.items()}
    rig.expressions = {}
    return rig
