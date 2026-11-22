"""The move handle under Full Keyboard's panel, for Frametop's pointers. Pure math, no OpenVR.

Full Keyboard draws a move line under its case, but only its own lasers can drag it: a forwarded
pointer can't (its hosted protocol leaves moving to the shell, with set_pose). The 3D mouse and
hand rays reach the panel only as forwarded pointers, so the panel couldn't be moved (user,
2026-11-21: "I also can't drag it, it needs to have a drag toggle underneath it"). So a poke on
the strip under the case, in the middle where the line is, drags the panel here instead: in its
own plane, by how far the point has moved since the press.

The plane stays the same while the panel slides in it, so the pointer helper's ray, which meets
the panel itself while pressed, keeps meeting it where the drag put it. The other hand's pinch held
meanwhile turns it (Drag.turn); the slide then goes on in the turned plane.
"""

ZONE_U = (0.25, 0.75)   # the strip's middle, around the line


def in_zone(u, v, case_bottom):
    """Whether texture point (u, v) is on the handle: under the case (whose bottom edge is
    case_bottom, in v) and within the panel."""
    return case_bottom is not None and ZONE_U[0] <= u <= ZONE_U[1] and case_bottom < v <= 1.0


class Drag:
    def __init__(self, now, point, matrix):
        self.start = list(point)
        self.matrix = [list(row) for row in matrix]
        self.pose = self.matrix   # where it last put the panel
        self.last = now   # when it was last heard of
        self.turn_from = None   # the pose when a two-hand turn began, while one is on

    def to(self, now, point):
        """The panel's pose with point where the press was, slid in the panel's plane; None while a
        two-hand turn places it."""
        self.last = now
        if self.turn_from is not None:
            return None
        if self.start is None:   # the first since a turn: the slide goes on from here
            self.start = list(point)
        d = [point[r] - self.start[r] for r in range(3)]
        m = [list(row) for row in self.matrix]
        for axis in (0, 1):   # right, up: the plane's own directions
            along = sum(self.matrix[r][axis] * d[r] for r in range(3))
            for r in range(3):
                m[r][3] += self.matrix[r][axis] * along
        self.pose = m
        return m

    def turn(self, now, words):
        """The pointer helper's two-hand turn (pointer/helper/twohand.h): "begin", "move" and twelve
        numbers (rows of the turn R since the begin, the move t in the last column), "end". The panel
        turns about its own centre by R and moves by t. The pose to set, or None. After it the slide
        goes on in the turned panel's plane, from the next point."""
        self.last = now
        if words == ["begin"]:
            self.turn_from = self.pose
        elif words == ["end"]:
            if self.turn_from is not None:
                self.matrix, self.start, self.turn_from = self.pose, None, None
        elif self.turn_from is not None and len(words) == 13 and words[0] == "move":
            try:
                mv = [[float(w) for w in words[1 + 4 * r:5 + 4 * r]] for r in range(3)]
            except ValueError:
                return None
            f = self.turn_from
            self.pose = [[sum(mv[r][k] * f[k][c] for k in range(3)) for c in range(3)] + [f[r][3] + mv[r][3]]
                         for r in range(3)]
            return self.pose
        return None
