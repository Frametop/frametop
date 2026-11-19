"""Running one test process (testgate.py's files, ctests.py's programs, ninja): the time it gets,
and ending what it leaves running. Each runs in a session of its own, and what it starts stays
in that session even in a process group of its own (ninja gives each command one), unless it
starts a session of its own too (setsid).
"""
import atexit
import os
import signal
import subprocess
import threading
import time

TIMEOUT = 900   # seconds a test gets: room for a slow one, an end for a hung one
FROM_ENV = object()     # a limit argument's default: timeout() (None means no limit)


def timeout():
    """TIMEOUT, or FT_TEST_TIMEOUT's seconds; None (no limit) for 0 or less."""
    try:
        limit = float(os.environ.get("FT_TEST_TIMEOUT") or TIMEOUT)
    except ValueError:
        return TIMEOUT
    return limit if limit > 0 else None


class Stopped(Exception):
    """The runner was stopped (Ctrl-C, SIGTERM): no test starts after that."""


class Live:
    """The sessions a runner has running. Once stop() has it, none starts: a test a worker was
    about to start when the runner was stopped doesn't start after the others were ended."""

    def __init__(self):
        self.lock = threading.Lock()
        self.sids = set()
        self.stopped = False


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


def _threads(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            return next((int(line.split()[1]) for line in f if line.startswith("Threads:")), 1)
    except (OSError, ValueError, IndexError):
        return 1


def _scan(sids):
    """{sid: [pid, ...]}: the live processes in each of sids, from one read of /proc. A zombie
    counts only while it has threads still running (its first thread ended before the rest):
    a dead one can't be signalled away, and the leaders are kept zombies on purpose (exited)."""
    out = {}
    for name in os.listdir("/proc"):
        if name.isdigit():
            try:
                with open(f"/proc/{name}/stat") as f:
                    fields = f.read().rsplit(")", 1)[1].split()
            except (OSError, IndexError):
                continue
            sid = int(fields[3])
            if sid in sids and (fields[0] != "Z" or _threads(name) > 1):
                out.setdefault(sid, []).append(int(name))
    return out


def members(sid):
    """The live processes in session sid (a zombie counted only while it has threads running)."""
    return _scan({sid}).get(sid, [])


def _signal(pids, sig):
    for pid in pids:
        try:
            os.kill(pid, sig)
        except OSError:
            pass


def end_sessions(sids):
    """SIGTERM what's left in the sessions, all at once, and SIGKILL what's still there a second
    later, again and again for up to a second (one forked while /proc was read isn't in that
    read)."""
    sids = set(sids)
    left = _scan(sids)
    if not left:
        return
    _signal([p for pids in left.values() for p in pids], signal.SIGTERM)
    deadline = time.monotonic() + 1.0
    while _scan(sids) and time.monotonic() < deadline:
        time.sleep(0.05)
    deadline = time.monotonic() + 1.0
    while (left := _scan(sids)) and time.monotonic() < deadline:
        _signal([p for pids in left.values() for p in pids], signal.SIGKILL)
        time.sleep(0.01)


def end_session(sid):
    end_sessions([sid])


def run_session(argv, live, limit, **popen):
    """Run argv in a session of its own and wait for it: its exit code, or None when it was still
    running after limit seconds (None: no limit) and was killed. What it leaves running is ended.
    live (a Live) holds its session id while it runs; if the wait is stopped (Ctrl-C, SIGTERM),
    the session is ended and the exception goes on. Stopped when live was stopped first."""
    p = None
    try:    # from before Popen: a Ctrl-C as it returns still ends the session it started
        with live.lock:
            if live.stopped:
                raise Stopped(argv[0])
            p = subprocess.Popen(argv, stdin=subprocess.DEVNULL, start_new_session=True, **popen)
            live.sids.add(p.pid)
        timed_out = not exited(p.pid, limit)
        if timed_out:
            _signal(members(p.pid), signal.SIGKILL)
        end_session(p.pid)
    except BaseException:
        if p is not None:
            end_session(p.pid)
            p.kill()
            p.wait()
        raise
    finally:
        if p is not None:
            with live.lock:
                live.sids.discard(p.pid)
    code = p.wait()
    return None if timed_out else code


def _ignore(*_):
    """Stands in for SIG_IGN (see on_stop)."""


def on_stop(handler):
    """handler for SIGTERM and SIGHUP (a closed terminal): either way, the cleanup runs.

    One the run was started ignoring (nohup) still doesn't end it, through _ignore rather than
    SIG_IGN, which every test it starts would inherit (a handler isn't inherited: they start with
    the default). SIG_IGN is back on the way out, as Python's own shutdown would reset _ignore
    to the default and leave a few milliseconds in which a hangup ends a run that passed."""
    for sig in (signal.SIGTERM, signal.SIGHUP):
        if signal.getsignal(sig) in (signal.SIG_IGN, _ignore):
            if signal.getsignal(sig) is signal.SIG_IGN:
                atexit.register(signal.signal, sig, signal.SIG_IGN)
            signal.signal(sig, _ignore)
        else:
            signal.signal(sig, handler)


def stop(live):
    """End every session run_session has running (a runner that was stopped), and start none."""
    with live.lock:
        live.stopped = True
        sids = list(live.sids)
    end_sessions(sids)
