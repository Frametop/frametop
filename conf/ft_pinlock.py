"""Read the opt-in, expiring Steam PIN access lease; never read any credential.

Also the guard's private-file reads and writes, for the lease and its carry record.
"""
import os
import re
import stat
import tempfile
import time
from pathlib import Path

import ft_conf

LEASE_MS = 750
SKEW_MS = 50  # boottime may run this far from monotonic before it counts as a suspend
STATE = Path(f"/run/user/{os.getuid()}/frametop-pin-state")


def enabled():
    value = os.environ.get("FT_STEAM_PIN_LOCK")
    if value is None:
        value = ft_conf.read(str(Path.home() / ".config/frametop.conf")).get("STEAM_PIN_LOCK", "0")
    return value != "0"


def clocks():
    return (time.clock_gettime_ns(time.CLOCK_BOOTTIME) // 1_000_000,
            time.monotonic_ns() // 1_000_000)


def fresh(boot, mono, now_boot, now_mono):
    b, m = now_boot - boot, now_mono - mono
    return boot >= 0 and mono >= 0 and 0 <= b < LEASE_MS and 0 <= m < LEASE_MS and abs(b - m) < SKEW_MS


def read_private(path):
    """A short ASCII file of this user's that no one else can read or write, or None.
    Never follows a link or blocks on a FIFO."""
    try:
        fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            info = os.fstat(fd)
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
                    info.st_mode & 0o077 or not 0 < info.st_size < 160):
                return None
            return os.read(fd, 160).decode("ascii")
        finally:
            os.close(fd)
    except (OSError, UnicodeError):
        return None


def write_private(path, text):
    """Replace path atomically with a 0600 file holding text."""
    fd, temporary = tempfile.mkstemp(prefix=".frametop-pin-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as output:
            output.write(text)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def allowed():
    if not enabled():
        return True
    try:
        text = read_private(STATE)
        if text is None:
            return False
        match = re.fullmatch(r"FTPIN1 1 ([0-9]+) ([0-9]+) ([0-9]+)\n", text)
        if not match:
            return False
        boot, mono, pid = map(int, match.groups())
        if pid <= 1:
            return False
        os.kill(pid, 0)
        return fresh(boot, mono, *clocks())
    except (OSError, ValueError, UnicodeError):
        return False


if __name__ == "__main__":
    raise SystemExit(0 if allowed() else 1)
