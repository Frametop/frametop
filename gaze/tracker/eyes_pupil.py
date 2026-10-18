"""Classic pupil and glint finder for one 512x400 eye-camera frame.

The pupil is a dark blob enclosed by brighter iris and skin. The lens rim and the unlit
background are just as dark, but they touch the image edge, so any dark region that reaches
the edge is dropped. Closing the glints' holes can join the pupil to that background (the
left camera's, when you look more than about 20 degrees left), so when nothing is found
the search runs again with a smaller closing.

A dark spot well inside the frame can pass all that too: on 2026-10-17 the right camera took
one about 9 px across for the pupil (23 px) about once a second, for 3-7 frames at a time, and
the gaze jumped 60 degrees. A pupil doesn't shrink to half its size in a frame, so a find that
much smaller than the last half second's (PupilSize) is skipped.
"""
from collections import deque

import cv2
import numpy as np

# One thread: OpenCV's pool of one per core costs more than it saves on a frame this small
# (a 140-240 px window while it follows the pupil). Its idle workers spun and yielded about
# 14,000 times a second each, a quarter of a core, beside SteamVR's compositor.
cv2.setNumThreads(1)

DARK = 30         # pupil pixels are below this (the face around it is 40-180)
MIN_AREA = 150     # pupil area range in pixels
MAX_AREA = 20000
MIN_FILL = 0.75    # blob area / fitted-ellipse area
MAX_ASPECT = 3.0   # long / short axis; the steep camera sees a squashed pupil
GLINT = 200        # glints are near-saturated spots on or by the pupil
CLOSE = 7          # px: closes the glints' holes in the pupil (the right eye's need this much)
CLOSE_TIGHT = 3    # px: the retry, keeps a pupil near the dark background apart from it
_KERNELS = {size: np.ones((size, size), np.uint8) for size in {3, CLOSE, CLOSE_TIGHT}}


NEAR = 70          # the windowed search: this many pixels, or 3 pupil radii, around a hint


def find_pupil(frame, near=None, smallest=0.0):
    """Return dict(x, y, a, b, angle, area, fill, glints) or None.

    `near` (a previous result) searches a window around it first (0.4 ms, not 1.4-2.1);
    if the pupil isn't wholly inside the window it falls back to the whole frame. Blobs whose
    long semi-axis is under `smallest` px don't count (PupilSize.smallest)."""
    if near is not None:
        r = int(max(NEAR, 3 * near['a']))
        x0, y0 = max(int(near['x']) - r, 0), max(int(near['y']) - r, 0)
        p = _find_either(frame[y0:int(near['y']) + r, x0:int(near['x']) + r], x0, y0, smallest)
        if p is not None:
            p['glints'] = find_glints(frame, p)
            return p
    p = _find_either(frame, 0, 0, smallest)
    if p is not None:
        p['glints'] = find_glints(frame, p)
    return p


def _find_either(img, ox, oy, smallest=0.0):
    # Both closing sizes use the same input mask; morphologyEx returns a new one.
    dark = (cv2.GaussianBlur(img, (5, 5), 0) < DARK).astype(np.uint8)
    p = _find(img, dark, ox, oy, smallest=smallest)
    return p if p is not None else _find(img, dark, ox, oy, CLOSE_TIGHT, smallest)


def _find(img, dark, ox, oy, close=CLOSE, smallest=0.0):
    """The pupil in `img` (a window at ox, oy), using its prepared `dark` mask. A dark
    region touching the window's edge doesn't count: it's background, rim, or cut off."""
    # Glints punch bright holes in the pupil; close them so the blob stays whole.
    dark = cv2.morphologyEx(dark, cv2.MORPH_CLOSE, _KERNELS[close])
    dark = cv2.morphologyEx(dark, cv2.MORPH_OPEN, _KERNELS[3])
    n, lab, stats, _ = cv2.connectedComponentsWithStats(dark, connectivity=8)
    h, w = img.shape
    best = None
    for i in range(1, n):
        x, y, bw, bh, area = stats[i]
        if area < MIN_AREA or area > MAX_AREA:
            continue
        if x <= 1 or y <= 1 or x + bw >= w - 1 or y + bh >= h - 1:
            continue
        blob = lab[y:y + bh, x:x + bw] == i
        cs, _ = cv2.findContours(blob.astype(np.uint8), cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
        c = max(cs, key=len)
        if len(c) < 5:
            continue
        (ex, ey), (d1, d2), ang = cv2.fitEllipse(c)
        a, b = max(d1, d2) / 2, min(d1, d2) / 2
        if b < 3 or a / b > MAX_ASPECT or a < smallest:
            continue
        fill = area / (np.pi * a * b)
        if fill < MIN_FILL or fill > 1.25:
            continue
        # Prefer the darkest, fullest blob.
        score = fill - img[y:y + bh, x:x + bw][blob].mean() / 255
        if best is None or score > best[0]:
            # fitEllipse's angle is the direction of its first axis (d1); the long axis is
            # that one or the one at right angles.
            major = np.radians(ang if d1 >= d2 else ang + 90)
            best = (score, dict(x=ex + x + ox, y=ey + y + oy, a=a, b=b, angle=ang, major=major,
                                area=int(area), fill=fill, box=(x + ox, y + oy, bw, bh)))
    return best[1] if best else None


SMALLEST = 0.5     # a find under this share of the pupil's recent size is something else
SIZE_WINDOW = 0.5  # s of finds the recent size comes from
SIZE_FINDS = 10    # and at least this many of them


class PupilSize:
    """One eye's recent pupil size, the median long semi-axis of its finds over the last
    SIZE_WINDOW s. A pupil narrows in bright light over a second or so, and turning the eye
    changes how big the camera sees it by a few percent per frame; in the practice captures
    (2026-09-29) under 0.4 % of finds were below half this median, many of them strays as well.
    Only smaller finds are refused: if a wrong spot gets in (after a loss, with no recent size),
    the real pupil isn't refused when it's back, and the median follows it."""

    def __init__(self):
        self.finds = deque()   # (t, a)

    def smallest(self, t):
        """The smallest long semi-axis a find at time t may have, in px (0: no limit yet)."""
        while self.finds and self.finds[0][0] < t - SIZE_WINDOW:
            self.finds.popleft()
        if len(self.finds) < SIZE_FINDS:
            return 0.0
        return SMALLEST * float(np.median([a for _, a in self.finds]))

    def add(self, t, p):
        if p is not None:
            self.finds.append((t, p['a']))


GLINT_RING = 140   # a glint sits on dark iris or pupil: its surroundings are below this
GLINT_PAIR = (5, 45)  # the two LEDs' reflections: this far apart, in pixels, mostly vertical


def find_glints(frame, p):
    """Small bright spots on dark iris or pupil within 2.5 pupil radii, as (x, y) list.
    Bright skin has noise speckle above GLINT too, so a spot counts only if the ring
    around it is dark."""
    r = int(p['a'] * 2.5) + 6
    x0, y0 = max(int(p['x']) - r, 0), max(int(p['y']) - r, 0)
    roi = frame[y0:int(p['y']) + r, x0:int(p['x']) + r]
    n, lab, stats, cents = cv2.connectedComponentsWithStats((roi >= GLINT).astype(np.uint8))
    out = []
    for i in range(1, n):
        x, y, w, h, area = stats[i]
        if not 2 <= area <= 80:
            continue
        ya, yb, xa, xb = max(y - 4, 0), y + h + 4, max(x - 4, 0), x + w + 4
        ring = roi[ya:yb, xa:xb][lab[ya:yb, xa:xb] != i]
        if ring.size and np.median(ring) < GLINT_RING:
            out.append((cents[i][0] + x0, cents[i][1] + y0))
    return out


def glint_pair(p):
    """The two LED reflections as ((x, y) upper, (x, y) lower), or None. Picks the
    vertical-ish pair nearest the pupil centre."""
    g = p['glints']
    best = None
    for i in range(len(g)):
        for j in range(i + 1, len(g)):
            dx, dy = g[j][0] - g[i][0], g[j][1] - g[i][1]
            d = np.hypot(dx, dy)
            if not GLINT_PAIR[0] <= d <= GLINT_PAIR[1] or abs(dy) < 2 * abs(dx):
                continue
            mx, my = (g[i][0] + g[j][0]) / 2, (g[i][1] + g[j][1]) / 2
            cost = np.hypot(mx - p['x'], my - p['y'])
            if best is None or cost < best[0]:
                best = (cost, (g[i], g[j]) if dy > 0 else (g[j], g[i]))
    return best[1] if best else None


def draw(frame, p, scale=1.0):
    img = cv2.cvtColor(cv2.convertScaleAbs(frame, alpha=2.0), cv2.COLOR_GRAY2BGR)
    if p:
        cv2.ellipse(img, ((p['x'], p['y']), (2 * p['a'], 2 * p['b']), np.degrees(p['major'])),
                    (0, 255, 0), 1)
        for gx, gy in p['glints']:
            cv2.circle(img, (int(gx), int(gy)), 3, (0, 0, 255), 1)
    if scale != 1.0:
        img = cv2.resize(img, None, fx=scale, fy=scale)
    return img
