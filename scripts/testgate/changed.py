#!/usr/bin/env python3
"""`just test-changed [BASE]`: the tests a change since the last green `just test` reaches, for the
loop while working; `just test` is still the gate before a push.

A passing `just test` writes the commit it started on to build/tests/last-green
(`--mark-start RUN`, then `--mark-green RUN`), with the paths that were edited and not committed
then: they count as changed every time, since what was tested isn't in git. This compares the
tree as it is now (commits, edits, new files) with that commit, or with BASE, and runs:

- lint, the shell scripts' syntax check and the tests that read the whole repo (ALWAYS: docs'
  links), always (seconds);
- the Python test files (scripts/testgate/python-suites.txt) in a folder that changed, or whose
  text names a changed file (its name, with or without the extension, as a word); the apps'
  import check when a Python file changed; all of them when a file everything reads changed
  (FULL_PYTHON);
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
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cbuild import SEEN, under  # noqa: E402

TREE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STAMP = "build/tests/last-green"
PENDING = STAMP + ".pending-"  # + a run's id: the tree a `just test` still running started on
PENDING_DAYS = 1   # a failed run's pending file goes after this
SUITES = "scripts/testgate/python-suites.txt"
# a change here can change what any test does or which run: everything runs
FULL = ("justfile", "pyproject.toml", "uv.lock", "setup/packages.txt", "pack/", "scripts/testgate/",
        "scripts/_env.sh")
# read by nearly every Python program: every Python test file runs
FULL_PYTHON = ("conf/",)
STEM_MIN = 4    # shorter names ("run", "app") are in every file: "run.sh" is looked for instead
# read every doc or every file in the repo: a rename anywhere can break them
ALWAYS = ("docs/tests/test_doc_links.py",)
# imports every app: a change to any Python module can break it
ON_PYTHON = ("scripts/tests/test_app_imports.py",)


def git(*args):
    # `podman exec dev` runs as the box's root, and git won't read a tree root doesn't own
    # ("dubious ownership") unless told this one is safe
    p = subprocess.run(["git", "-c", f"safe.directory={TREE}", *args], cwd=TREE, capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {p.stderr.strip()}")
    return p.stdout


def _paths(out):
    return {p for p in out.split("\0") if p}


def dirty():
    """Repo paths that differ from HEAD: edited, deleted or new (untracked). A rename is both of its
    paths (--no-renames), since tests can name either."""
    return _paths(git("diff", "-z", "--no-renames", "--name-only", "HEAD", "--")) | \
        _paths(git("ls-files", "-z", "--others", "--exclude-standard"))


def changed_since(base, also=()):
    """Repo paths that differ from base (committed since, edited, deleted or new), and also."""
    return sorted(_paths(git("diff", "-z", "--no-renames", "--name-only", base, "--"))
                  | _paths(git("ls-files", "-z", "--others", "--exclude-standard")) | set(also))


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
    if any(under(c, FULL_PYTHON) for c in changed):
        return list(files)
    tops = {c.split("/", 1)[0] for c in changed if "/" in c}
    names = {n for c in changed for n in (os.path.basename(c), os.path.splitext(os.path.basename(c))[0])
             if len(n) >= STEM_MIN}
    words = re.compile(r"(?<![\w-])(" + "|".join(sorted(map(re.escape, names))) + r")(?![\w-])") if names else None
    always = set(ALWAYS) | (set(ON_PYTHON) if any(c.endswith(".py") for c in changed) else set())
    picked = []
    for f in files:
        if f.split("/", 1)[0] in tops or f in changed or f in always:
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
    if any(under(c, FULL) for c in changed):
        return None
    py = pick_python(python_files(root), changed, root)
    runs = [("lint", ["just", "lint"], None), ("shell scripts", ["just", "test-bash"], None)]
    if py:
        runs += [("countdown headers", ["layout/countdown/deps.sh"], None),
                 (f"{len(py)} Python test files", ["python3", "scripts/testgate/testgate.py", "--times",
                                                   "build/tests/times-python.json", *py], None)]
    if any(under(c, ("session",)) for c in changed):
        runs.append(("session/tests", ["pytest", "-q", "session/tests"], None))
    runs.append(("C/C++ tests", ["python3", "scripts/testgate/ctests.py", "--changed", "-"], "\n".join(changed)))
    for roots, recipe in ((("keyboard", "scripts/openvr.sh"), "test-keyboard"),
                          (("testbench", ".claude/skills/verify-frametop"), "test-testbench"),
                          (("scripts/fast-install", "scripts/tests/test_fast_install.py"), "test-fast-install")):
        if any(under(c, roots) for c in changed):
            runs.append((recipe, ["just", recipe], None))
    return runs


def _tree_now():
    """HEAD and the paths edited since (what is tested that git doesn't have), as stamp lines."""
    return [git("rev-parse", "HEAD").strip(), *sorted(dirty())]


def _write(path, lines):
    """Write the stamp lines whole or not at all (a stopped write leaves no half file)."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + ".tmp", "w") as f:
        f.write("".join(f"{line}\n" for line in lines))
    os.replace(path + ".tmp", path)


def mark_start(run):
    """`just test` (its run id) starts: what it tests is the tree now, not an edit or commit made
    while it runs. Another run's file is never this one's, and a failed run's goes in a day."""
    folder = os.path.join(TREE, os.path.dirname(STAMP))
    try:
        names = os.listdir(folder)
    except OSError:
        names = []
    for name in names:
        if not name.startswith(os.path.basename(STAMP) + ".pending"):   # last-green.pending too (no run ids)
            continue
        old = os.path.join(folder, name)
        try:
            if time.time() - os.path.getmtime(old) > PENDING_DAYS * 86400:
                os.unlink(old)
        except OSError:
            pass    # gone already (another run's sweep)
    try:
        lines = _tree_now()
    except (RuntimeError, OSError):
        return 0    # no git here: the run ends with nothing to record
    _write(os.path.join(TREE, PENDING + run), lines)
    return 0


def mark_green(run):
    """`just test` (its run id) passed: the tree it started on is the last green one. With no
    record of its start (no git), the last green stays as it was: test-changed then runs more."""
    pending = os.path.join(TREE, PENDING + run)
    try:
        with open(pending) as f:
            lines = [line.strip() for line in f if line.strip()]
    except OSError:
        lines = []
    if not lines:
        print("last green: not recorded (no record of where this run started, as with no git); "
              "`just test-changed` starts from the one before")
        return 0
    os.replace(pending, os.path.join(TREE, STAMP))
    if os.path.exists(os.path.join(TREE, SEEN)):     # what objects read before green is behind it now
        with open(os.path.join(TREE, SEEN), "w") as f:
            f.write("{}")
    edited = len(lines) - 1
    print(f"last green: {lines[0][:12]}" + (f" and {edited} edited paths" if edited else "")
          + " (build/tests/last-green; `just test-changed` starts from it)")
    return 0


def read_green():
    """(commit, edited paths) from the last green run, or None."""
    try:
        with open(os.path.join(TREE, STAMP)) as f:
            lines = [line.strip() for line in f if line.strip()]
    except OSError:
        return None
    return (lines[0], lines[1:]) if lines else None


def full(why):
    print(f"test-changed: {why}: running `just test`", flush=True)
    return subprocess.run(["just", "test"], cwd=TREE).returncode


def main(argv):
    if argv[:1] in (["--mark-start"], ["--mark-green"]):
        if len(argv) != 2 or not argv[1]:
            sys.exit(f"changed.py {argv[0]} RUN: the run's id (the justfile's test_run)")
        return (mark_start if argv[0] == "--mark-start" else mark_green)(argv[1])
    base, also = (argv[0], ()) if argv else (read_green() or (None, ()))
    if base is None:
        return full("no green `just test` recorded yet")
    try:
        changed = changed_since(base, also)
    except (RuntimeError, OSError) as e:
        return full(str(e))
    if not changed:
        print(f"test-changed: nothing changed since {base[:12]}")
        return 0
    runs = plan(changed)
    if runs is None:
        return full("a change to what decides the tests (" + ", ".join(c for c in changed if under(c, FULL)) + ")")
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
