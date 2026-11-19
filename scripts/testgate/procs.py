"""Running one test process (testgate.py's files, ctests.py's programs): the time it gets, and
ending what it leaves running. Each runs in a session of its own, so what it starts is in its
process group, unless that went to a session of its own too (setsid).
"""
import os
import signal
import subprocess
import time

TIMEOUT = 900   # seconds a test gets: room for a slow one, an end for a hung one


def timeout():
    """TIMEOUT, or FT_TEST_TIMEOUT's seconds; None (no limit) for 0 or less."""
    try:
        limit = float(os.environ.get("FT_TEST_TIMEOUT") or TIMEOUT)
    except ValueError:
        return TIMEOUT
    return limit if limit > 0 else None


def exited(pid, limit):
    """Wait up to limit seconds (None: no limit) for pid to exit, without reaping it, so its
    process group id stays its own while the group is ended. True when it exited."""
    deadline = None if limit is None else time.monotonic() + limit
    while True:
        r = os.waitid(os.P_PID, pid, os.WEXITED | os.WNOWAIT | os.WNOHANG)
        if r is not None and r.si_pid == pid:
            return True
        if deadline is not None and time.monotonic() >= deadline:
            return False
        time.sleep(0.05)


def members(pgid):
    """The live processes in process group pgid (the zombie leader not counted)."""
    out = []
    for name in os.listdir("/proc"):
        if name.isdigit():
            try:
                with open(f"/proc/{name}/stat") as f:
                    fields = f.read().rsplit(")", 1)[1].split()
            except (OSError, IndexError):
                continue
            if fields[0] != "Z" and int(fields[2]) == pgid:
                out.append(int(name))
    return out


def end_group(pgid):
    """SIGTERM what's left in the group, and SIGKILL what's still there a second later."""
    if not members(pgid):
        return
    for sig, grace in ((signal.SIGTERM, 1.0), (signal.SIGKILL, 0)):
        try:
            os.killpg(pgid, sig)
        except OSError:
            return
        deadline = time.monotonic() + grace
        while members(pgid) and time.monotonic() < deadline:
            time.sleep(0.05)
        if not members(pgid):
            return


def run_session(argv, live, limit, **popen):
    """Run argv in a session of its own and wait for it: its exit code, or None when it was still
    running after limit seconds (None: no limit) and was killed. What it leaves running is ended.
    live (a set) holds its process group id while it runs; if the wait is stopped (Ctrl-C,
    SIGTERM), the group is ended and the exception goes on."""
    p = subprocess.Popen(argv, stdin=subprocess.DEVNULL, start_new_session=True, **popen)
    live.add(p.pid)
    try:
        timed_out = not exited(p.pid, limit)
        if timed_out:
            os.killpg(p.pid, signal.SIGKILL)
        end_group(p.pid)
    except BaseException:
        end_group(p.pid)
        p.kill()
        p.wait()
        raise
    finally:
        live.discard(p.pid)
    code = p.wait()
    return None if timed_out else code


def stop(live):
    """End every group run_session has running (a runner that was stopped)."""
    for pgid in list(live):
        end_group(pgid)
