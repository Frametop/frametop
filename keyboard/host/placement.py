"""Where Frametop opens Full Keyboard, and where a fingertip touches it. Pure math, no OpenVR.

Poses are OpenVR 3x4 row-major matrices in the standing universe (lists of 3 rows of 4).

- spot(): ft-screens says where to open it, "head M" (the headset's pose) or "dash M" (the
  bottom centre of Steam's page, or of Frametop's tile of it). Like Frametop's keyboard before
  (keyboard/FRAMETOP.md): 0.70 m ahead and 0.45 m under the eyes, tilted 26.6 degrees to face
  them, so a hand's laser reaches the bottom row; or 14 cm under Steam's page, tilted 50 (user,
  2026-11-21: "the keyboard should open lower"; it was 0.35 m and 6 cm).
- touch(): a fingertip's point (ft-screens' "kbpoke") on the panel as Full Keyboard last
  reported it ("geometry"), as the texture coordinates its forwarded pointer takes.
"""
import math

AHEAD, BELOW, TILT = 0.70, 0.45, 26.6
DASH_TILT, DASH_FORWARD, DASH_GAP = 50.0, 0.24, 0.14


def matrix(words):
    """Twelve numbers as a 3x4 pose, or None."""
    if len(words) != 12:
        return None
    try:
        values = [float(w) for w in words]
    except ValueError:
        return None
    if not all(math.isfinite(v) and abs(v) < 1000 for v in values):
        return None
    return [values[0:4], values[4:8], values[8:12]]


def level_heading(pose):
    """A rotation turned like pose about the vertical axis only (its back axis flattened), so
    looking down or tilting the head doesn't open a slanted panel. Straight up or down: from its
    right axis."""
    zx, zz = pose[0][2], pose[2][2]
    if math.hypot(zx, zz) < .001:
        zx, zz = -pose[2][0], pose[0][0]
    n = math.hypot(zx, zz)
    if n < .001:
        return [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]
    zx, zz = zx / n, zz / n
    return [[zz, 0.0, zx], [0.0, 1.0, 0.0], [-zx, 0.0, zz]]


def spot(kind, pose, width, height):
    """The panel's pose for an opening at "head" or "dash" pose, for a panel width x height m
    (Full Keyboard 0.8.1's PanelPlacement::recenter, with Frametop's head spot)."""
    rot = level_heading(pose)
    # The top edge tilts away from the viewer.
    pitch = -math.radians(DASH_TILT if kind == "dash" else TILT)
    c, s = math.cos(pitch), math.sin(pitch)
    world = [[0.0] * 4 for _ in range(3)]
    for r in range(3):
        world[r][0] = rot[r][0]
        world[r][1] = rot[r][1] * c + rot[r][2] * s
        world[r][2] = -rot[r][1] * s + rot[r][2] * c
        if kind == "dash":  # DASH_GAP under the page's bottom edge, DASH_FORWARD toward the viewer
            world[r][3] = pose[r][3] + rot[r][2] * DASH_FORWARD - rot[r][1] * (DASH_GAP + height * c / 2)
        else:
            world[r][3] = pose[r][3] - rot[r][2] * AHEAD - rot[r][1] * BELOW
    return world


def touch(point, panel, width, height):
    """point (x, y, z) on panel (its pose; width x height m) as (u, v): 0..1 across and down the
    texture, unclamped (off the panel is outside 0..1)."""
    d = [point[r] - panel[r][3] for r in range(3)]
    local_x = sum(panel[r][0] * d[r] for r in range(3))
    local_y = sum(panel[r][1] * d[r] for r in range(3))
    return 0.5 + local_x / width, 0.5 - local_y / height
