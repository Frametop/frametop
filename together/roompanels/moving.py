"""Moving this Frame's person through the room (docs/together.md, "Moving yourself"). Nobody is
carried anywhere: their own floor moves in the room under them (ft-together's "floor move"),
and so that the room stands still around them, everything that stands in it here moves the
other way on that floor (ft-screens' "shift"). The others see them at their new place from
their next pose on, and the panels they share stay where the room has them.

  go(spot)       to a spot of the room, facing its way
  go_person(n)   to in front of someone, facing them
  go_panel(id)   to where a shared panel reads well, facing it
  pull(...)      the room pulled past by so much on your own floor (grabbing the air)
  step(...)      so far the way you face or to your right, and a turn where you stand (a key)

The rules only. Its questions go out through the two functions the keeper has too, each a text
in and the answer or None back: `together` (ft-together's local socket) and `screens`
(ft-screens'). What the room says of the others comes from them: a place that isn't a number,
or is further off than a floor may lie, moves nobody.
"""
import json
import math

from . import placing

BESIDE = 1.2                       # metres in front of someone you go to
SPAN = math.radians(60)            # of your view, across: what a panel you go to fills
NEAREST, FARTHEST = 0.6, 3.0       # metres from a panel you go to, whatever its size
FURTHEST = 1000.0                  # metres: where ft-together lets a floor lie (together/local/floor.h)
MOST_PULL = 5.0                    # metres at once: a pull is a hand's move, not a jump
MOST_STEP = 5.0                    # ...and a step is a step


def beside(eyes, yaw):
    """The spot in front of someone whose eyes are over (x, z) of the room facing `yaw`,
    turned to face them."""
    ahead = placing.turned(yaw, (0.0, 0.0, -1.0))
    return (eyes[0] + ahead[0] * BESIDE, eyes[1] + ahead[2] * BESIDE, placing.wrap(yaw + math.pi))


def reading(panel):
    """The spot a room's panel reads well from: in front of it, level, as far off as makes it
    fill SPAN of the view. One that lies flat is read from its lower edge. None when it has no
    front to stand before."""
    x, y, z = placing.axes(panel["turn"])
    front = (z[0], z[2])
    if math.hypot(*front) < 0.3:       # facing up or down: stand where its top points away from
        front = (-y[0], -y[2])
    length = math.hypot(*front)
    if length < 0.3:
        return None
    front = (front[0] / length, front[1] / length)
    away = min(FARTHEST, max(NEAREST, panel["metres"] / 2 / math.tan(SPAN / 2)))
    return (panel["at"][0] + front[0] * away, panel["at"][2] + front[1] * away, math.atan2(front[0], front[1]))


USAGE = "error go X Z DEGREES | go person N | go panel ID | pull DX DZ DEGREES | step FORWARD RIGHT DEGREES"


def answer(mover, words):
    """The answer to one of ft-roompanels' questions when it is about moving, or None."""
    if words[:1] not in (["go"], ["pull"], ["step"]):
        return None
    if words[0] == "go" and len(words) == 3 and words[1] in ("person", "panel") and words[2].isdigit() and len(words[2]) < 4:
        return (mover.go_person if words[1] == "person" else mover.go_panel)(int(words[2]))
    got = _numbers(words[1:], 3)
    if got is None:
        return USAGE
    if words[0] == "pull":
        return mover.pull(*got)
    if words[0] == "step":
        return mover.step(*got)
    if max(abs(got[0]), abs(got[1])) > FURTHEST or abs(got[2]) > 360:
        return "error that is too far"
    return mover.go((got[0], got[1], math.radians(got[2])))


def _numbers(words, count):
    try:
        got = [float(w) for w in words]
    except ValueError:
        return None
    return got if len(got) == count and all(math.isfinite(v) for v in got) else None


def _reply(text, count):
    words = (text or "").split()
    return _numbers(words[1:], count) if words[:1] == ["ok"] else None


def _shift_words(shift):
    return f"shift {shift[0]:.5f} {shift[1]:.5f} {math.degrees(shift[2]):.4f}"


class Mover:
    def __init__(self, together, screens):
        self.together, self.screens = together, screens

    # ---- what its person asks ----

    def go(self, spot):
        """Their head comes to stand over `spot` (x, z, yaw) of the room, facing its way."""
        here = self._here()
        return here if isinstance(here, str) else self._to(*here, spot)

    def step(self, forward, right, degrees):
        """They go `forward` metres the way their head faces and `right` metres to its right
        (over the floor, wherever they look up or down), and turn `degrees` to the left where
        they then stand."""
        if math.hypot(forward, right) > MOST_STEP or abs(degrees) > 180:
            return "error that is more than a step"
        here = self._here()
        if isinstance(here, str):
            return here
        floor, head = here
        at = placing.turned(floor[2], (head[0], 0.0, head[1]))
        facing = floor[2] + head[2]
        way = placing.turned(facing, (right, 0.0, -forward))
        return self._to(floor, head, (floor[0] + at[0] + way[0], floor[1] + at[2] + way[2], placing.wrap(facing + math.radians(degrees))))

    def go_person(self, place):
        room = self._room()
        if room is None:
            return "error no answer from ft-together"
        if place == room.get("me"):
            return "error that is you"
        person = next((p for p in room.get("people", []) if p.get("id") == place), None)
        if person is None:
            return f"error nobody is at place {place}"
        seen = self._where().get(place)
        if seen is None:       # no pose of theirs lately: the spot the room gave them
            spot = _numbers(person.get("spot", []), 4)
            seen = None if spot is None else (spot[0], spot[2], spot[3])
        if seen is None:
            return f"error where {place} is isn't known"
        return self.go(beside((seen[0], seen[1]), seen[2]))

    def go_panel(self, ident):
        room = self._room()
        if room is None:
            return "error no answer from ft-together"
        panel = next((p for p in room.get("panels", []) if p.get("id") == ident), None)
        if panel is None:
            return f"error no panel {ident} in the room"
        spot = reading(panel)
        return self.go(spot) if spot else f"error panel {ident} has no front to stand before"

    def pull(self, dx, dz, degrees):
        """The room moves by (dx, dz) metres on their own floor and turns `degrees` to the left
        about the floor's middle: they move the other way through it."""
        if math.hypot(dx, dz) > MOST_PULL or abs(degrees) > 180:
            return "error that is more than a pull"
        floor = self._floor()
        return floor if isinstance(floor, str) else self._shift(floor, (dx, dz, math.radians(degrees)))

    # ---- the move ----

    def _here(self):
        """(this Frame's floor in the room, the head on it as (x, z, yaw)), or the error that
        says why one of them isn't known."""
        floor = self._floor()
        if isinstance(floor, str):
            return floor
        head = _reply(self.screens("head"), 4)
        if head is None:
            return "error no head pose (is the headset on?)"
        return floor, (head[0], head[2], math.radians(head[3]))

    def _to(self, floor, head, spot):
        """The head, at `head` of the floor, comes to stand over `spot` (x, z, yaw) of the room."""
        to = placing.lay((spot[0], 0.0, spot[1], spot[2]), head)
        return self._shift(floor, placing.then(placing.back(to), floor))

    def _shift(self, floor, shift):
        """Everything here moves by `shift` on the floor, and the floor so that the room stands
        still. ft-screens is told in words, so the floor follows what the words say."""
        words = _shift_words(shift)
        said = _numbers(words.split()[1:], 3)
        said = (said[0], said[1], math.radians(said[2]))
        to = placing.then(floor, placing.back(said))
        if max(abs(to[0]), abs(to[1])) > FURTHEST:
            return "error that is too far"
        reply = self.screens(words)
        if _reply(reply, 1) is None:
            return reply if reply and reply.startswith("error") else "error no answer from ft-screens"
        moved = self.together(f"floor move {to[0]:.6f} {to[1]:.6f} {to[2]:.6f}")
        if _reply(moved, 3) is None:
            self.screens(_shift_words(placing.back(said)))   # the room didn't move: nor may the panels
            return moved if moved and moved.startswith("error") else "error no answer from ft-together"
        return f"ok {to[0]:.3f} {to[1]:.3f} {math.degrees(to[2]):.1f}"

    # ---- what ft-together knows ----

    def _floor(self):
        """This Frame's floor in the room (x, z, yaw), or the error that says why there is none."""
        said = self.together("floor")
        got = _reply(said, 3)
        if got is not None:
            return tuple(got)
        if said is None:
            return "error no answer from ft-together"
        return said if said.startswith("error") else "error your floor isn't laid in the room yet"

    def _room(self):
        reply = self.together("state") or ""
        try:
            state = json.loads(reply[3:]) if reply.startswith("ok {") else None
        except ValueError:
            state = None
        return state if isinstance(state, dict) else None

    def _where(self):
        """{place: (x, z, yaw)} of those whose poses come."""
        words = (self.together("where") or "").split()
        seen = {}
        for entry in words[2:] if words[:1] == ["ok"] else []:
            got = _numbers(entry.split(":"), 5)
            if got is not None and got[0] == int(got[0]):
                seen[int(got[0])] = (got[1], got[3], got[4])
        return seen
