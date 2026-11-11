"""What ft-roommove does, look by look: while this Frame is in a room and both grips are down,
it asks ft-screens where the hands are and ft-roompanels for the pull that keeps the room in
them (grab.py has which pull). Outside a room it listens to no controller at all.

Its questions go out through three functions it is given, each a text in and the answer or None
back: `together` (ft-together's local socket: is there a floor in a room), `screens`
(ft-screens': `reach`) and `panels` (ft-roompanels': `pull`). `grips` says whether both are
down (grips.py). So the tests ask it everything without any of them.

- A pull that isn't made (refused, or no answer) lets go: the grab starts afresh, so the room
  never jumps later by what was missed.
- When ft-screens says the hands can't be had (a controller isn't tracked, a game has them),
  the grips are forgotten too: they count again once pressed again.
"""
import json
import math

from . import grab

STEP = 1 / 60        # seconds between pulls while the room is held
ROOM_EVERY = 1.0     # seconds between asking whether this Frame is in a room


def _numbers(reply, count):
    words = (reply or "").split()
    if words[:1] != ["ok"] or len(words) != count + 1:
        return None
    try:
        got = [float(w) for w in words[1:]]
    except ValueError:
        return None
    return got if all(math.isfinite(v) for v in got) else None


class Puller:
    def __init__(self, together, screens, panels, grips):
        self.together, self.screens, self.panels, self.grips = together, screens, panels, grips
        self.grab = grab.Grab()
        self.in_room = False
        self.room_asked = self.stepped = -1e9
        self.pulls = self.refused = 0

    def wait(self, now):
        """Seconds until the next look is due, if nothing comes before."""
        room = self.room_asked + ROOM_EVERY - now
        step = self.stepped + STEP - now if self.in_room and self.grips.both else room
        return max(0.0, min(room, step))

    def look(self, now):
        if now - self.room_asked >= ROOM_EVERY:
            self.room_asked = now
            self.in_room = _numbers(self.together("floor"), 3) is not None
        if not self.in_room:
            self.grips.close()
            self.grab.drop()
            return
        self.grips.look(now)
        if not self.grips.both:
            self.grab.drop()
            return
        if now - self.stepped < STEP:
            return
        self.stepped = now
        reply = self.screens("reach")
        at = _numbers(reply, 6)
        if at is None:
            if reply and reply.startswith("error"):
                self.grips.forget()
            self.grab.drop()
            return
        pull = self.grab.step(True, (at[:3], at[3:]))
        if pull is None:
            return
        if (self.panels(grab.words(pull)) or "").startswith("ok "):
            self.grab.made(pull)
            self.pulls += 1
        else:
            self.grab.drop()
            self.refused += 1

    def state(self):
        return "ok " + json.dumps({"room": self.in_room, "both": bool(self.in_room and self.grips.both), "held": self.grab.held,
                                   "pulls": self.pulls, "refused": self.refused})

    def end(self):
        self.grips.close()
