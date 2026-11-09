"""Passive Steam PIN observer: diagnostic or opt-in access leases with --watch.

Steam's UI starts with no active lock props before initialization. A false
IsLockScreenActive() alone is NOT proof of unlock. Require a non-cancellable
lock in the same live UI context before accepting its subsequent dismissal.
Never fetch the settings object over CDP: it can contain the user's PIN.
Steam alone controls locking and authentication; this module never requests either.
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
                observer.invalidate("observation-gap-or-suspend")
                publish(False, stamp)
            last = stamp
            try:
                snap = steamui.evaluate(SNAPSHOT)
                state = observer.sample(snap)
                if not ft_pinlock.fresh(*stamp, *ft_pinlock.clocks()):
                    observer.invalidate("stale-steam-response")
                    state = "unknown"
                # Initialized, explicitly disabled Steam PIN is not an unlock inferred
                # from UI absence. Protect active/reset prompts even in that case.
                no_pin = (valid_snapshot(snap) and
                          snap["configured"] is False and snap["active"] is False and
                          snap["mandatory"] is False and snap["resetting"] is False and
                          ft_pinlock.fresh(*stamp, *ft_pinlock.clocks()))
                permit = state == "unlocked-after-observed-lock" or no_pin
                publish(permit, stamp)
                reason = "steam-pin-disabled" if no_pin else observer.reason
            except (steamui.SteamUnreachable, OSError, ValueError, KeyError, TypeError):
                observer.invalidate("steam-query-failed")
                state = "unknown"
                reason = observer.reason
                publish(False, ft_pinlock.clocks())
            # Fixed reason codes only: never log raw Steam data or exception text.
            status = (state, reason)
            if status != previous:
                print(f"Steam PIN gate: {state} ({reason})", flush=True)
                previous = status
            time.sleep(0.15)
    finally:
        publish(False, ft_pinlock.clocks())
        os.close(fd)


class Observer:
    """A conservative diagnostic latch; connection loss invalidates prior proof."""

    def __init__(self):
        self.invalidate()

    def invalidate(self, reason="awaiting-observed-lock"):
        self.generation = None
        self.saw_mandatory_lock = False
        self.reason = reason

    def sample(self, snapshot):
        if not valid_snapshot(snapshot):
            self.invalidate("invalid-steam-snapshot")
            return "unknown"
        if snapshot["generation"] != self.generation:
            if self.generation is not None:
                self.invalidate("steam-ui-generation-changed")
            self.generation = snapshot["generation"]
        if snapshot["resetting"] or not snapshot["configured"]:
            self.saw_mandatory_lock = False
            self.reason = "pin-reset-or-configuration-change"
            return "unknown"
        if snapshot["mandatory"] and not snapshot["active"]:
            self.invalidate("inconsistent-lock-flags")
            return "unknown"
        if snapshot["active"]:
            self.saw_mandatory_lock |= snapshot["mandatory"]
            self.reason = "steam-locked" if self.saw_mandatory_lock else "cancellable-steam-prompt"
            return "locked"
        if self.saw_mandatory_lock:
            self.reason = "observed-lock-dismissed"
            return "unlocked-after-observed-lock"
        return "unknown"


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
