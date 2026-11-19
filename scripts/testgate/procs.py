"""Running one test process (testgate.py's files, ctests.py's programs, ninja): the time it gets,
and ending what it leaves running. Each runs in a session of its own, and what it starts stays
in that session even in a process group of its own (ninja gives each command one), unless it
starts a session of its own too (setsid).
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
    session id stays its own while the session is ended. True when it exited."""
    deadline = None if limit is None else time.monotonic() + limit
    while True:
        r = os.waitid(os.P_PID, pid, os.WEXITED | os.WNOWAIT | os.WNOHANG)
        if r is not None and r.si_pid == pid:
            return True
        if deadline is not None and time.monotonic() >= deadline:
            return False
        time.sleep(0.05)


def members(sid):
    """The live processes in session sid (the zombie leader not counted)."""
    out = []
    for name in os.listdir("/proc"):
        if name.isdigit():
            try:
                with open(f"/proc/{name}/stat") as f:
                    fields = f.read().rsplit(")", 1)[1].split()
            except (OSError, IndexError):
                continue
            if fields[0] != "Z" and int(fields[3]) == sid:
                out.append(int(name))
    return out


def _signal(pids, sig):
    for pid in pids:
        try:
            os.kill(pid, sig)
        except OSError:
            pass


def end_session(sid):
    """SIGTERM what's left in the session, and SIGKILL what's still there a second later, again
    and again for up to a second (one forked while /proc was read isn't in that read)."""
    left = members(sid)
    if not left:
        return
    _signal(left, signal.SIGTERM)
    deadline = time.monotonic() + 1.0
    while members(sid) and time.monotonic() < deadline:
        time.sleep(0.05)
    deadline = time.monotonic() + 1.0
    while (left := members(sid)) and time.monotonic() < deadline:
        _signal(left, signal.SIGKILL)
        time.sleep(0.01)


def run_session(argv, live, limit, **popen):
    """Run argv in a session of its own and wait for it: its exit code, or None when it was still
    running after limit seconds (None: no limit) and was killed. What it leaves running is ended.
    live (a set) holds its session id while it runs; if the wait is stopped (Ctrl-C, SIGTERM),
    the session is ended and the exception goes on."""
    p = subprocess.Popen(argv, stdin=subprocess.DEVNULL, start_new_session=True, **popen)
    live.add(p.pid)
    try:
        timed_out = not exited(p.pid, limit)
        if timed_out:
            _signal(members(p.pid), signal.SIGKILL)
        end_session(p.pid)
    except BaseException:
        end_session(p.pid)
        p.kill()
        p.wait()
        raise
    finally:
        live.discard(p.pid)
    code = p.wait()
    return None if timed_out else code


def on_stop(handler):
    """handler for SIGTERM and SIGHUP (a closed terminal): either way, the cleanup runs. One the
    run was started ignoring (nohup) still doesn't end it, but through a handler that does
    nothing rather than SIG_IGN, which every test it starts would inherit (a handler isn't
    inherited: they start with the default)."""
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, handler if signal.getsignal(sig) is not signal.SIG_IGN else (lambda *_: None))


def stop(live):
    """End every session run_session has running (a runner that was stopped)."""
    for sid in list(live):
        end_session(sid)
