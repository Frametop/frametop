"""What ft-roomaway keeps true while this Frame is in a room (docs/together.md, "Away"): the
room knows whether its wearer is there. With the headset off their head they are away ("off"),
with Frametop paused for a game they are in a game ("game"), and otherwise they are back.

The rules only. Its questions go out through three functions it is given, each a text in and
the answer or None back: `together` (ft-together's local socket), `helper` (the pointer helper,
which knows whether the headset is worn) and `relay` (the input relay, which knows whether
Frametop is paused). So the tests ask it everything without any of them.

Someone nobody can tell about is taken to be there: a Frame without the pointer helper running
still has a wearer, and the room is told nothing it doesn't know.
"""
import json

GAME, OFF = 1, 2    # how the room has someone away (proto/messages.h)
WORDS = {0: "back", GAME: "game", OFF: "off"}
FRESH = 5.0         # seconds an answer about the headset or the pause stands for


def worn(reply):
    """The pointer helper's answer to "gaze ? headset": True, False, or None when it isn't one."""
    words = (reply or "").split()
    if len(words) >= 3 and words[0] == "ok" and words[2] in ("worn", "away"):
        return words[2] == "worn"
    return None


def paused(reply):
    """The relay's answer to "pause ?": True, False, or None when it isn't one."""
    try:
        said = json.loads(reply or "")
    except ValueError:
        return None
    if isinstance(said, dict) and said.get("t") == "pause" and isinstance(said.get("paused"), bool):
        return said["paused"]
    return None


def away(is_worn, is_paused):
    """How the wearer is away, from what is known of each (None: not known)."""
    if is_worn is False:
        return OFF
    return GAME if is_paused else 0


def mine(reply):
    """How the room has this Frame's wearer away, from ft-together's "state": a number, or None
    while this Frame is in no room."""
    if not reply or not reply.startswith("ok "):
        return None
    try:
        state = json.loads(reply[3:])
        if state.get("mode") not in ("hosting", "joined"):
            return None
        return next(int(p["away"]) for p in state["people"] if p["id"] == state["me"])
    except (ValueError, TypeError, KeyError, AttributeError, StopIteration):
        return None


class Known:
    """An answer and when it came: it stands for FRESH seconds."""

    def __init__(self):
        self.value, self.at = None, None

    def take(self, value, now):
        if value is not None:
            self.value, self.at = value, now

    def get(self, now):
        return self.value if self.at is not None and now - self.at <= FRESH else None


class Teller:
    def __init__(self, together, helper, relay, clock):
        self._together, self._helper, self._relay, self._clock = together, helper, relay, clock
        self._worn, self._paused = Known(), Known()
        self.in_room = False
        self.told = None    # the last thing the room was told: "back", "game", "off"

    def tick(self):
        """One look. Outside a room nobody else is asked anything."""
        has = mine(self._together("state"))
        self.in_room = has is not None
        if not self.in_room:
            self.told = None
            return
        now = self._clock()
        self._worn.take(worn(self._helper("gaze ? headset")), now)
        self._paused.take(paused(self._relay("pause ?")), now)
        want = away(self._worn.get(now), self._paused.get(now))
        if want != has and (self._together("away " + WORDS[want]) or "").startswith("ok"):
            self.told = WORDS[want]
