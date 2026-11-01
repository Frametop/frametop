"""layout_geometry: where Frametop's screens go, as pure math (no files, sockets or SteamVR).

ft_layout (layout/ft_layout.py) reads the layout and the room and calls these; it re-exports
them under their old names, so its importers and tests reach them there too. Tests:
layout/tests/test_plan.py and layout/tests/test_preview.py.

Head frame: x right, y up, -z forward, at the eye, turned to the heading (yaw).
yaw 0 = -Z, positive yaw turns left, positive pitch looks up (as in SteamVR's helper).
A preview panel is its centre, its axes (x right, y up, z out of its front) and size; a curved
one is a cylinder of radius "curve" round its front, its centre line where the flat one would
be (screens/vr.cpp OnSurface).
"""
import math


def direction(yaw, pitch):
    y, p = math.radians(yaw), math.radians(pitch)
    return (-math.sin(y) * math.cos(p), math.sin(p), -math.cos(y) * math.cos(p))


def yaw_pitch(v):
    n = math.sqrt(sum(c * c for c in v)) or 1.0
    return math.degrees(math.atan2(-v[0], -v[2])), math.degrees(math.asin(max(-1.0, min(1.0, v[1] / n))))


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def normalize(v):
    n = math.sqrt(dot(v, v)) or 1.0
    return tuple(c / n for c in v)


def turn_yaw(v, yaw):
    """Rotate v about +Y by yaw degrees (head frame -> world for the heading yaw)."""
    s, c = math.sin(math.radians(yaw)), math.cos(math.radians(yaw))
    return (v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c)


def chain(widths, d, gap):
    """One row of flat screens hinged edge to edge around the eye, each turned to face it,
    like monitors on a desk: the middle screen (or the seam between the middle two) is
    straight ahead at distance d; each neighbour starts at the previous screen's outer
    edge (plus the gap) and is turned until it faces the eye. Returns [(x, z, yaw)], in
    the head frame from above (x right, -z forward)."""
    n = len(widths)
    out = [None] * n

    def right(yaw):  # a screen's right vector for its yaw
        return math.cos(math.radians(yaw)), -math.sin(math.radians(yaw))

    def yaw_of(x, z):
        return math.degrees(math.atan2(-x, -z))

    def hang(hinge, w, side):
        # Turn the screen about its hinge until it faces the eye: its centre
        # C = hinge + side * w/2 * right(yaw) must have no sideways part, dot(C, right) = 0,
        # that is dot(hinge, right(yaw)) = -side * w/2. Take the root nearest the hinge's
        # own direction, turning outward (right: yaw falls; left: yaw rises).
        def g(yaw):
            rx, rz = right(yaw)
            return hinge[0] * rx + hinge[1] * rz + side * w / 2
        start = yaw_of(*hinge)
        a, ga = start, g(start)
        for k in range(1, 721):  # 0.25-degree steps, up to half a turn
            b = start - side * k * 0.25
            gb = g(b)
            if ga == 0 or ga * gb < 0:
                for _ in range(50):
                    mid = (a + b) / 2
                    if g(a) * g(mid) <= 0:
                        b = mid
                    else:
                        a = mid
                break
            a, ga = b, gb
        yaw = a
        rx, rz = right(yaw)
        return hinge[0] + side * rx * w / 2, hinge[1] + side * rz * w / 2, yaw

    mid = n // 2
    if n % 2:
        out[mid] = (0.0, -d, 0.0)
        right_edge, left_edge, first_right, first_left = (widths[mid] / 2, -d), (-widths[mid] / 2, -d), mid + 1, mid - 1
        yaw_r = yaw_l = 0.0
    else:  # a seam straight ahead
        right_edge, left_edge, first_right, first_left = (gap / 2, -d), (-gap / 2, -d), mid, mid - 1
        yaw_r = yaw_l = 0.0
    for i in range(first_right, n):  # to the right: screen i hangs from the previous right edge
        rx, rz = right(yaw_r)
        g = gap if i != first_right or n % 2 else 0
        hinge = (right_edge[0] + rx * g, right_edge[1] + rz * g)
        cx, cz, yaw_r = hang(hinge, widths[i], +1)
        out[i] = (cx, cz, yaw_r)
        rx, rz = right(yaw_r)
        right_edge = (cx + rx * widths[i] / 2, cz + rz * widths[i] / 2)
    for i in range(first_left, -1, -1):  # to the left, mirrored
        rx, rz = right(yaw_l)
        g = gap if i != first_left or n % 2 else 0
        hinge = (left_edge[0] - rx * g, left_edge[1] - rz * g)
        cx, cz, yaw_l = hang(hinge, widths[i], -1)
        out[i] = (cx, cz, yaw_l)
        rx, rz = right(yaw_l)
        left_edge = (cx - rx * widths[i] / 2, cz - rz * widths[i] / 2)
    return out


def place(layout, sizes, rolls):
    """Screen poses in the head frame for screens of sizes [(w, h)] metres and rolls [deg]:
    [{"pos": (x, y, z), "face": (yaw, pitch), "roll": deg}]. A custom layout with a pose for
    each screen keeps those; otherwise its preset ("kind" flat or curved, rows, distance, gap,
    height) places them."""
    count = len(sizes)
    if layout.get("mode") == "custom" and len(layout.get("screens", [])) >= count and all(
            "pos" in s for s in layout["screens"][:count]):
        return [{"pos": tuple(s["pos"]), "face": tuple(s.get("face", yaw_pitch(s["pos"]))),
                 "roll": float(s.get("roll", rolls[i]))} for i, s in enumerate(layout["screens"][:count])]
    p = layout["preset"]
    rows = max(1, min(int(p.get("rows", 1)), count))
    cols = math.ceil(count / rows)
    # A row setting above what the screens fill leaves empty grid rows (4 screens,
    # 3 rows -> cols 2 -> a third row with nothing in it), and max() over an empty
    # row crashes place(). Trim rows to what the screens actually fill.
    rows = max(1, math.ceil(count / cols))
    d = max(0.3, float(p.get("distance", 2.0)))
    gap = max(0.0, float(p.get("gap", 0.05)))
    height = float(p.get("height", 0.0))
    flat = p.get("kind") == "flat"
    grid = [list(range(r * cols, min(count, (r + 1) * cols))) for r in range(rows)]
    out = [None] * count
    if flat:
        # One flat wall: rows of screens side by side, centred, facing forward.
        row_h = [max(sizes[i][1] for i in row) for row in grid]
        top = height + (sum(row_h) + gap * (rows - 1)) / 2
        for r, row in enumerate(grid):
            y = top - sum(row_h[:r]) - gap * r - row_h[r] / 2
            x = -(sum(sizes[i][0] for i in row) + gap * (len(row) - 1)) / 2
            for i in row:
                out[i] = {"pos": (x + sizes[i][0] / 2, y, -d), "face": (0.0, 0.0), "roll": rolls[i]}
                x += sizes[i][0] + gap
        return out
    # Curved: each row hinged edge to edge around you (see chain); rows stacked by angle
    # (a row of height h at distance d spans 2 atan(h/2d)), each tilted to face you.
    span = lambda m: 2 * math.degrees(math.atan(m / 2 / d))
    row_h = [max(span(sizes[i][1]) for i in row) for row in grid]
    g = span(gap)
    top = math.degrees(math.atan(height / d)) + (sum(row_h) + g * (rows - 1)) / 2
    for r, row in enumerate(grid):
        pitch = top - sum(row_h[:r]) - g * r - row_h[r] / 2
        cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
        for i, (x, z, yaw) in zip(row, chain([sizes[i][0] for i in row], d, gap)):
            # Tilt the row about the eye's left-right axis: forward distance shrinks by cos,
            # height grows by sin.
            r_h = math.hypot(x, z)
            out[i] = {"pos": (x * cp, r_h * sp, z * cp), "face": (yaw, pitch), "roll": rolls[i]}
    return out


def relative_pose(center, x_axis, z_axis, eye, heading):
    """A screen's pose in the world -> custom layout entry (pos, face, roll) in the head frame."""
    rel = turn_yaw(tuple(c - e for c, e in zip(center, eye)), -heading)
    fyaw, fpitch = yaw_pitch(tuple(-c for c in z_axis))
    # Roll: the panel's right vector against an upright panel's right and up.
    right = normalize(cross((0.0, 1.0, 0.0), z_axis))
    up = cross(z_axis, right)
    roll = math.degrees(math.atan2(dot(x_axis, up), dot(x_axis, right)))
    return {"pos": [round(v, 4) for v in rel], "face": [round(fyaw - heading, 2), round(fpitch, 2)],
            "roll": round(roll, 2)}


def panel_axes(face, roll):
    """A panel's axes from its face (yaw, pitch) and roll, as relative_pose reads them."""
    z = tuple(-c for c in direction(*face))
    right = cross((0.0, 1.0, 0.0), z)
    right = normalize(right) if dot(right, right) > 1e-9 else (1.0, 0.0, 0.0)  # straight up or down
    up = cross(z, right)
    r = math.radians(roll)
    x = tuple(math.cos(r) * a + math.sin(r) * b for a, b in zip(right, up))
    return x, cross(z, x), z


def surface_point(p, u, v):
    """The point u metres along a panel from its centre (along the arc when curved), v up."""
    (cx, cy, cz), (x, y, z), r = p["center"], p["axes"], p.get("curve", 0)
    lx, lz = (r * math.sin(u / r), r - r * math.cos(u / r)) if r > 0 else (u, 0.0)
    return tuple(c + a * lx + b * v + d * lz for c, a, b, d in zip((cx, cy, cz), x, y, z))


def to_anchor(v, anchor, point=True):
    """A world point (or, with point=False, a direction) in the anchor's head frame."""
    eye, heading = anchor
    return turn_yaw(tuple(a - b for a, b in zip(v, eye)) if point else v, -heading)


def wrap(deg):
    return (deg + 180.0) % 360.0 - 180.0


def preview_panel(p, eye, facing=0.0, steps=12):
    """p with "top": [x, z] along its middle seen from above, and "view": its outline as
    [yaw, pitch] seen from eye: within half a turn of facing at its centre, and unwrapped
    round that, so one behind you stays whole."""
    w, h = p["width"], p["height"]
    us = [-w / 2 + w * k / steps for k in range(steps + 1)]
    top = [surface_point(p, u, 0.0) for u in us]
    edge = ([(u, -h / 2) for u in us] + [(w / 2, -h / 2 + h * k / 4) for k in range(1, 5)]
            + [(u, h / 2) for u in reversed(us[:-1])] + [(-w / 2, h / 2 - h * k / 4) for k in range(1, 4)])
    mid = yaw_pitch(tuple(a - b for a, b in zip(p["center"], eye)))[0]
    mid = facing + wrap(mid - facing)
    view = []
    for u, v in edge:
        yaw, pitch = yaw_pitch(tuple(a - b for a, b in zip(surface_point(p, u, v), eye)))
        view.append([round(mid + wrap(yaw - mid), 2), round(pitch, 2)])
    out = {k: v for k, v in p.items() if k not in ("center", "axes")}
    out["top"] = [[round(q[0], 4), round(q[2], 4)] for q in top]
    out["view"] = view
    out["distance"] = round(math.dist(p["center"], eye), 3)
    return out
