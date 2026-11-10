"""Frametop's model file (avatar/modelfile.h, avatar/bodyfile.h), read in Python: what Settings
shows of a model (its name, where it came from, whether it may be passed on, whether it is a
whole avatar) and what the tests check. The programs that draw a model read it with
modelfile.cpp and bodyfile.cpp, which are the strict ones; this refuses what it can't follow
and checks no more."""
import struct
from dataclasses import dataclass, field

JOINTS = 25
POS_STEP = 1e-05
MAX_BYTES = 64 << 20
# bodyfile.h's roles, in the file's order
ROLES = ('hips spine chest upperChest neck head leftEye rightEye jaw '
         'leftShoulder leftUpperArm leftLowerArm leftHand rightShoulder rightUpperArm rightLowerArm rightHand '
         'leftUpperLeg leftLowerLeg leftFoot leftToes rightUpperLeg rightLowerLeg rightFoot rightToes').split() + [
    side + finger + part for side in ('left', 'right')
    for finger, parts in (('Thumb', ('Metacarpal', 'Proximal', 'Distal')), *((f, ('Proximal', 'Intermediate', 'Distal'))
                                                                               for f in ('Index', 'Middle', 'Ring', 'Little')))
    for part in parts]
SHAPES = ('', 'aa', 'ih', 'ou', 'ee', 'oh', 'blink', 'blinkLeft', 'blinkRight')


class BadModel(ValueError):
    pass


@dataclass
class Hand:
    bind: list           # 25 (x, y, z), metres, the wrist at 0
    pos: list            # (x, y, z) per vertex, metres
    joints: list         # 4 joints per vertex
    weights: list        # 4 weights per vertex, out of 255
    tris: list           # (a, b, c) vertex numbers


@dataclass
class Body:
    eyes: tuple              # where the wearer looks from, metres
    bones: dict              # role -> (x, y, z), metres: the ones the body has
    pos: list                # (x, y, z) per vertex, metres
    nrm: list                # (x, y, z) per vertex, out of 127
    uv: list                 # (u, v) per vertex
    joints: list             # 4 roles (numbers into ROLES) per vertex
    weights: list            # 4 weights per vertex, out of 255
    tris: list               # (a, b, c) vertex numbers
    parts: list              # (first triangle, triangles, material)
    materials: list          # dicts: color, texture, alpha, cutoff, flags
    textures: list = field(default_factory=list)   # (width, height, RGBA bytes)
    shapes: dict = field(default_factory=dict)     # name -> {vertex: (dx, dy, dz) metres}


@dataclass
class Model:
    name: str = ''
    licence: str = ''
    author: str = ''
    format: str = ''
    may_share: bool = False
    hands: dict = field(default_factory=dict)   # 'left', 'right'
    has_body: bool = False                      # an avatar: the file has a body
    body: Body = None                           # ...read when asked for


def _info(body, model):
    at = 0
    while at + 3 <= len(body):
        key, size = struct.unpack_from('<BH', body, at)
        value = body[at + 3:at + 3 + size]
        at += 3 + size
        if len(value) != size:
            raise BadModel('INFO runs past its end')
        if key == 5:
            model.may_share = value == b'\x01'
        elif key in (1, 2, 3, 4):
            setattr(model, ('name', 'licence', 'author', 'format')[key - 1], value.decode('utf-8', 'replace'))


def _hand(body, model):
    if len(body) < 12 + JOINTS * 12:
        raise BadModel('HAND is too short')
    side, verts, tris = struct.unpack_from('<B3xII', body, 0)
    at = 12
    bind = [struct.unpack_from('<3f', body, at + 12 * j) for j in range(JOINTS)]
    at += JOINTS * 12

    def take(fmt, count, width):
        nonlocal at
        size = struct.calcsize(fmt) * count * width
        if at + size > len(body):
            raise BadModel('HAND runs past its end')
        flat = struct.unpack_from(f'<{count * width}{fmt}', body, at)
        at += (size + 3) & ~3
        return [flat[i:i + width] for i in range(0, len(flat), width)]

    pos = [tuple(c * POS_STEP for c in p) for p in take('h', verts, 3)]
    take('b', verts, 3)   # the normals
    joints, weights = take('B', verts, 4), take('B', verts, 4)
    model.hands['right' if side else 'left'] = Hand(bind, pos, joints, weights, take('H', tris, 3))


class _Taker:
    """Arrays out of a chunk, each padded to a multiple of four."""
    def __init__(self, body, at, what):
        self.body, self.at, self.what = body, at, what

    def __call__(self, fmt, count, width=1):
        size = struct.calcsize(fmt) * count * width
        if self.at + size > len(self.body):
            raise BadModel(f'{self.what} runs past its end')
        flat = struct.unpack_from(f'<{count * width}{fmt}', self.body, self.at)
        self.at += (size + 3) & ~3
        return list(flat) if width == 1 else [flat[i:i + width] for i in range(0, len(flat), width)]


def _body(body):
    if len(body) < 40:
        raise BadModel('BODY is too short')
    zero, unit, ex, ey, ez, roles, verts, tris, parts, materials = struct.unpack_from('<If3f5I', body, 0)
    if zero != 0 or roles != len(ROLES):
        raise BadModel('a body of another kind')
    take = _Taker(body, 40, 'BODY')
    at, present = take('f', roles, 3), take('B', roles)
    pos = [tuple(c * unit for c in p) for p in take('h', verts, 3)]
    nrm, uv, joints, weights = take('b', verts, 3), take('f', verts, 2), take('B', verts, 4), take('B', verts, 4)
    triangles, part = take('H', tris, 3), take('I', parts, 3)
    mats = []
    for _ in range(materials):
        if take.at + 32 > len(body):
            raise BadModel('BODY runs past its end')
        r, g, b, a, texture, alpha, cutoff, flags = struct.unpack_from('<4fiIfI', body, take.at)
        take.at += 32
        mats.append({'color': (r, g, b, a), 'texture': texture, 'alpha': alpha, 'cutoff': cutoff, 'flags': flags})
    bones = {ROLES[r]: at[r] for r in range(roles) if present[r]}
    return Body((ex, ey, ez), bones, pos, nrm, uv, joints, weights, triangles, part, mats), unit


def _texture(body):
    if len(body) < 8:
        raise BadModel('TEXR is too short')
    width, height, fmt = struct.unpack_from('<HHI', body, 0)
    if fmt != 0 or len(body) < 8 + width * height * 4:
        raise BadModel('a texture of another kind')
    return width, height, body[8:8 + width * height * 4]


def _shape(body, unit):
    if len(body) < 8:
        raise BadModel('SHAP is too short')
    which, count = struct.unpack_from('<II', body, 0)
    take = _Taker(body, 8, 'SHAP')
    verts, deltas = take('H', count), take('h', count, 3)
    name = SHAPES[which] if 0 < which < len(SHAPES) else str(which)
    return name, {v: tuple(c * unit for c in d) for v, d in zip(verts, deltas)}


def _chunks(path):
    with open(path, 'rb') as f:
        data = f.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES or data[:4] != b'FTMD' or len(data) < 8 or struct.unpack_from('<I', data, 4)[0] != 1:
        raise BadModel('not a model file')
    at = 8
    while at + 8 <= len(data):
        tag, size = data[at:at + 4], struct.unpack_from('<I', data, at + 4)[0]
        body = data[at + 8:at + 8 + size]
        if len(body) != size:
            raise BadModel('a chunk runs past the end')
        at += 8 + ((size + 3) & ~3)
        yield tag, body


def read(path, hands=True, body=False):
    """The model in the file. Raises BadModel (or OSError). hands=False: its INFO alone (the
    hands' numbers aren't unpacked), for a list of models. body=True: its body too, if it has one
    (has_body says so either way)."""
    model = Model()
    unit, later = 0.0, []
    for tag, data in _chunks(path):
        if tag == b'INFO':
            _info(data, model)
        elif tag == b'HAND' and hands:
            _hand(data, model)
        elif tag == b'BODY':
            model.has_body = True
            if body:
                model.body, unit = _body(data)
        elif tag in (b'TEXR', b'SHAP') and body:
            later.append((tag, data))
    for tag, data in later:
        if model.body is None:
            raise BadModel('textures or shapes without a body')
        if tag == b'TEXR':
            model.body.textures.append(_texture(data))
        else:
            name, moves = _shape(data, unit)
            model.body.shapes[name] = moves
    return model
