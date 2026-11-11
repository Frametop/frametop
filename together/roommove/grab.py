"""Grabbing the air (docs/together.md, "Moving yourself"): while both grips are down, the room
goes where the hands go. Each look gives where the two hands are on the floor, and what comes
out is the pull to ask ft-roompanels for (`pull DX DZ DEGREES`, roompanels/moving.py), which
moves the wearer's floor in the room and everything here the other way.

The two places of the room the hands closed on are the anchor. A pull is the move of the floor
that brings the anchor under the hands again: the middle between them moves with the middle of
the hands, and the line between them turns with the line between the hands. The anchor then
follows the pull as it was said, to the digit, so what rounding leaves over is in the next pull
and nothing adds up.

- Both grips, or nothing: one hand alone holds no room.
- A hand that isn't tracked lets go, and so does a hand that is somewhere else all at once: a
  tracking jump must not throw anyone through the room. Holding on, the grab starts afresh
  from where the hands are then.
- Hands closer together than APART on the floor say nothing about a turn, so they only move.
  Hands that closed that near and come apart turn the room from the line they have then.
- A move too small to see is kept for later (hands held still must not make the room buzz).

The rules only: nothing here asks anybody anything.
"""
import math

DEAD = 0.002                       # metres: less than this and under DEAD_TURN is no pull yet
DEAD_TURN = math.radians(0.15)
MOST = 0.5                         # metres between two looks: more is not a hand's move
MOST_TURN = math.radians(45)
APART = 0.15                       # metres between the hands on the floor for their line to turn the room


def turned(yaw, at):
    """(x, z) turned `yaw` radians to the left about the way up, as a floor turns (roompanels/placing.py)."""
    c, s = math.cos(yaw), math.sin(yaw)
    return (at[0] * c + at[1] * s, -at[0] * s + at[1] * c)


def moved(pull, at):
    """Where a place (x, z) on the floor is after `pull` (dx, dz, degrees)."""
    x, z = turned(math.radians(pull[2]), at)
    return (x + pull[0], z + pull[1])


def _middle(pair):
    return ((pair[0][0] + pair[1][0]) / 2, (pair[0][1] + pair[1][1]) / 2)


def _heading(pair):
    """The way the line from the first to the second points, as a turn to the left."""
    return math.atan2(-(pair[1][0] - pair[0][0]), -(pair[1][1] - pair[0][1]))


def _apart(pair):
    return math.hypot(pair[1][0] - pair[0][0], pair[1][1] - pair[0][1])


class Grab:
    def __init__(self):
        self.anchor = None      # ((x, z), (x, z)): where the two held places of the room are on the floor now

    @property
    def held(self):
        return self.anchor is not None

    def step(self, both, hands):
        """One look. `both`: both grips are down. `hands`: where the left and the right hand
        are on the floor, ((x, y, z), (x, y, z)), or None when either isn't known. The pull to
        ask for now, (dx, dz, degrees) as its words will say it, or None."""
        if not both or hands is None:
            self.anchor = None
            return None
        now = ((hands[0][0], hands[0][2]), (hands[1][0], hands[1][2]))
        if self.anchor is None:
            self.anchor = now
            return None
        was, to = _middle(self.anchor), _middle(now)
        if _apart(self.anchor) < APART <= _apart(now):
            # They closed too near each other for their line to say a turn, and came apart: it counts from here.
            self.anchor = tuple((at[0] - to[0] + was[0], at[1] - to[1] + was[1]) for at in now)
        yaw = 0.0
        if _apart(now) >= APART:
            yaw = math.remainder(_heading(now) - _heading(self.anchor), 2 * math.pi)
        went = math.hypot(to[0] - was[0], to[1] - was[1])
        if went > MOST or abs(yaw) > MOST_TURN:
            self.anchor = now       # not a hand's move: hold on from here
            return None
        if went < DEAD and abs(yaw) < DEAD_TURN:
            return None
        about = turned(yaw, was)
        return (round(to[0] - about[0], 5), round(to[1] - about[1], 5), round(math.degrees(yaw), 4))

    def made(self, pull):
        """ft-roompanels made `pull`: the held places moved with everything else."""
        if self.anchor is not None:
            self.anchor = (moved(pull, self.anchor[0]), moved(pull, self.anchor[1]))

    def drop(self):
        """A pull wasn't made, or nobody knows whether: the grab starts afresh at the next look."""
        self.anchor = None


def words(pull):
    """The question to ft-roompanels for `pull`, with every digit step() rounded to."""
    return f"pull {pull[0]:.5f} {pull[1]:.5f} {pull[2]:.4f}"
