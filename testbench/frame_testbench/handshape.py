"""Scripted hands for the fake hand tracker: a hand in one of a few shapes, posed in the head frame.

Everything is what ft-hands publishes (hands/include/fh_hands.h): metres in OpenVR's HMD frame
(+X right, +Y up, -Z forward), MediaPipe's 21 landmarks, and joint frames in OpenXR's hand-joint
axes (-Z along the bone toward the fingertip, +Y out of the back of the hand, +X = Y x Z).

A hand is built in its own frame, the wrist at the origin: with no rotation the fingers point
forward and the palm faces down. A left hand is the right one mirrored across X.
"""
import math

from . import pose

SHAPES = ('open', 'point', 'pinch', 'fist')

# Right hand, its own frame. Each finger: knuckle (MCP), then its three bones' lengths.
FINGERS = (
    ((-0.022, 0.0, -0.088), (0.040, 0.024, 0.021)),   # index: landmarks 5-8
    ((-0.002, 0.0, -0.092), (0.044, 0.028, 0.023)),   # middle: 9-12
    ((0.016, 0.0, -0.087), (0.041, 0.027, 0.023)),    # ring: 13-16
    ((0.032, 0.0, -0.078), (0.032, 0.020, 0.020)),    # little: 17-20
)
THUMB_CMC = (-0.020, -0.008, -0.025)
THUMB_OPEN = (-0.050, -0.012, -0.080)   # where the open thumb's tip is

# Flexion (rad, toward the palm) of each finger's MCP, PIP and DIP joints, per shape.
STRAIGHT, RELAXED, CURLED = (0.05, 0.05, 0.02), (0.10, 0.10, 0.05), (1.50, 1.70, 1.00)
FLEXION = {
    'open': (RELAXED,) * 4,
    'point': (STRAIGHT, CURLED, CURLED, CURLED),
    'pinch': ((0.50, 0.60, 0.30), (0.25, 0.30, 0.15), (0.30, 0.35, 0.20), (0.35, 0.40, 0.20)),
    'fist': (CURLED,) * 4,
}

# Rough joint radii (m): ft-hands' come from the fit, these are its typical values.
RADIUS = [0.025, 0.011, 0.010, 0.009, 0.008] + [0.010, 0.009, 0.008, 0.007] * 4

# The capsules ft-hands cuts out (hands/track/io.cpp): the same bones, radii and near-plane clip.
CAPSULE_THUMB = ((0, 1), (1, 2), (2, 3), (3, 4))
CAPSULE_FINGERS = ((5, 6), (6, 7), (7, 8), (9, 10), (10, 11), (11, 12), (13, 14), (14, 15), (15, 16),
                   (17, 18), (18, 19), (19, 20))
CAPSULE_PALM = ((0, 5), (0, 9), (0, 13), (0, 17), (5, 9), (9, 13), (13, 17), (1, 5))
THUMB_R, FINGER_R, PALM_R, ARM_R, ARM_LEN, MARGIN, NEAR = 0.0095, 0.0085, 0.015, (0.028, 0.034), 0.16, 0.004, 0.12


def add(a, b): return [x + y for x, y in zip(a, b)]
def sub(a, b): return [x - y for x, y in zip(a, b)]
def scale(a, s): return [x * s for x in a]
def dot(a, b): return sum(x * y for x, y in zip(a, b))
def norm(a): return math.sqrt(dot(a, a))
def unit(a): return scale(a, 1 / norm(a))
def cross(a, b): return [a[1]*b[2] - a[2]*b[1], a[2]*b[0] - a[0]*b[2], a[0]*b[1] - a[1]*b[0]]


def quat_from_axes(x, y, z):
    """[w, x, y, z] of the rotation whose columns are the axes x, y, z."""
    m = [[x[0], y[0], z[0]], [x[1], y[1], z[1]], [x[2], y[2], z[2]]]
    trace = m[0][0] + m[1][1] + m[2][2]
    if trace > 0:
        s = 2 * math.sqrt(trace + 1)
        q = [s / 4, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s]
    else:
        i = max(range(3), key=lambda k: m[k][k])
        j, k = (i + 1) % 3, (i + 2) % 3
        s = 2 * math.sqrt(1 + m[i][i] - m[j][j] - m[k][k])
        q = [0.0] * 4
        q[0] = (m[k][j] - m[j][k]) / s
        q[1 + i] = s / 4
        q[1 + j] = (m[j][i] + m[i][j]) / s
        q[1 + k] = (m[k][i] + m[i][k]) / s
    return pose.normalize(q)


def local_hand(shape):
    """The right hand's 21 landmarks and joint frames ([w, x, y, z]) in its own frame."""
    pts = [None] * 21
    frames = [None] * 21
    pts[0] = [0.0, 0.0, 0.0]
    flexion = FLEXION[shape]
    for f, (knuckle, lengths) in enumerate(FINGERS):
        base = 5 + 4 * f
        pts[base] = list(knuckle)
        phi = 0.0
        for b, length in enumerate(lengths):
            phi += flexion[f][b]
            # Curling turns the bone about the hand's X axis, toward the palm (-Y).
            zaxis = [0.0, math.sin(phi), math.cos(phi)]
            yaxis = [0.0, math.cos(phi), -math.sin(phi)]
            pts[base + b + 1] = add(pts[base + b], scale(zaxis, -length))
            frames[base + b] = quat_from_axes(cross(yaxis, zaxis), yaxis, zaxis)
        frames[base + 3] = frames[base + 2]
    # The thumb: from its base to where its tip is in this shape, bending out to the thumb's side.
    cmc = list(THUMB_CMC)
    if shape == 'pinch':
        tip = add(pts[8], [-0.006, 0.0, 0.0])
    elif shape == 'open':
        tip = list(THUMB_OPEN)
    else:   # tucked over the middle finger's middle bone
        tip = add(pts[10], [-0.012, -0.018, 0.0])
    bow = scale(unit([-1.0, -0.3, 0.0]), 0.012)
    pts[1] = cmc
    pts[2] = add(add(cmc, scale(sub(tip, cmc), 0.42)), bow)
    pts[3] = add(add(cmc, scale(sub(tip, cmc), 0.74)), scale(bow, 0.6))
    pts[4] = tip
    for j in range(1, 4):
        zaxis = unit(sub(pts[j], pts[j + 1]))
        yaxis = sub([0.0, 1.0, 0.0], scale(zaxis, zaxis[1]))
        yaxis = unit(yaxis) if norm(yaxis) > 1e-6 else [0.0, 0.0, 1.0]
        frames[j] = quat_from_axes(cross(yaxis, zaxis), yaxis, zaxis)
    frames[4] = frames[3]
    frames[0] = [1.0, 0.0, 0.0, 0.0]   # the palm's frame is the hand's own
    angles = [0.0] * 26
    for f in range(4):
        angles[6 + 5 * f + 1], angles[6 + 5 * f + 3], angles[6 + 5 * f + 4] = flexion[f]
    return pts, frames, angles


def parent(j):
    return 0 if j in (1, 5, 9, 13, 17) else j - 1


def curl(pts):
    """The grip's reading (hands/track/pinch.h GripParams): each finger's tip-to-wrist over its
    knuckle-to-wrist distance, averaged: about 2 open, under 1.2 closed."""
    return sum(norm(sub(pts[t], pts[0])) / norm(sub(pts[k], pts[0])) for k, t in ((5, 8), (9, 12), (13, 16), (17, 20))) / 4


def hand(side, position, quaternion, shape):
    """A posed hand: landmarks, joint frames ([x, y, z, w], fh_hands.h's order), and what the
    hands and gestures files derive from them."""
    if shape not in SHAPES:
        raise ValueError(f'unknown hand shape {shape!r}')
    pts, frames, angles = local_hand(shape)
    mirror = side == 'left'
    position = pose.vector(position, 3)
    quaternion = pose.normalize(quaternion)
    out_pts, out_q = [], []
    for p, q in zip(pts, frames):
        if mirror:
            p = [-p[0], p[1], p[2]]
            # Mirroring across X keeps the axes' Y and Z and the rule X = Y x Z, so the frame's
            # rotation is mirrored: (w, x, y, z) -> (w, x, -y, -z).
            q = [q[0], q[1], -q[2], -q[3]]
        out_pts.append(add(position, pose.rotate(quaternion, p)))
        w, x, y, z = pose.multiply(quaternion, q)
        out_q.append([x, y, z, w])
    p = out_pts
    bones = [norm(sub(p[j], p[parent(j)])) for j in range(1, 21)]
    elbow = add(position, pose.rotate(quaternion, [0.0, -0.08, 0.24]))
    return {
        'side': side, 'shape': shape, 'pts': p, 'quats': out_q, 'radius': RADIUS, 'bones': bones,
        'angles': angles, 'elbow': elbow,
        'pinch_point': scale(add(p[5], p[9]), 0.5), 'contact': scale(add(p[4], p[8]), 0.5),
        'pinch_d': [norm(sub(p[4], p[8])), norm(sub(p[4], p[12]))],
        'palm': scale([sum(p[j][k] for j in (0, 5, 9, 13, 17)) for k in range(3)], 1 / 5),
        'curl': curl(p),
    }


def capsules(pts):
    """The hand's capsules, as ft-hands' Publisher::write makes them: (a, b, ra, rb)."""
    out = []

    def put(a, b, ra, rb):
        za, zb = -a[2] - NEAR, -b[2] - NEAR
        if za < 0 and zb < 0:
            return
        if za < 0 or zb < 0:
            t = za / (za - zb)
            m = add(a, scale(sub(b, a), t))
            rm = ra + (rb - ra) * t
            if za < 0:
                a, ra = m, rm
            else:
                b, rb = m, rm
        out.append((a, b, ra + MARGIN, rb + MARGIN))

    for i, j in CAPSULE_THUMB:
        put(pts[i], pts[j], THUMB_R, THUMB_R)
    for i, j in CAPSULE_FINGERS:
        put(pts[i], pts[j], FINGER_R, FINGER_R)
    for i, j in CAPSULE_PALM:
        put(pts[i], pts[j], PALM_R, PALM_R)
    wrist = pts[0]
    d = sub(wrist, pts[9])
    if norm(d) > 0.02:
        put(wrist, add(wrist, scale(d, ARM_LEN / norm(d))), ARM_R[0], ARM_R[1])
    return out
