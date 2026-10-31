#!/usr/bin/env python3
"""The Python test files as a strict gate (`just test-python`, DEE-302): runs each file the
patterns match with python3, one at a time, and says of each whether it passed, was skipped, or
failed.

- A pattern that matches no file fails the gate: a moved or renamed folder doesn't quietly drop
  its tests.
- A skip is never "ok". A file whose unittest tests were all skipped is SKIPPED, with the
  reasons; one with some skipped says how many, and why. Exit status 77 is how a test file
  that isn't unittest says it skipped itself (its last stdout line is the reason), and a
  "skip  REASON" line on its stdout is a part it skipped.
- A failing file's output is shown; a passing one's isn't.

  scripts/testgate/testgate.py 'conf/test/*.py' 'layout/tests/test_*.py' ...
"""
import glob
import json
import os
import subprocess
import sys
import tempfile

HOOK = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hook")
SKIP = 77


def run(path, python=sys.executable):
    """Run one test file: (status, how many tests ran, the skip reasons, its stdout, its stderr).
    status is "ok", "skipped" or "failed"."""
    with tempfile.TemporaryDirectory(prefix="ft-testgate-") as tmp:
        report = os.path.join(tmp, "report.json")
        env = dict(os.environ, FT_TEST_REPORT=report,
                   PYTHONPATH=os.pathsep.join(p for p in (HOOK, os.environ.get("PYTHONPATH")) if p))
        # files, not pipes: a server a test starts and leaves running can hold a pipe open
        with open(os.path.join(tmp, "out"), "w+", errors="replace") as out, \
                open(os.path.join(tmp, "err"), "w+", errors="replace") as err:
            code = subprocess.call([python, path], stdin=subprocess.DEVNULL, stdout=out, stderr=err, env=env)
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
    if code == SKIP:
        return "skipped", seen["run"], skipped or [lines[-1] if lines else "(no reason given)"], stdout, stderr
    if code != 0:
        return "failed", seen["run"], skipped, stdout, stderr
    if seen["skipped"] and len(set(seen.get("skipped_tests", []))) >= seen["run"]:
        return "skipped", seen["run"], skipped, stdout, stderr
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
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1:]))
