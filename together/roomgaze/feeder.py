"""What ft-roomgaze keeps true (docs/together.md, "Gaze"): this Frame's ft-together knows whether
its wearer's eyes move their pointer, and, only while the wearer shares where they look with
the room they are in, where that is.

Each second (a look):
 - ft-together is asked whether the gaze is shared (`gaze`). While it isn't there, nobody else
   is asked anything.
 - The pointer helper is asked whether gaze mode is on (`gaze ?`), and ft-together is told
   `gaze pointer eyes` or `gaze pointer hand`. When the helper doesn't answer, nothing is told,
   and ft-together comes to take the pointer for one the eyes move (local/gaze.h): what isn't
   known isn't sent.
 - While the gaze is shared and the wearer isn't away, the gaze service is asked to keep
   running (`wake`), whatever gaze mode says, and the feed is on.

Five times a second while the feed is on (a sample): the gaze service is asked for its `status`,
and the direction it last had goes to ft-together as `gaze say YAW PITCH`, smoothed. Only a
direction is read: degrees to the wearer's left and up from the way the head faces. One that is
old, or from a gaze service that isn't running, is told to nobody, and ft-together sends poses
without a gaze a second after the last was told.

The rules only. Its questions go out through three functions it is given, each a text in and
the answer or None back: `together` (ft-together's local socket), `helper` (the pointer helper)
and `gazed` (the gaze service, gaze/ft-gazed). So the tests ask it everything without any of
them.
"""
import json
import math

LOOK_S = 1.0      # between looks
IDLE_S = 2.0      # ...while ft-together isn't there
SAMPLE_S = 0.2    # between samples while the feed is on
WAKE_S = 3        # how long the gaze service is asked to keep running, at each look
FRESH_S = 0.5     # a direction older than this isn't where they look now
SMOOTH_S = 0.3    # the smoothing's time constant: after a second without a direction, the next is nearly all of it
FARTHEST = 90.0   # degrees: further than this is no direction of an eye's


def _number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def shares(reply):
    """ft-together's answer to `gaze`: whether the wearer shares where they look, or None when
    that isn't its answer."""
    if not reply or not reply.startswith("ok {"):
        return None
    try:
        said = json.loads(reply[3:])
    except ValueError:
        return None
    return said["shared"] if isinstance(said, dict) and isinstance(said.get("shared"), bool) else None


def there(reply):
    """ft-together's answer to `state`: whether its wearer is in a room and not away. None when
    that isn't its answer."""
    if not reply or not reply.startswith("ok {"):
        return None
    try:
        state = json.loads(reply[3:])
    except ValueError:
        return None
    if not isinstance(state, dict) or not _number(state.get("away")):
        return None
    return state.get("mode") in ("hosting", "joined") and state["away"] == 0


def eyes(reply):
    """The pointer helper's answer to `gaze ?`: True when the eyes move the pointer (gaze mode
    is on), False when the hand does, None when it isn't an answer."""
    words = (reply or "").split()
    if len(words) >= 2 and words[0] == "ok" and words[1] in ("on", "off"):
        return words[1] == "on"
    return None


def sample(reply):
    """The gaze service's `status` read: (yaw, pitch) in degrees, to the left and up from the
    way the head faces, or None when it has no direction that is where the wearer looks now."""
    try:
        status = json.loads(reply or "")
    except ValueError:
        return None
    if not isinstance(status, dict) or status.get("awake") is not True:
        return None
    age, last = status.get("sample_age_s"), status.get("last")
    if not _number(age) or not 0 <= age <= FRESH_S or not isinstance(last, list) or len(last) < 2:
        return None
    yaw, pitch = last[0], last[1]
    if not _number(yaw) or not _number(pitch) or abs(yaw) > FARTHEST or abs(pitch) > FARTHEST:
        return None
    return float(yaw), float(pitch)


class Feeder:
    def __init__(self, together, helper, gazed, clock):
        self._together, self._helper, self._gazed, self._clock = together, helper, gazed, clock
        self._looked = None
        self._smooth, self._at = None, None
        self.running = False   # ft-together is there
        self.pointer = None    # what it was last told moves the pointer: "eyes", "hand"
        self.shared = False    # the wearer shares where they look
        self.feeding = False   # ...and is there: the gaze service is asked, and its direction told
        self.told = False      # the last sample was a direction, and ft-together took it

    def tick(self):
        """One pass: a look when one is due, and a sample while the feed is on. Returns the
        seconds until the next."""
        now = self._clock()
        if self._looked is None or now - self._looked >= LOOK_S - 1e-6:
            self._looked = now
            self._look()
        if not self.feeding:
            return LOOK_S if self.running else IDLE_S
        self._sample(now)
        return SAMPLE_S

    def _look(self):
        shared = shares(self._together("gaze"))
        self.running = shared is not None
        if not self.running:
            self.pointer, self.shared = None, False
            return self._stop()
        moves = eyes(self._helper("gaze ?"))
        if moves is not None:
            word = "eyes" if moves else "hand"
            self.pointer = word if (self._together("gaze pointer " + word) or "").startswith("ok") else None
        else:
            self.pointer = None
        self.shared = shared
        if not (shared and there(self._together("state")) is True):
            return self._stop()
        self.feeding = True
        self._gazed("wake %d" % WAKE_S)

    def _stop(self):
        self.feeding = self.told = False

    def _sample(self, now):
        found = sample(self._gazed("status"))
        if found is None:
            self.told = False
            return
        if self._smooth is None:
            self._smooth = found
        else:
            part = 1 - math.exp(-max(0.0, now - self._at) / SMOOTH_S)
            self._smooth = tuple(was + (new - was) * part for was, new in zip(self._smooth, found))
        self._at = now
        self.told = (self._together("gaze say %.1f %.1f" % self._smooth) or "").startswith("ok")
