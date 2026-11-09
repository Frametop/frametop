"""Passive Steam PIN observer: diagnostic or opt-in access leases with --watch.

Accept an inactive lock only in a fresh, well-formed snapshot with loaded PIN
settings and no reset or inconsistent mandatory prompt. Do not require an earlier
lock cycle. Cold-boot initialization ordering still needs hardware validation.
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
            # Deny after a scheduling gap/suspend, then require a fresh Steam query.
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
                permit = state == "unlocked"
                publish(permit, stamp)
                reason = observer.reason
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
    """Classify explicit current Steam state; never reuse a previous unlock."""

    def __init__(self):
        self.invalidate()

    def invalidate(self, reason="awaiting-steam-state"):
        self.reason = reason

    def sample(self, snapshot):
        if not valid_snapshot(snapshot):
            self.invalidate("invalid-steam-snapshot")
            return "unknown"
        if snapshot["resetting"]:
            self.reason = "pin-reset-in-progress"
            return "unknown"
        if snapshot["mandatory"] and not snapshot["active"]:
            self.invalidate("inconsistent-lock-flags")
            return "unknown"
        if snapshot["active"]:
            self.reason = "steam-locked" if snapshot["mandatory"] else "cancellable-steam-prompt"
            return "locked"
        self.reason = "steam-unlocked" if snapshot["configured"] else "steam-pin-disabled"
        return "unlocked"


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
