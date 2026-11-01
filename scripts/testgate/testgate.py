#!/usr/bin/env python3
"""The Python test files as a strict gate (`just test-python`): runs each file the
patterns match with python3, one at a time, and says of each whether it passed, was skipped, or
failed.

- A pattern that matches no file fails the gate: a moved or renamed folder doesn't quietly drop
  its tests.
- A skip is never "ok". A file whose unittest tests were all skipped is SKIPPED, with the
  reasons; one with some skipped says how many, and why. Exit status 77 is how a test file
  that isn't unittest says it skipped itself (its last stdout line is the reason), and a
  "skip  REASON" line on its stdout is a part it skipped.
- A file that defines unittest tests but runs none (no unittest.main()) fails: it would pass
  having tested nothing.
- A file gets TIMEOUT seconds (FT_TEST_TIMEOUT overrides; 0 is no limit); one still running
  then is killed and fails, so a hung test can't hang the gate. Each file runs in a session of
  its own, and what is left in its process group when it ends (or testgate is stopped) is
  ended too: SIGTERM, then SIGKILL a second later. A program a test starts in a session of its
  own (setsid) isn't in that group and isn't ended.
- A failing file's output is shown; a passing one's isn't.

  scripts/testgate/testgate.py 'conf/test/*.py' 'layout/tests/test_*.py' ...
"""
import glob
import json
import os
import signal
import subprocess
import sys
import tempfile
import time

HOOK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hook")
# The repo root, on each test's PYTHONPATH: hands/rec and gaze/rec are packages (hands.rec, gaze.rec), and
# their tests import by package.
TREE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SKIP = 77
TIMEOUT = 900   # seconds a file gets: room for a slow file, an end for a hung one


def timeout():
    """TIMEOUT, or FT_TEST_TIMEOUT's seconds; None (no limit) for 0 or less."""
    try:
        limit = float(os.environ.get("FT_TEST_TIMEOUT") or TIMEOUT)
    except ValueError:
        return TIMEOUT
    return limit if limit > 0 else None


def _exited(pid, limit):
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


def _members(pgid):
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


def _end_group(pgid):
    """SIGTERM what's left in the group, and SIGKILL what's still there a second later."""
    if not _members(pgid):
        return
    for sig, grace in ((signal.SIGTERM, 1.0), (signal.SIGKILL, 0)):
        try:
            os.killpg(pgid, sig)
        except OSError:
            return
        deadline = time.monotonic() + grace
        while _members(pgid) and time.monotonic() < deadline:
            time.sleep(0.05)
        if not _members(pgid):
            return


def run(path, python=sys.executable, limit=None):
    """Run one test file: (status, how many tests ran, the skip reasons, its stdout, its stderr).
    status is "ok", "skipped" or "failed"."""
    limit = limit if limit is not None else timeout()
    with tempfile.TemporaryDirectory(prefix="ft-testgate-") as tmp:
        report = os.path.join(tmp, "report.json")
        env = dict(os.environ, FT_TEST_REPORT=report,
                   PYTHONPATH=os.pathsep.join(p for p in (HOOK, TREE, os.environ.get("PYTHONPATH")) if p))
        # files, not pipes: a server a test starts and leaves running can hold a pipe open
        with open(os.path.join(tmp, "out"), "w+", errors="replace") as out, \
                open(os.path.join(tmp, "err"), "w+", errors="replace") as err:
            p = subprocess.Popen([python, path], stdin=subprocess.DEVNULL, stdout=out, stderr=err, env=env,
                                 start_new_session=True)
            try:
                timed_out = not _exited(p.pid, limit)
                if timed_out:
                    os.killpg(p.pid, signal.SIGKILL)
                _end_group(p.pid)   # what it started and left running goes too
            except BaseException:   # testgate stopped (Ctrl-C, SIGTERM): the file goes with it
                _end_group(p.pid)
                p.kill()
                p.wait()
                raise
            code = None if timed_out else p.wait()
            out.seek(0)
            err.seek(0)
            stdout, stderr = out.read(), err.read()
        seen = {"run": 0, "skipped": []}
        if os.path.exists(report):
            with open(report) as f:
                seen = json.load(f)
    lines = stdout.splitlines()
    skipped = seen["skipped"] + [line.split(None, 1)[1] for line in lines
                                 if line.startswith("skip ") and len(line.split(None, 1)) > 1]
    if code is None:
        return "failed", seen["run"], skipped, stdout, stderr + f"\ntestgate: still running after {limit:g} s, killed\n"
    if code == SKIP:
        return "skipped", seen["run"], skipped or [lines[-1] if lines else "(no reason given)"], stdout, stderr
    if code != 0:
        return "failed", seen["run"], skipped, stdout, stderr
    if seen["skipped"] and len(set(seen.get("skipped_tests", []))) >= seen["run"]:
        return "skipped", seen["run"], skipped, stdout, stderr
    if seen.get("defined") and not seen["run"] and not seen["skipped"]:
        return "failed", 0, skipped, stdout, stderr + (f"\ntestgate: defines {seen['defined']} unittest test(s) "
                                                       "but ran none (no unittest.main()?)\n")
    return "ok", seen["run"], skipped, stdout, stderr


def reasons(skipped):
    """The skip reasons, each once, with how often it was given."""
    counts = {}
    for r in skipped:
        counts[r] = counts.get(r, 0) + 1
    return "; ".join(r if n == 1 else f"{r} (x{n})" for r, n in counts.items())


def tail(text, n=60):
    lines = text.rstrip("\n").splitlines()
    return "\n".join((["..."] if len(lines) > n else []) + lines[-n:])


def main(patterns):
    files = {"ok": 0, "skipped": 0, "failed": 0}
    tests = {"run": 0, "skipped": 0}
    status = 0
    for pattern in patterns:
        matched = sorted(glob.glob(pattern))
        if not matched:
            print(f"{pattern}: FAILED, matches no file (a folder moved or renamed?)", flush=True)
            status = 1
            continue
        for path in matched:
            how, ran, skipped, stdout, stderr = run(path)
            files[how] += 1
            tests["run"] += ran
            tests["skipped"] += len(skipped)
            if how == "failed":
                status = 1
                print(f"{path}: FAILED", flush=True)
                for name, text in (("stdout", stdout), ("stderr", stderr)):
                    if text.strip():
                        print(f"--- {path} {name}:\n{tail(text)}", flush=True)
            elif how == "skipped":
                print(f"{path}: SKIPPED: {reasons(skipped)}", flush=True)
            elif skipped:
                print(f"{path}: ok, {len(skipped)} skipped: {reasons(skipped)}", flush=True)
            else:
                print(f"{path}: ok", flush=True)
    print(f"python files: {files['ok']} ok, {files['skipped']} skipped, {files['failed']} failed; "
          f"unittest tests run: {tests['run']}, skips: {tests['skipped']}", flush=True)
    return status


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(128 + signal.SIGTERM))   # so the file running ends too
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1:]))
