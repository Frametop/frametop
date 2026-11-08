"""Steam PIN observer: read-only probe by default, opt-in access guard with --watch.

Steam's UI starts with no active lock props before initialization. A false
IsLockScreenActive() alone is NOT proof of unlock. Require a non-cancellable
lock in the same live UI context before accepting its subsequent dismissal.
Never fetch the settings object over CDP: it can contain the user's PIN.
"""
import argparse
import json
import math
import fcntl
import os
import signal
import sys
import tempfile
import time
from pathlib import Path

import steamui

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "conf"))
import ft_pinlock  # noqa: E402


# Return only explicit booleans and a UI-context generation. Do not return settings,
# active props, account names, callbacks, PIN values, or authentication tickets.
SNAPSHOT = """(() => {
    const s = window.securitystore;
    if (!s || typeof s.GetSettings !== 'function' ||
        typeof s.GetActiveLockScreenProps !== 'function' ||
        typeof s.IsLockScreenActive !== 'function' ||
        typeof s.BResettingPIN !== 'function') return null;
    const settings = s.GetSettings();
    const props = s.GetActiveLockScreenProps();
    if (!settings || props === undefined ||
        typeof settings.bLockOnWake !== 'boolean' ||
        typeof settings.bLockDesktopMode !== 'boolean') return null;
    return {
        generation: performance.timeOrigin,
        active: s.IsLockScreenActive(),
        mandatory: props !== null && props.preventCancel === true,
        resetting: s.BResettingPIN(),
        configured: settings.bLockOnWake || settings.bLockDesktopMode
    };
})()"""

# This invokes Steam's own non-cancellable PIN UI, exactly as its wake path does.
# It never submits a PIN or changes settings; do not replace an existing prompt.
REQUEST_LOCK = """(() => {
    const s = window.securitystore;
    if (!s || typeof s.SetActiveLockScreenProps !== 'function' ||
        typeof s.IsLockScreenActive !== 'function') return false;
    const settings = s.GetSettings();
    if (!settings || !(settings.bLockOnWake || settings.bLockDesktopMode)) return false;
    if (!s.IsLockScreenActive())
        s.SetActiveLockScreenProps({preventCancel:true, preventSteamButtons:true});
    return true;
})()"""


def publish(permit, stamp):
    """Atomic, private, boot-local state; request time, not response time, dates proof."""
    path = ft_pinlock.STATE
    fd, temporary = tempfile.mkstemp(prefix=".frametop-pin-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as output:
            output.write(f"FTPIN1 {int(permit)} {stamp[0]} {stamp[1]} {os.getpid()}\n")
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def valid_snapshot(snapshot):
    fields = {"generation", "active", "mandatory", "resetting", "configured"}
    return (isinstance(snapshot, dict) and set(snapshot) == fields and
            type(snapshot["generation"]) in (int, float) and
            math.isfinite(snapshot["generation"]) and snapshot["generation"] > 0 and
            all(type(snapshot[k]) is bool for k in fields - {"generation"}))


def watch():
    if not ft_pinlock.enabled():
        return
    # Single publisher; do not overwrite the state if another guard owns it.
    fd = os.open(ft_pinlock.STATE.with_suffix(".lock"), os.O_CREAT | os.O_RDWR | os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(fd)
        return
    observer = Observer()
    previous = None
    last = ft_pinlock.clocks()
    publish(False, last)
    def stop(_sig, _frame):
        raise SystemExit
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        while True:
            stamp = ft_pinlock.clocks()
            # Any scheduling gap or suspend discards proof before querying Steam.
            if not ft_pinlock.fresh(*last, *stamp):
                observer.invalidate()
                publish(False, stamp)
            last = stamp
            try:
                snap = steamui.evaluate(SNAPSHOT)
                state = observer.sample(snap)
                if not ft_pinlock.fresh(*stamp, *ft_pinlock.clocks()):
                    observer.invalidate()
                    state = "unknown"
                # Initialized, explicitly disabled Steam PIN is not an unlock inferred
                # from UI absence. Protect active/reset prompts even in that case.
                no_pin = (valid_snapshot(snap) and
                          snap["configured"] is False and snap["active"] is False and
                          snap["mandatory"] is False and snap["resetting"] is False and
                          ft_pinlock.fresh(*stamp, *ft_pinlock.clocks()))
                permit = state == "unlocked-after-observed-lock" or no_pin
                publish(permit, stamp)
                if state == "unknown" and not no_pin and isinstance(snap, dict) and snap.get("configured") is True:
                    # A restart in an unlocked session has no proof: request real Steam
                    # authentication instead of opening access on a guessed false state.
                    steamui.evaluate(REQUEST_LOCK)
            except (steamui.SteamUnreachable, OSError, ValueError, KeyError, TypeError):
                observer.invalidate()
                state = "unknown"
                publish(False, ft_pinlock.clocks())
            if state != previous:
                print(f"Steam PIN gate: {state}", flush=True)
                previous = state
            time.sleep(0.15)
    finally:
        publish(False, ft_pinlock.clocks())
        os.close(fd)


class Observer:
    """A conservative diagnostic latch; connection loss invalidates prior proof."""

    def __init__(self):
        self.invalidate()

    def invalidate(self):
        self.generation = None
        self.saw_mandatory_lock = False

    def sample(self, snapshot):
        if not valid_snapshot(snapshot):
            self.invalidate()
            return "unknown"
        if snapshot["generation"] != self.generation:
            self.invalidate()
            self.generation = snapshot["generation"]
        if snapshot["resetting"] or not snapshot["configured"]:
            self.saw_mandatory_lock = False
            return "unknown"
        if snapshot["mandatory"] and not snapshot["active"]:
            self.invalidate()
            return "unknown"
        if snapshot["active"]:
            self.saw_mandatory_lock |= snapshot["mandatory"]
            return "locked"
        return "unlocked-after-observed-lock" if self.saw_mandatory_lock else "unknown"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=180, choices=range(1, 601),
                        metavar="1..600", help="bounded probe duration (default: 180)")
    parser.add_argument("--watch", action="store_true", help="publish the opt-in expiring access lease")
    args = parser.parse_args()
    if args.watch:
        watch()
        return
    observer = Observer()
    previous = None
    until = time.monotonic() + args.seconds
    while time.monotonic() < until:
        try:
            status = observer.sample(steamui.evaluate(SNAPSHOT))
        except (steamui.SteamUnreachable, ValueError, KeyError, TypeError):
            observer.invalidate()
            status = "unknown"
        if status != previous:
            print(json.dumps({"state": status}), flush=True)
            previous = status
        time.sleep(0.2)


if __name__ == "__main__":
    main()
