"""Where a panel is, said two ways: on this Frame's own floor, as ft-screens says it ("get" and
"pose": SteamVR's standing universe), and in the room, as ft-together does ("share" and the
room's state: the owner's directions). Between them lies the floor (together/local/floor.h): a
place on your floor, turned by `yaw` about the way up and then moved by (x, z), is that place in
the room. The sums only; avatar/room/floor.h has the same ones for people.
"""
import math


def turned(yaw, v):
    """`v` turned by `yaw` radians about the way up."""
    c, s = math.cos(yaw), math.sin(yaw)
    return (v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c)


def wrap(angle):
    return (angle + math.pi) % (2 * math.pi) - math.pi


def lay(spot, head):
    """The floor of someone whose spot in the room is `spot` (x, y, z, yaw) and whose head is at
    (x, z) of their own floor facing `yaw` (radians, 0 faces -z): where they stand is the spot,
    facing the way the spot faces."""
    yaw = wrap(spot[3] - head[2])
    at = turned(yaw, (head[0], 0.0, head[1]))
    return (spot[0] - at[0], spot[2] - at[2], yaw)


def quat(x, y, z):
    """The turn (w, x, y, z) whose axes are the unit vectors x, y, z."""
    trace = x[0] + y[1] + z[2]
    if trace > 0:
        s = math.sqrt(trace + 1) * 2
        q = (s / 4, (y[2] - z[1]) / s, (z[0] - x[2]) / s, (x[1] - y[0]) / s)
    elif x[0] > y[1] and x[0] > z[2]:
        s = math.sqrt(1 + x[0] - y[1] - z[2]) * 2
        q = ((y[2] - z[1]) / s, s / 4, (y[0] + x[1]) / s, (z[0] + x[2]) / s)
    elif y[1] > z[2]:
        s = math.sqrt(1 + y[1] - x[0] - z[2]) * 2
        q = ((z[0] - x[2]) / s, (y[0] + x[1]) / s, s / 4, (z[1] + y[2]) / s)
    else:
        s = math.sqrt(1 + z[2] - x[0] - y[1]) * 2
        q = ((x[1] - y[0]) / s, (z[0] + x[2]) / s, (z[1] + y[2]) / s, s / 4)
    n = math.sqrt(sum(v * v for v in q)) or 1.0
    return tuple(v / n if q[0] >= 0 else -v / n for v in q)


def axes(q):
    """The axes x, y, z of the turn q = (w, x, y, z); a turn of no length is no turn."""
    n = math.sqrt(sum(v * v for v in q))
    w, x, y, z = (v / n for v in q) if n > 1e-9 else (1.0, 0.0, 0.0, 0.0)
    return ((1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)),
            (2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)),
            (2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)))


def parse_get(reply):
    """ft-screens' answer to "get N": {"at", "x", "y", "z", "metres", "curve"}, or None."""
    words = reply.split()
    if len(words) < 16 or words[0] != "ok":
        return None
    try:
        g = [float(w) for w in words[1:16]]
    except ValueError:
        return None
    if not all(math.isfinite(v) for v in g):
        return None
    return {"at": tuple(g[0:3]), "x": tuple(g[3:6]), "y": tuple(g[6:9]), "z": tuple(g[9:12]), "metres": g[12], "curve": g[14]}


def to_room(floor, got):
    """A panel of this Frame's (parse_get) as the room has it: {"at", "turn", "metres", "curve"}."""
    x, z, yaw = floor
    at = turned(yaw, got["at"])
    return {"at": (at[0] + x, at[1], at[2] + z), "turn": quat(turned(yaw, got["x"]), turned(yaw, got["y"]), turned(yaw, got["z"])),
            "metres": got["metres"], "curve": got["curve"]}


def to_floor(floor, panel):
    """A panel of the room's (its "at" and "turn") on this Frame's floor: the twelve numbers of
    ft-screens' "pose", the rows of a 3x4."""
    x, z, yaw = floor
    at = turned(-yaw, (panel["at"][0] - x, panel["at"][1], panel["at"][2] - z))
    ax = [turned(-yaw, a) for a in axes(panel["turn"])]
    return [ax[0][0], ax[1][0], ax[2][0], at[0], ax[0][1], ax[1][1], ax[2][1], at[1], ax[0][2], ax[1][2], ax[2][2], at[2]]
