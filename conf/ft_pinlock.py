"""Read the opt-in, expiring Steam PIN access lease; never read any credential."""
import os
import re
import stat
import time
from pathlib import Path

import ft_conf

LEASE_MS = 750
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
    return boot >= 0 and mono >= 0 and 0 <= b < LEASE_MS and 0 <= m < LEASE_MS and abs(b - m) < 50


def allowed():
    if not enabled():
        return True
    try:
        fd = os.open(STATE, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            info = os.fstat(fd)
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
                    info.st_mode & 0o077 or not 0 < info.st_size < 160):
                return False
            text = os.read(fd, 160).decode("ascii")
        finally:
            os.close(fd)
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
