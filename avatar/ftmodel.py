"""Frametop's model file (avatar/modelfile.h), read in Python: what Settings shows of a model
(its name, where it came from, whether it may be passed on) and what the tests check. The
programs that draw a model read it with modelfile.cpp, which is the strict one; this refuses
what it can't follow and checks no more."""
import struct
from dataclasses import dataclass, field

JOINTS = 25
POS_STEP = 1e-05
MAX_BYTES = 64 << 20


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
class Model:
    name: str = ''
    licence: str = ''
    author: str = ''
    format: str = ''
    may_share: bool = False
    hands: dict = field(default_factory=dict)   # 'left', 'right'


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


def read(path, hands=True):
    """The model in the file. Raises BadModel (or OSError). hands=False: its INFO alone (the
    hands' numbers aren't unpacked), for a list of models."""
    model = Model()
    for tag, body in _chunks(path):
        if tag == b'INFO':
            _info(body, model)
        elif tag == b'HAND' and hands:
            _hand(body, model)
    return model
