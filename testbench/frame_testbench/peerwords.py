"""The words a fake peer's questions are made of (peeracts.py, peershare.py): numbers within
bounds, a turn about the upright, and an answer of `ok` and JSON."""
import json
import math

PANELS = 32   # ids a room's panels may have (together/proto: kPeople * kPanelNumbers)
PEOPLE = 4    # in a room, its owner among them (proto/messages.h)
FAR = 100.0   # metres from the room's middle, as far as a panel goes (proto/panel.h)


def number(word, least, most):
    value = float(word)
    if not least <= value <= most:   # which a nan isn't
        raise ValueError
    return value


def whole(word, least, most):
    value = int(word)
    if not least <= value <= most:
        raise ValueError
    return value


def upright(degrees):
    """A turn of that much to the left about the upright, as a unit quaternion (w, x, y, z)."""
    half = math.radians(degrees) / 2
    return (math.cos(half), 0.0, math.sin(half), 0.0)


def told(reply):
    """What an answer of `ok` and JSON says, or the answer when it is none."""
    return json.loads(reply[3:]) if reply.startswith('ok {') else {'error': reply}
