"""The Steam PIN guard's last proven unlock, kept so a restarted guard can resume it.

A guard that restarts (crash, update, SteamVR restart) or loses Steam for a moment
would otherwise keep Frametop hidden until Steam locks and unlocks again. While the
guard holds an unlock it saw Steam's own lock lead to, it keeps a record here: the
Steam UI context (performance.timeOrigin) and the two clocks. A later guard may
resume that unlock only in the same UI context, on the same boot, with no suspend
since the record and within CARRY_MS of it (matches). A Steam client restart or a
cold boot starts a new UI context, so those still need a lock seen first; a cold
boot also starts with an empty /run/user. A suspend shows as boottime running ahead
of monotonic, which ends the record for good, so PIN-on-wake keeps its own lock.

Idea and first version: Firtina Ozbalikchi (PR 95, warm-start recovery).
"""
import os
import re
import stat
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "conf"))
import ft_pinlock  # noqa: E402

CARRY_MS = 60_000  # long enough for a SteamVR restart, which restarts the guard
SUSPEND_MS = 50    # ft_pinlock.fresh's boottime/monotonic tolerance
WIRE = re.compile(r"FTCARRY1 ([0-9]+(?:\.[0-9]+)?(?:e\+[0-9]+)?) ([0-9]+) ([0-9]+) ([0-9a-f-]{36})\n")


def path():
    return ft_pinlock.STATE.with_name("frametop-pin-carry")


def boot_id():
    return Path("/proc/sys/kernel/random/boot_id").read_text().strip()


def save(generation, stamp):
    """Atomic and private like the lease: the unlock held in generation at stamp."""
    target = path()
    fd, temporary = tempfile.mkstemp(prefix=".frametop-pin-", dir=target.parent)
    try:
        with os.fdopen(fd, "w") as output:
            output.write(f"FTCARRY1 {float(generation)!r} {stamp[0]} {stamp[1]} {boot_id()}\n")
        os.replace(temporary, target)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def clear():
    try:
        os.unlink(path())
    except FileNotFoundError:
        pass


def load():
    """(generation, boot, mono, boot_id) from a private record of this user's, or None."""
    try:
        fd = os.open(path(), os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            info = os.fstat(fd)
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
                    info.st_mode & 0o077 or not 0 < info.st_size < 160):
                return None
            text = os.read(fd, 160).decode("ascii")
        finally:
            os.close(fd)
    except (OSError, UnicodeError):
        return None
    match = WIRE.fullmatch(text)
    if not match:
        return None
    return float(match[1]), int(match[2]), int(match[3]), match[4]


def matches(record, generation, now):
    """Whether record proves an unlock still holds in generation at now (boot, mono)."""
    if record is None:
        return False
    proved, boot, mono, booted = record
    try:
        same_boot = booted == boot_id()
    except OSError:
        return False
    b, m = now[0] - boot, now[1] - mono
    return (same_boot and float(generation) == proved and
            0 <= m < CARRY_MS and 0 <= b and abs(b - m) < SUSPEND_MS)
