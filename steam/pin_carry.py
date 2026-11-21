"""The Steam PIN guard's last proven unlock, kept so a restarted guard can resume it.

A guard that restarts (crash, update, SteamVR restart) or loses Steam for a moment
would otherwise keep Frametop hidden until Steam locks and unlocks again. While the
guard holds an unlock it saw Steam's own lock lead to, it keeps a record here: the
Steam UI context (performance.timeOrigin), the two clocks and the kernel's suspend
counts. A later guard may resume that unlock only in the same UI context, on the
same boot, with no suspend tried since the record and within CARRY_MS of it (see
matches). A Steam client restart or a cold boot starts a new UI context, so those
still need a lock seen first; a cold boot also starts with an empty /run/user. A
suspend shows as boottime running ahead of monotonic, and any suspend tried, even
one that failed, moves the counts, so PIN-on-wake keeps its own lock. A running
guard that stalls spends the record itself (pin_lock.watch).

Idea and first version: Firtina Ozbalikchi (PR 95, warm-start recovery).
"""
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "conf"))
import ft_pinlock  # noqa: E402

CARRY_MS = 60_000  # long enough for a SteamVR restart, which restarts the guard
WIRE = re.compile(r"FTCARRY1 ([0-9]+\.[0-9]+) ([0-9]+) ([0-9]+) ([0-9a-f-]{36}) ([0-9]+) ([0-9]+)\n")
SUSPEND_STATS = Path("/sys/power/suspend_stats")


def path():
    return ft_pinlock.STATE.with_name("frametop-pin-carry")


def boot_id():
    return Path("/proc/sys/kernel/random/boot_id").read_text().strip()


def suspends():
    """(succeeded, failed): how many suspends the kernel has tried this boot, or None."""
    try:
        return tuple(int((SUSPEND_STATS / name).read_text()) for name in ("success", "fail"))
    except (OSError, ValueError):
        return None


def save(generation, stamp, tried):
    """Atomic and private like the lease: the unlock held in generation at stamp, with
    tried the suspend counts read before stamp (none: no record, nothing to resume)."""
    if tried is None:
        return
    ft_pinlock.write_private(path(), f"FTCARRY1 {float(generation)!r} {stamp[0]} {stamp[1]} "
                                     f"{boot_id()} {tried[0]} {tried[1]}\n")


def clear():
    try:
        os.unlink(path())
    except FileNotFoundError:
        pass


def load():
    """(generation, boot, mono, boot_id, suspends) from a private record of this user's, or None."""
    match = WIRE.fullmatch(ft_pinlock.read_private(path()) or "")
    if not match:
        return None
    return float(match[1]), int(match[2]), int(match[3]), match[4], (int(match[5]), int(match[6]))


def matches(record, generation, now):
    """Whether record proves an unlock still holds in generation at now (boot, mono)."""
    if record is None:
        return False
    proved, boot, mono, booted, slept = record
    try:
        awake = booted == boot_id() and slept == suspends()
    except OSError:
        return False
    b, m = now[0] - boot, now[1] - mono
    # 0 <= b too: the skew test alone lets boottime run slightly backwards.
    return (awake and float(generation) == proved and
            0 <= m < CARRY_MS and 0 <= b and abs(b - m) < ft_pinlock.SKEW_MS)
