"""steamlook: SteamVR's look while it holds still, sent to our own tracker as a weak reading of
its slip (ft-eyes' "look"; gaze/tracker/slipfilter.py).

The gaze service (ft-gazed) feeds every sample through SteamLooks.step while Eye tracker is Own
tracker and no check runs. Once SteamVR's gaze has held within STEADY_DEG of its median for
WINDOW s, at most every EVERY s, step() returns (t, yaw, pitch): where SteamVR says you looked
over the WINDOW before t. ft-eyes reads its pupils over the same frames against that look.

The look is both eyes' mean, as fusion.json's affine is fitted on (gazecal.Fusion.mean), only
with both seen; with SteamVR tracking one eye (Track Dominant Eye Only), that eye's alone. It
goes through fusion.json's affine only if that was fitted at the same calibration as ours
(made within SAME_CALIBRATION s of it, both eyes): an older affine is for another day's
readings. Raw, SteamVR reads looks out from the centre up to a quarter too far (2026-11-22's
dots), and the slip filter takes SteamVR's error as one bias, which a scale isn't: so a raw look
counts only within RAW_DEG of straight ahead, and a corrected one within CENTRE_DEG (where the
affine's residual is near the same everywhere; the fit pill sits 12 below).
"""
import math
import statistics
import time
from collections import deque

WINDOW = 0.3            # s a look must hold (ft-eyes' LOOK_WINDOW)
STEADY_DEG = 1.5        # within this of its median over the window
EVERY = 0.5             # s between looks sent (closer ones are much the same reading)
MIN_SAMPLES = 5
SAME_CALIBRATION = 120.0  # s between fusion.json's made and our calibration's: the same calibration
CENTRE_DEG = 15.0
RAW_DEG = 8.0


def same_calibration(fusion, made, one_eye):
    """fusion.json was fitted at our calibration made `made` ("%Y-%m-%d %H:%M:%S"), both eyes
    seen, and SteamVR tracks both now: its affine fixes SteamVR's eyes as they read that day."""
    if fusion is None or one_eye is not None or not made or fusion.info.get("how") != "calibration":
        return False
    try:
        then = [time.mktime(time.strptime(m, "%Y-%m-%d %H:%M:%S")) for m in (fusion.info["made"], made)]
    except (KeyError, TypeError, ValueError):
        return False
    return abs(then[0] - then[1]) <= SAME_CALIBRATION


class SteamLooks:
    def __init__(self):
        self.looks = deque()   # (t, yaw, pitch)
        self.sent = -math.inf

    def reset(self):
        self.looks.clear()

    def step(self, t, left, right, one_eye, fusion, made):
        """A sample at t (CLOCK_MONOTONIC_RAW, ft-gaze's): SteamVR's left and right eye readings
        ((yaw, pitch) or None when lost), the eye it tracks alone (0 left, 1 right) or None, the
        fusion (gazecal.Fusion or None) and our calibration's made. A look to send, or None."""
        if one_eye is None:
            look =((left[0] + right[0]) / 2, (left[1] + right[1]) / 2) if left and right else None
        else:
            look = (left, right)[one_eye]
        if look is None:
            self.looks.clear()   # a blink or a lost eye: the look starts over
            return None
        looks = self.looks
        looks.append((t, *look))
        while looks and looks[0][0] < t - WINDOW:
            looks.popleft()
        if t - self.sent < EVERY or len(looks) < MIN_SAMPLES or t - looks[0][0] < 0.8 * WINDOW:
            return None
        m = (statistics.median(v[1] for v in looks), statistics.median(v[2] for v in looks))
        if any(math.hypot(y - m[0], p - m[1]) > STEADY_DEG for _, y, p in looks):
            return None
        limit = RAW_DEG
        if same_calibration(fusion, made, one_eye):
            fixed = fusion.steam_gaze(m, m, clicks=False)
            if fixed:
                m, limit = fixed, CENTRE_DEG
        if math.hypot(*m) > limit:
            return None
        self.sent = t
        return t, m[0], m[1]
