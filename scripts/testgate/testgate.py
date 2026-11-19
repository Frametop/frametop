#!/usr/bin/env python3
"""The Python test files as a strict gate (`just test-python`): runs each file the
patterns match with python3, several at once (--jobs N; scripts/testgate/jobs.py picks how many
otherwise), and says of each, in the order the patterns list them, whether it passed, was
skipped, or failed.

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
- Each file gets a temp folder of its own as TMPDIR. A file that can't share the machine with
  others (a fixed port, a fixed path outside TMPDIR) says `# testgate: alone` in its first
  lines, and runs with nothing else running.
- An argument @FILE reads patterns from FILE, one a line (# starts a comment). --times FILE
  writes how long each file took, as JSON; the slowest are named at the end either way.

  scripts/testgate/testgate.py [--jobs N] [--times FILE] 'conf/test/*.py' @scripts/testgate/python-suites.txt ...
"""
import concurrent.futures
import glob
import json
import os
import signal
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import procs  # noqa: E402
from jobs import jobs  # noqa: E402

HOOK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hook")
# The repo root, on each test's PYTHONPATH: hands/rec and gaze/rec are packages (hands.rec, gaze.rec), and
# their tests import by package.
TREE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SKIP = 77
TIMEOUT = procs.TIMEOUT    # seconds a file gets (procs.py)
timeout = procs.timeout
LIVE = set()    # the test files running now (their process group ids)


def run(path, python=sys.executable, limit=None, threads=None):
    """Run one test file: (status, how many tests ran, the skip reasons, its stdout, its stderr).
    status is "ok", "skipped" or "failed". threads: the math libraries' threads, when set."""
    limit = limit if limit is not None else procs.timeout()
    with tempfile.TemporaryDirectory(prefix="ft-testgate-") as tmp:
        report = os.path.join(tmp, "report.json")
        env = dict(os.environ, FT_TEST_REPORT=report, TMPDIR=tmp,
                   PYTHONPATH=os.pathsep.join(p for p in (HOOK, TREE, os.environ.get("PYTHONPATH")) if p))
        if threads:     # files run side by side: one math thread each, not one per CPU each
            for name in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
                env.setdefault(name, str(threads))
        # files, not pipes: a server a test starts and leaves running can hold a pipe open
        with open(os.path.join(tmp, "out"), "w+", errors="replace") as out, \
                open(os.path.join(tmp, "err"), "w+", errors="replace") as err:
            code = procs.run_session([python, path], LIVE, limit, stdout=out, stderr=err, env=env)
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


ALONE = "# testgate: alone"


def alone(path):
    """Whether the file says it has to run with nothing else running (in its first 20 lines)."""
    try:
        with open(path, errors="replace") as f:
            return any(line.strip() == ALONE for _, line in zip(range(20), f))
    except OSError:
        return False


def expand(args):
    """The patterns in args, an @FILE argument read as one pattern a line."""
    out = []
    for a in args:
        if a.startswith("@"):
            with open(a[1:]) as f:
                out += [line.strip() for line in f if line.strip() and not line.lstrip().startswith("#")]
        else:
            out.append(a)
    return out


def main(patterns, njobs=1, times=None):
    files = {"ok": 0, "skipped": 0, "failed": 0}
    tests = {"run": 0, "skipped": 0}
    took = {}
    status = 0
    paths = []
    for pattern in patterns:
        matched = sorted(glob.glob(pattern))
        if not matched:
            paths.append((pattern, None))
        paths += [(pattern, p) for p in matched]

    def timed(path):
        start = time.monotonic()
        result = run(path, threads=1 if njobs > 1 else None)
        took[path] = round(time.monotonic() - start, 2)
        return result

    def report(pattern, path, result):
        nonlocal status
        if path is None:
            print(f"{pattern}: FAILED, matches no file (a folder moved or renamed?)", flush=True)
            status = 1
            return
        how, ran, skipped, stdout, stderr = result
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

    # the files in order, side by side; one that runs alone waits for those before it, and the
    # ones after it wait for it. Each is reported in order, as soon as those before it are.
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=max(1, njobs))
    try:
        pending = []
        for pattern, path in paths:
            if path is not None and njobs > 1 and alone(path):
                for item in pending:
                    report(item[0], item[1], item[2].result() if item[2] else None)
                pending = []
                report(pattern, path, timed(path))
                continue
            pending.append((pattern, path, pool.submit(timed, path) if path else None))
        for item in pending:
            report(item[0], item[1], item[2].result() if item[2] else None)
    except BaseException:   # testgate stopped (Ctrl-C, SIGTERM): what runs goes, what waits doesn't start
        pool.shutdown(wait=False, cancel_futures=True)
        procs.stop(LIVE)
        raise
    pool.shutdown()
    print(f"python files: {files['ok']} ok, {files['skipped']} skipped, {files['failed']} failed; "
          f"unittest tests run: {tests['run']}, skips: {tests['skipped']}", flush=True)
    slow = sorted(took.items(), key=lambda kv: -kv[1])[:5]
    if slow:
        print(f"python files took (at {max(1, njobs)} at once); slowest: "
              + ", ".join(f"{p} {t:g} s" for p, t in slow), flush=True)
    if times:
        os.makedirs(os.path.dirname(os.path.abspath(times)), exist_ok=True)
        with open(times, "w") as f:
            json.dump(took, f, indent=1, sort_keys=True)
    return status


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(128 + signal.SIGTERM))   # so the file running ends too
    import argparse
    a = argparse.ArgumentParser(usage=__doc__.rstrip().rsplit("\n", 1)[-1].strip())
    a.add_argument("--jobs", "-j", type=int, default=0)
    a.add_argument("--times")
    a.add_argument("patterns", nargs="*")
    args = a.parse_args()
    if not args.patterns:
        sys.exit(__doc__)
    sys.exit(main(expand(args.patterns), args.jobs or jobs(), args.times))
