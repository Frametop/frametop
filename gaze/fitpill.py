"""fitpill: the headset-fit pill, a small capsule low in view that says which way to nudge the
headset while it sits off the seating our tracker was calibrated at, then "Looks good" (gaze
/README.md, "Headset fit"). The gaze service drives it (gazecheck.py, the fit watch); this file
only decides what it says, from readings of where the headset sits:

  terms   the fit coach's position terms, {"up", "left"} in degrees, fitcoach.assess's sign:
          ft-eyes' slip filter's estimate (gazecheck.pill_terms), or None while it isn't sure
          (just put on, before a steady look or a click)

Pill.step(now, terms) returns what changed for the panel, or None:
  ("show", words, arrow)   open it        ("say", words, arrow)   change what it says
  ("hide",)                close it
arrow is "up", "down", "left", "right" (the way to nudge it), "good" or "" (no mark).

It opens after the headset has sat past SHOW_DEG for SHOW_HOLD, follows each reading while it's
open (down to GOOD_DEG, so it guides the whole way back, not just under the trigger), says
"Looks good" once within GOOD_DEG for GOOD_HOLD, and closes GOOD_SECONDS later. One that never
gets there closes after PILL_MAX and waits COOLDOWN before opening again. Thresholds are
provisional: 1 degree here is about 1.3 px of pupil on the maintainer's headset (fitcoach).
"""
SHOW_DEG = 5.0       # a term past this, for SHOW_HOLD: the pill opens (fitcoach.MOVED_DEG)
SHOW_HOLD = 2.0      # seconds
GOOD_DEG = 2.5       # every term within this, for GOOD_HOLD: "Looks good"
GOOD_HOLD = 0.6
GOOD_SECONDS = 1.5   # "Looks good" stays this long
BIG_DEG = 10.0       # past this "Nudge right", under it "Nudge slightly right"
SWITCH_DEG = 2.0     # hysteresis: "Nudge right" until under BIG_DEG - SWITCH_DEG; the other
                     # direction once its term is SWITCH_DEG bigger than the one said
STALE = 3.0          # seconds without a reading while open: "Look straight ahead"
PILL_MAX = 30.0      # seconds open without getting good: it closes
COOLDOWN = 120.0     # and doesn't open again before
AGAIN = 10.0         # after "Looks good", not again before

GOOD = ("Looks good", "good")
LOOK = ("Look straight ahead", "")


def biggest(terms):
    """(term, degrees) of the bigger position term, or (None, 0.0)."""
    keys = [k for k in ("up", "left") if k in (terms or {})]
    if not keys:
        return None, 0.0
    k = max(keys, key=lambda k: abs(terms[k]))
    return k, terms[k]


WAYS = {"up": ("up", "down"), "left": ("left", "right")}


def words(terms, said=None):
    """What to say for terms past GOOD_DEG: (words, arrow); GOOD within it. said (what the
    pill says now) holds its direction and size within SWITCH_DEG, so it doesn't flicker
    between two terms nearly the same size, or around BIG_DEG."""
    k, v = biggest(terms)
    if k is None or abs(v) <= GOOD_DEG:
        return GOOD
    way = WAYS[k][0] if v > 0 else WAYS[k][1]
    if said and said[1] in WAYS["up"] + WAYS["left"] and said[1] != way:
        k0 = "up" if said[1] in WAYS["up"] else "left"
        v0 = terms.get(k0, 0.0)
        if (v0 > 0) == (said[1] == WAYS[k0][0]) and abs(v0) > GOOD_DEG and abs(v) - abs(v0) < SWITCH_DEG:
            k, v, way = k0, v0, said[1]
    big = abs(v) > BIG_DEG or (bool(said) and said[0] == "Nudge " + way and abs(v) > BIG_DEG - SWITCH_DEG)
    return ("Nudge " if big else "Nudge slightly ") + way, way


class Pill:
    def __init__(self):
        self.state = "hidden"     # hidden, open, good
        self.off_since = None     # past SHOW_DEG since (hidden)
        self.good_since = None    # within GOOD_DEG since (open)
        self.opened = 0.0
        self.read_at = 0.0        # the last reading
        self.until = 0.0          # "Looks good" closes then
        self.next_at = 0.0        # not open again before
        self.said = None

    @property
    def shown(self):
        return self.state != "hidden"

    def step(self, now, terms):
        if self.state == "hidden":
            return self._hidden(now, terms)
        if self.state == "good":
            if now >= self.until:
                return self._close(now, AGAIN)
            return None
        return self._open(now, terms)

    def close(self, now):
        """Closed from outside (a check opens, gaze mode off, the panel restarts): AGAIN before it
        opens again."""
        if self.shown:
            return self._close(now, AGAIN)
        self.off_since = None
        return None

    def _hidden(self, now, terms):
        if terms is None:
            if self.off_since is not None and now - self.read_at > STALE:
                self.off_since = None   # (readings stopped: the next starts SHOW_HOLD over)
            return None
        self.read_at = now
        if abs(biggest(terms)[1]) <= SHOW_DEG:
            self.off_since = None
            return None
        if self.off_since is None:
            self.off_since = now
        if now - self.off_since < SHOW_HOLD or now < self.next_at:
            return None
        self.state, self.opened, self.read_at, self.good_since = "open", now, now, None
        self.said = words(terms)
        return ("show", *self.said)

    def _open(self, now, terms):
        if now - self.opened > PILL_MAX:
            return self._close(now, COOLDOWN)
        if terms is None:
            if now - self.read_at > STALE and self.said != LOOK:
                return self._say(LOOK)
            return None
        self.read_at = now
        said = words(terms, self.said)
        if said == GOOD:
            if self.good_since is None:
                self.good_since = now
            if now - self.good_since < GOOD_HOLD:
                return None
            self.state, self.until = "good", now + GOOD_SECONDS
            return self._say(GOOD)
        self.good_since = None
        return self._say(said)

    def _say(self, said):
        if said == self.said:
            return None
        self.said = said
        return ("say", *said)

    def _close(self, now, wait):
        self.state, self.off_since, self.good_since, self.said = "hidden", None, None, None
        self.next_at = now + wait
        return ("hide",)
