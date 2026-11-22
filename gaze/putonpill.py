"""putonpill: what the fit pill says just after the headset goes on, with our tracker in use.

Our tracker's slip filter and your clicks keep the gaze right as the headset sits, so the quick
check's dot isn't needed then (GAZE_PUTON_QUICK=1 brings it back). What a click can't do is put
the headset back where it sat at the calibration, where the tracker is most accurate. So the
pill first reads the fit coach (gaze/fitcoach.py: ft-eyes' pupils and glints against your saved
fit) while you look straight ahead, and says which way to move the headset, live, until it's
back ("Looks good"). A fit as saved says "Looks good" briefly. Changes are fitpill's: ("show",
words, mark), ("say", words, mark) or ("hide",).

`step(now, reading)` takes the coach's reading (fitcoach.assess's, against your saved fit) or
None while there's none (not looking straight ahead yet); `done` is set once it's over.
"""
import fitcoach
import fitpill

LOOK_AFTER = 1.5   # seconds without a reading before it asks you to look straight ahead
TIMEOUT = 30.0     # seconds it guides before giving up (as the put-on check's PUTON_TIMEOUT)
LOOK = ("Look straight ahead", "")


class PutOnPill:
    def __init__(self, now):
        self.since = now
        self.shown = None     # what it says, while shown
        self.moved = False    # it said to move the headset
        self.until = None     # closing then ("Looks good" stays fitpill.GOOD_SECONDS)
        self.done = False
        self.why = None

    def step(self, now, reading):
        if self.done:
            return []
        if self.until is not None:
            return self._end("the headset sits as your saved fit") if now >= self.until else []
        if now - self.since > TIMEOUT:
            return self._end(f"not back after {TIMEOUT:.0f} s")
        if reading is None or reading.get("state") == "waiting":
            return self._say(LOOK) if now - self.since >= LOOK_AFTER and not self.moved else []
        if not fitcoach.moved(reading):
            self.until = now + fitpill.GOOD_SECONDS
            return self._say(fitpill.GOOD)
        self.moved = True
        return self._say(words(reading, self.shown))

    def close(self):
        """Closed from outside (a check opens, the headset comes off)."""
        return self._end("closed") if not self.done else []

    def _say(self, said):
        if said == self.shown:
            return []
        change = ("say" if self.shown else "show", *said)
        self.shown = said
        return [change]

    def _end(self, why):
        self.done, self.why = True, why
        return [("hide",)] if self.shown else []


def words(reading, said=None):
    """Which way to move it: the biggest position term as the slip pill says it, or the IPD hint
    when only that changed."""
    w = fitpill.words(reading.get("terms", {}), said)
    if w != fitpill.GOOD:
        return w
    ipd = next((h["text"] for h in reading.get("hints", []) if h["key"] == "ipd-changed"), None)
    return (ipd, "") if ipd else w
