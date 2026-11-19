#!/usr/bin/env python3
"""`just test-changed [BASE]`: the tests a change since the last green `just test` reaches, for the
loop while working; `just test` is still the gate before a push.

`just test` writes the commit it passed on to build/tests/last-green (`--mark-green`). This
compares the tree as it is now (commits, edits, new files) with that commit, or with BASE, and
runs:

- lint and the shell scripts' syntax check, always (seconds);
- the Python test files (scripts/testgate/python-suites.txt) in a folder that changed, or whose
  text names a changed file (its name without the extension, as a word); all of them when a
  file everything reads changed (FULL_PYTHON);
- the C/C++ tests the change reaches (scripts/testgate/ctests.py --changed);
- Full Keyboard's ctest, the testbench's tests, fast-install's test and session/tests when
  their folders changed.

A change to what decides what runs (FULL) runs `just test` itself. With no green run recorded,
or no git, it says so and runs `just test`.
"""
import glob
import os
import re
import subprocess
import sys

TREE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STAMP = "build/tests/last-green"
SUITES = "scripts/testgate/python-suites.txt"
# a change here can change what any test does or which run: everything runs
FULL = ("justfile", "pyproject.toml", "uv.lock", "setup/packages.txt", "pack/", "scripts/testgate/",
        "scripts/_env.sh")
# read by nearly every Python program: every Python test file runs
FULL_PYTHON = ("conf/",)
STEM_MIN = 4    # shorter names ("run", "app") are in every file


def _under(path, roots):
    return any(path == r.rstrip("/") or path.startswith(r.rstrip("/") + "/") for r in roots)


def git(*args):
    # `podman exec dev` runs as the box's root, and git won't read a tree root doesn't own
    # ("dubious ownership") unless told this one is safe
    p = subprocess.run(["git", "-c", f"safe.directory={TREE}", *args], cwd=TREE, capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {p.stderr.strip()}")
    return p.stdout


def changed_since(base):
    """Repo paths that differ from base: committed since, edited, deleted or new (untracked)."""
    paths = set(git("diff", "--name-only", base, "--").split())
    paths |= set(git("ls-files", "--others", "--exclude-standard").split())
    return sorted(paths)


def python_files(root=TREE):
    """The test files python-suites.txt's patterns match, in its order."""
    out = []
    with open(os.path.join(root, SUITES)) as f:
        for line in f:
            pattern = line.strip()
            if pattern and not pattern.startswith("#"):
                out += [os.path.relpath(p, root) for p in sorted(glob.glob(os.path.join(root, pattern)))]
    return out


def pick_python(files, changed, root=TREE):
    """The test files of files a change reaches: same top folder, or a changed file's name in it."""
    if any(_under(c, FULL_PYTHON) for c in changed):
        return list(files)
    tops = {c.split("/", 1)[0] for c in changed if "/" in c}
    stems = {os.path.splitext(os.path.basename(c))[0] for c in changed}
    words = re.compile(r"\b(" + "|".join(sorted(map(re.escape, (s for s in stems if len(s) >= STEM_MIN)))) + r")\b") \
        if any(len(s) >= STEM_MIN for s in stems) else None
    picked = []
    for f in files:
        if f.split("/", 1)[0] in tops or f in changed:
            picked.append(f)
            continue
        if words:
            try:
                with open(os.path.join(root, f), errors="replace") as fh:
                    if words.search(fh.read()):
                        picked.append(f)
            except OSError:
                pass
    return picked


def plan(changed, root=TREE):
    """[(what, argv, stdin)]: the runs the change calls for, or None for `just test`."""
    if any(_under(c, FULL) for c in changed):
        return None
    py = pick_python(python_files(root), changed, root)
    runs = [("lint", ["just", "lint"], None), ("shell scripts", ["just", "test-bash"], None)]
    if py:
        runs += [("countdown headers", ["layout/countdown/deps.sh"], None),
                 (f"{len(py)} Python test files", ["python3", "scripts/testgate/testgate.py", "--times",
                                                   "build/tests/times-python.json", *py], None)]
    if any(_under(c, ("session",)) for c in changed):
        runs.append(("session/tests", ["pytest", "-q", "session/tests"], None))
    runs.append(("C/C++ tests", ["python3", "scripts/testgate/ctests.py", "--changed", "-"], "\n".join(changed)))
    for roots, recipe in ((("keyboard", "scripts/openvr.sh"), "test-keyboard"), (("testbench",), "test-testbench"),
                          (("scripts/fast-install", "scripts/tests/test_fast_install.py"), "test-fast-install")):
        if any(_under(c, roots) for c in changed):
            runs.append((recipe, ["just", recipe], None))
    return runs


def mark_green():
    try:
        head = git("rev-parse", "HEAD").strip()
    except (RuntimeError, OSError):
        print("last green: not recorded (no git here); `just test-changed` will run everything")
        return 0
    os.makedirs(os.path.join(TREE, os.path.dirname(STAMP)), exist_ok=True)
    with open(os.path.join(TREE, STAMP), "w") as f:
        f.write(head + "\n")
    print(f"last green: {head[:12]} (build/tests/last-green; `just test-changed` starts from it)")
    return 0


def full(why):
    print(f"test-changed: {why}: running `just test`", flush=True)
    return subprocess.run(["just", "test"], cwd=TREE).returncode


def main(argv):
    if argv[:1] == ["--mark-green"]:
        return mark_green()
    base = argv[0] if argv else None
    if base is None:
        try:
            with open(os.path.join(TREE, STAMP)) as f:
                base = f.read().strip()
        except OSError:
            return full("no green `just test` recorded yet")
    try:
        changed = changed_since(base)
    except (RuntimeError, OSError) as e:
        return full(str(e))
    if not changed:
        print(f"test-changed: nothing changed since {base[:12]}")
        return 0
    runs = plan(changed)
    if runs is None:
        return full("a change to what decides the tests (" + ", ".join(c for c in changed if _under(c, FULL)) + ")")
    print(f"test-changed: {len(changed)} paths changed since {base[:12]}: " + ", ".join(w for w, _, _ in runs),
          flush=True)
    failed = []
    for what, cmd, stdin in runs:
        print(f"--- {what}", flush=True)
        if subprocess.run(cmd, cwd=TREE, input=stdin, text=True).returncode != 0:
            failed.append(what)
    print("test-changed: " + (f"FAILED: {', '.join(failed)}" if failed else "ok")
          + " (a partial run: `just test` is the gate)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
