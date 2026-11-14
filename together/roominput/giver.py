"""What ft-roominput does with the input of the others in a room (docs/together.md, "Control
and moving"): each act ft-together passed on (local/inputs.h) goes to ft-screens as a "peer"
command (screens/peerinput.h), on the screen of this desktop that the panel is.

Whose input is taken, for which panel and when is ft-together's to say, and it said so before
an act came here. What is left to do here:

- A panel is a number of this Frame's in the room; the screen it is comes from ft-roompanels,
  which shared it. An act for a panel that is no screen now is dropped.
- While someone has been given a panel, ft-screens is told so about once a second ("peer on"),
  and from then tells ft-together when the wearer's own hand is on the desktop.
- When nobody has one any more, and when ft-together stops answering, whatever they held is let
  go ("peer letgo"). ft-together lets go for them itself when it takes a panel back; this is
  for the times it can't.

The rules only. Its questions go out through three functions it is given: `together`
(ft-together's local socket) and `panels` (ft-roompanels'), each a text in and the answer or
None back, and `screens` (ft-screens'), a text in and nothing back. So the tests ask it
everything without any of them.

An act is bytes from ft-together, which bounded them (proto/control.h). They are read here as
whole numbers only, and anything that isn't an act is dropped.
"""
import json
import struct

INPUT = 0x85                      # an act's datagram (local/datagrams.h)
LETGO, MOVE, BUTTON, SCROLL, KEY = range(5)
WORDS = {MOVE: "move", BUTTON: "button", SCROLL: "scroll", KEY: "key"}
PANELS = 8                        # a Frame's panels in a room: a panel's id is its place * 8 + its number
ASK_AGAIN = 0.25                  # seconds before ft-roompanels is asked again about a panel nobody knows


def given(reply):
    """ft-together's "control" read: the panel numbers of this Frame's that someone was given
    (a set), or None when that isn't its answer."""
    if not reply or not reply.startswith("ok {"):
        return None
    try:
        return {int(g["number"]) for g in json.loads(reply[3:])["given"]}
    except (ValueError, TypeError, KeyError):
        return None


def shared(reply):
    """ft-roompanels' "state" read: {panel number: screen} for what the room has from this
    Frame; empty when that isn't its answer."""
    if not reply or not reply.startswith("ok {"):
        return {}
    try:
        return {int(s["panel"]): int(s["screen"]) for s in json.loads(reply[3:])["shares"] if s["told"] is True}
    except (ValueError, TypeError, KeyError):
        return {}


def read(datagram):
    """An act's datagram as (panel, what, a, b), or None."""
    if len(datagram) != 12 or datagram[0] != INPUT:
        return None
    return struct.unpack("<BBii", datagram[2:])


class Giver:
    def __init__(self, together, panels, screens, clock):
        self._together, self._panels, self._screens, self._clock = together, panels, screens, clock
        self._screen = {}       # panel number -> screen, as ft-roompanels last said
        self._asked = None      # ...and when
        self.live = False       # someone has been given a panel
        self.passed = self.dropped = 0

    def look(self):
        """Once a second or so."""
        has = given(self._together("control"))
        if has:
            self._learn()
            self._screens("peer on")
        elif self.live:
            self._screens("peer letgo")
        self.live = bool(has)

    def act(self, datagram):
        """One datagram from ft-together."""
        got = read(datagram)
        if got is None:
            return
        panel, what, a, b = got
        if what == LETGO:
            self._screens("peer letgo")
            return
        number = panel % PANELS
        if what not in WORDS or (number not in self._screen and not self._again(number)):
            self.dropped += 1
            return
        self.passed += 1
        self._screens(f"peer {self._screen[number]} {WORDS[what]} {a} {b}")

    def end(self):
        """The program stops: nothing of theirs stays down."""
        self._screens("peer letgo")

    def _learn(self):
        self._screen, self._asked = shared(self._panels("state")), self._clock()

    def _again(self, number):
        """A panel given since the last look: ft-roompanels is asked now, not too often."""
        if self._asked is None or self._clock() - self._asked >= ASK_AGAIN:
            self._learn()
        return number in self._screen
