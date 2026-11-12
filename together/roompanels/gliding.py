"""A flight through the room (docs/together.md, "Control and moving"): someone who pulls a panel
they may not move is taken to where it reads well instead, and not in one jump, so that they
see where they are going. The way from where they stand to that spot is gone in small steps
over GLIDE seconds, slow at both ends, each step a move of the mover's (moving.py): their head
is put over the next spot of the way wherever it is by then, so a head that moves meanwhile
still ends up over the last one.

The rule only. ft-roompanels' loop says `step` every STEP seconds while one is `going`.
"""
import math

from . import placing

GLIDE = 0.4          # seconds a flight takes
STEP = 1 / 60        # seconds between its steps
THERE = 0.02         # metres, and radians: this near the spot nobody is flown to it


class Glide:
    def __init__(self, mover, now):
        self.mover, self.now = mover, now
        self.path = None     # (the spot it began at, the spot it ends at, when it began)

    @property
    def going(self):
        return self.path is not None

    def to_panel(self, ident):
        """Begins the flight to where the room's panel `ident` reads well. "ok", or why not."""
        goal = self.mover.panel_spot(ident)
        if isinstance(goal, str):
            return goal
        start = self.mover.standing()
        if isinstance(start, str):
            return start
        if math.hypot(goal[0] - start[0], goal[1] - start[1]) > THERE or abs(placing.wrap(goal[2] - start[2])) > THERE:
            self.path = (start, goal, self.now())
        return "ok"

    def step(self):
        """The next bit of the way. A step that can't be made ends the flight where it is."""
        if self.path is None:
            return
        start, goal, began = self.path
        part = min(1.0, max(0.0, (self.now() - began) / GLIDE))
        eased = part * part * (3 - 2 * part)
        spot = (start[0] + (goal[0] - start[0]) * eased, start[1] + (goal[1] - start[1]) * eased,
                placing.wrap(start[2] + placing.wrap(goal[2] - start[2]) * eased))
        if not self.mover.go(spot).startswith("ok") or part >= 1.0:
            self.path = None
