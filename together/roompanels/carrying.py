"""Panels moved by someone other than their owner (docs/together.md, "Control and moving").

A panel has one place, the room's, and its owner's Frame says what that is: the panel is a
screen there. On every other Frame it is a panel of ft-screens' that its viewer can take hold
of like any other. So each look at one of them here asks where it stands (ft-screens' "get"):

  where the room has it      nothing to do
  where it was put, but the  the room moved it: it is put there
  room has it elsewhere
  anywhere else              its viewer moved it. Its owner is asked to take it there
                             (ft-together's "move"), and for SETTLE seconds after the last
                             such ask the room is waited for to come round. A room that
                             doesn't (its owner's Frame didn't take it) gets it back.
  ...and its viewer may not  it is put back where the room has it once it rests (or after
  move it                    REST seconds in their hand), and when they had pulled it more
                             than PULLED metres nearer, they are taken to it instead (`fly`)

A viewer who made it larger, smaller or bent it gets it back as the room has it: how large a
panel is stays its owner's alone.

And on its owner's Frame, where the others put this Frame's panels (ft-together's "moves") is
where their screens go (`brought`). The keeper then finds them there and tells the room, as it
does when its own person moved them.

The rules only. Its questions go out through the functions the keeper has: `together` and
`screens`, each a text in and the answer or None back.
"""
import math

from . import placing

NEAR = 0.003     # metres, and of an axis' length: a panel this near a place stands there
SETTLE = 1.0     # seconds the room has to come round to where a panel was carried
REST = 2.0       # seconds a panel that may not be moved may be held before it is put back
PULLED = 0.3     # metres nearer its viewer's head: a pull


def stands(pose, other):
    """Whether two poses (the twelve numbers of "pose") are one place."""
    return all(abs(a - b) <= NEAR for a, b in zip(pose, other))


def _ok(reply):
    return reply is not None and reply.startswith("ok")


def _numbers(words, count):
    try:
        got = [float(w) for w in words]
    except ValueError:
        return None
    return got if len(got) == count and all(math.isfinite(v) for v in got) else None


class Carrying:
    def __init__(self, together, screens, now, fly):
        """`fly(ident)`: takes this Frame's person to where the room's panel `ident` reads well."""
        self.together, self.screens, self.now, self.fly = together, screens, now, fly
        self.asked = {}      # a panel's id -> (where it stood when its owner was last asked to take it there, when)
        self.refused = {}    # ...-> (where it stood at the last look, since when it is known that it may not be moved)
        self.sized = {}      # ...-> (metres, curve), as ft-screens said them when it was last put

    def only(self, idents):
        """The panels that are shown now: what is known of any other is forgotten."""
        for known in (self.asked, self.refused, self.sized):
            for ident in [i for i in known if i not in idents]:
                del known[ident]

    # ---- someone else's panel, here ----

    def place(self, ident, number, floor, want, placed):
        """One look at the room's panel `ident`, which is screen `number` here. `want` is the
        panel as the room has it now, `placed` as it was last put here (None: not yet).
        Returns what it is put as from now on."""
        target = placing.to_floor(floor, want)
        got = None if placed is None else placing.parse_get(self.screens(f"get {number}") or "")
        if got is None:
            return placed if placed == want else self._put(ident, number, target, want, placed)
        here = placing.twelve(got)
        if stands(here, target):
            self.asked.pop(ident, None), self.refused.pop(ident, None)
            same = (placed["metres"], placed["curve"]) == (want["metres"], want["curve"]) and self.sized.get(ident, (got["metres"], got["curve"])) == (got["metres"], got["curve"])
            return want if same or self._size(ident, number, want) else placed
        asked = self.asked.get(ident)
        if asked is not None and stands(here, asked[0]):
            if self.now() - asked[1] <= SETTLE:
                return placed       # the room hasn't come round yet
            return self._put(ident, number, target, want, placed)       # its owner's Frame didn't take it there
        if asked is None and stands(here, placing.to_floor(floor, placed)):
            return self._put(ident, number, target, want, placed)       # it stands where it was put: the room moved it
        return self._carried(ident, number, floor, got, target, want, placed)

    def _carried(self, ident, number, floor, got, target, want, placed):
        """Its viewer moved it."""
        here = placing.twelve(got)
        refused = self.refused.get(ident)
        if refused is None:
            to = placing.to_room(floor, got)
            reply = self.together(f"move {ident} " + " ".join(f"{v:.4f}" for v in to["at"]) + " " + " ".join(f"{v:.5f}" for v in to["turn"]))
            if reply == "ok":
                self.asked[ident] = (here, self.now())
            elif reply is not None and reply != "error too fast":       # (those two: asked again at the next look)
                self.asked.pop(ident, None)
                self.refused[ident] = (here, self.now())
            return placed
        if not stands(here, refused[0]) and self.now() - refused[1] <= REST:
            self.refused[ident] = (here, refused[1])       # still on its way in their hand
            return placed
        nearer = self._nearer(got["at"], (target[3], target[7], target[11]))
        put = self._put(ident, number, target, want, placed)
        if nearer > PULLED:
            self.fly(ident)
        return put

    def _nearer(self, at, home):
        """How much nearer its viewer's head a panel at `at` is than at `home`, in metres."""
        words = (self.screens("head") or "").split()
        head = _numbers(words[1:], 4) if words[:1] == ["ok"] else None
        return 0.0 if head is None else math.dist(head[:3], home) - math.dist(head[:3], at)

    def _put(self, ident, number, target, want, placed):
        """The panel where and as the room has it. `placed` when ft-screens didn't take it."""
        posed = _ok(self.screens(f"pose {number} " + " ".join(f"{v:.5f}" for v in target)))
        return want if self._size(ident, number, want) and posed else placed

    def _size(self, ident, number, want):
        asked = [f"width {number} {want['metres']:.3f}", f"curve {number} {want['curve']:.3f}"]
        if not all([_ok(self.screens(a)) for a in asked]):
            return False
        got = placing.parse_get(self.screens(f"get {number}") or "")
        if got is None:
            self.sized.pop(ident, None)
        else:
            self.sized[ident] = (got["metres"], got["curve"])
        return True

    # ---- this Frame's own panels, moved by the others ----

    def brought(self, floor, screen_of):
        """Where the others put this Frame's panels since the last look: each screen goes there.
        `screen_of`: {a panel's number among this Frame's: its screen}."""
        words = (self.together("moves") or "").split()
        if len(words) < 2 or words[0] != "ok":
            return
        for entry in words[2:]:
            got = _numbers(entry.split(":"), 9)
            if got is None or got[0] not in screen_of:
                continue
            pose = placing.to_floor(floor, {"at": got[2:5], "turn": got[5:9]})
            self.screens(f"pose {screen_of[int(got[0])]} " + " ".join(f"{v:.5f}" for v in pose))
