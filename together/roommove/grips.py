"""Which grips of the Frame's controllers are down, read from SteamVR's web socket
(input/vrws.py). Reading there takes nothing from anyone: a grip still does whatever SteamVR
and the game have it do, which an action of our own at an overlay's priority would take away
(pointer/helper/vrbuttons.h).

`Held` is the rules: what a message says, whose controller it is, and that a controller that
went holds nothing. `VrGrips` is the connection around them: it never raises, waits for SteamVR
a second at most, and while SteamVR is gone no grip is down.

Only changes are told, so a grip let go while nobody listened would stay down here. Whoever
sees that a hand can't be holding on (its controller isn't tracked) says `forget`: from then
on a grip is down only once it is pressed again.
"""
import http.client
import json

GRIP = "/input/grip/click"


class Held:
    def __init__(self):
        self.sides = {}                              # a controller's path at SteamVR -> "left" | "right"
        self.down = {"left": False, "right": False}

    @property
    def both(self):
        return self.down["left"] and self.down["right"]

    def know(self, found):
        """The controllers SteamVR has now ({path: side}). Returns the paths to start and to
        stop following. One that went holds nothing."""
        found = {path: side for path, side in found.items() if side in self.down}
        new, gone = sorted(found.keys() - self.sides.keys()), sorted(self.sides.keys() - found.keys())
        self.sides = found
        for side in self.down:
            if side not in found.values():
                self.down[side] = False
        return new, gone

    def take(self, text):
        """One message of the web socket. Most are other buttons and the fingers' sensing, so
        only one that names the grip is read. True when a grip changed."""
        if GRIP not in text:
            return False
        try:
            msg = json.loads(text)
        except ValueError:
            return False
        if not isinstance(msg, dict) or not isinstance(msg.get("components"), dict) or not isinstance(msg.get("device"), str):
            return False
        side, down = self.sides.get(msg["device"]), msg["components"].get(GRIP)
        if side is None or not isinstance(down, bool) or self.down[side] == down:
            return False
        self.down[side] = down
        return True

    def none(self):
        self.down = dict.fromkeys(self.down, False)


class VrGrips:
    LOOKUP = 3.0     # seconds between asking which controllers there are
    RETRY = 5.0      # ...and between tries while SteamVR isn't there

    def __init__(self, vrws, mailbox):
        self.vrws, self.mailbox = vrws, mailbox
        self.held = Held()
        self.ws = None
        self.next_try = self.next_lookup = 0.0

    @property
    def both(self):
        return self.held.both

    def fileno(self):
        """The socket to wait on, or None while there is none."""
        return None if self.ws is None else self.ws.fileno()

    def look(self, now):
        """Takes what came, and asks now and then which controllers there are (a web request
        to this machine)."""
        try:
            if self.ws is None:
                if now < self.next_try:
                    return
                self.next_try = now + self.RETRY
                self.ws = self.vrws.VrSocket(timeout=1)
                self.ws.open(self.mailbox)
                self.next_lookup = 0.0
            if now >= self.next_lookup:
                self.next_lookup = now + self.LOOKUP
                new, gone = self.held.know(self.vrws.controllers(timeout=1))
                for path in gone:
                    self.ws.subscribe(path, on=False)
                for path in new:
                    self.ws.subscribe(path)
            for text in self.ws.messages(timeout=0):
                self.held.take(text)
        except (OSError, ValueError, AttributeError, TypeError, KeyError, http.client.HTTPException):     # SteamVR went, or answered something else
            self.close()

    def forget(self):
        self.held.none()

    def close(self):
        if self.ws is not None:
            try:
                self.ws.close()
            except OSError:
                pass
        self.ws = None
        self.held.know({})


class Told:
    """--probe: no SteamVR. The grips are as the program's own socket was last told."""

    def __init__(self):
        self.both = False

    def fileno(self):
        return None

    def look(self, now):
        pass

    def forget(self):
        self.both = False

    def close(self):
        self.both = False
