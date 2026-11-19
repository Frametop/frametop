#!/usr/bin/env python3
"""`just test-c`: build the C/C++ tests scripts/testgate/cprograms.py lists with ninja (in
parallel, each shared source once, only what a change reaches) and run them in order, saying of
each whether it was ok, FAILED (with its output) or SKIPPED (with why).

- A program that doesn't build fails, and only it: the rest still build and run. The compiler's
  errors come first, then each test that didn't build says so in its place.
- --changed FILE (one repo path a line; - for stdin) runs only the tests those paths reach: a
  program's sources and the headers they included when it last built (generated ones too, when
  what they're made from changed), the files it reads and the helpers it runs; a check's files,
  folders and programs. A test with no record of a build before this run, or that doesn't
  build now, always runs. Everything still builds (ninja keeps that quick).
- The tests run side by side, as many at once as compile at once, and are reported in order.
  Each runs in a session of its own with a log of its own (build/tests/logs): what it leaves
  running is ended, and one still running after FT_TEST_TIMEOUT seconds (procs.py) is killed
  and fails.
- How long each test took goes in build/tests/times-c.json, and the slowest are named at the end.
- --jobs N: compiles and tests at once (scripts/testgate/jobs.py picks it otherwise; FT_TEST_JOBS too).

  python3 scripts/testgate/ctests.py [--jobs N] [--changed FILE]
"""
import argparse
import concurrent.futures
import json
import os
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cbuild  # noqa: E402
import cprograms  # noqa: E402
import procs  # noqa: E402
from cbuild import Check, Fail, Generate, Program, Skip, under  # noqa: E402
from jobs import jobs  # noqa: E402

TREE = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# a change to these can change any test: the build of every one
EVERYTHING = ("scripts/testgate/cbuild.py", "scripts/testgate/cprograms.py", "scripts/testgate/ctests.py", "justfile")


def _repo_paths(argv, root):
    return [a for a in argv if not a.startswith("-") and os.path.exists(os.path.join(root, a))]


class Run:
    def __init__(self, items, root, njobs, changed=None, out=sys.stdout):
        self.items, self.root, self.njobs, self.out = items, root, njobs, out
        self.changed = None if changed is None or any(c in EVERYTHING for c in changed) else set(changed)
        self.status = 0
        self.times = {}
        self.preps = {}
        self.not_run = 0
        self.live = set()   # the tests running now (their process group ids)
        self.generated = {g.out: g for g in items if isinstance(g, Generate)}
        self.programs = {i.out: i for i in items if isinstance(i, Program)}
        if self.changed is not None:    # a generated file changes with what it's made from
            self.changed |= {g.out for g in self.generated.values() if any(under(c, g.watch) for c in self.changed)}

    def say(self, text):
        print(text, file=self.out, flush=True)

    # the build

    def build(self):
        bdir = os.path.join(self.root, cbuild.OUT)
        os.makedirs(bdir, exist_ok=True)
        for g in self.generated.values():
            err = cbuild.generate(g, self.root)
            if err:
                self.say(f"{g.out}: FAILED to make it\n{err}")
                self.status = 1
        self.plan = cbuild.plan(self.items, self.root)
        self.ninja_file = f"{cbuild.OUT}/build.ninja"
        with open(os.path.join(self.root, self.ninja_file), "w") as f:
            f.write(self.plan.ninja)
        # what each object read when it last built, before this build adds the ones it builds now
        self.deps = cbuild.deps(self.root, self.ninja_file) if self.changed is not None else {}
        targets = sorted(self.plan.objects)
        start = time.monotonic()
        p = subprocess.run(["ninja", "-f", self.ninja_file, "-k", "0", "-j", str(self.njobs), "--quiet", *targets],
                           cwd=self.root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.times["(build)"] = round(time.monotonic() - start, 2)
        self.unbuilt = cbuild.stale(self.root, self.ninja_file, targets) if p.returncode else set()
        if p.returncode:
            self.say(p.stdout.rstrip())
            self.status = 1

    def broken(self, path):
        """Why the program at path isn't there to run, or None."""
        if path in self.plan.unbuildable:
            return f"can't build here: {self.plan.unbuildable[path]}"
        if path in self.unbuilt:
            return "didn't build: the errors are above"
        return None

    def missing(self, needs):
        """The needed programs that aren't there to run."""
        return [n for n in needs if n not in self.programs or self.broken(self.programs[n].path)]

    # what a change reaches

    def program_hit(self, p):
        if self.changed is None or self.broken(p.path) or self.missing(p.needs):
            return True
        if any(under(c, p.watch) for c in self.changed):
            return True
        for o in self.plan.objects.get(p.path) or [p.path]:
            if o not in self.deps:
                return True     # no build before this run: no record of what it reads
            if self.deps[o] & self.changed:
                return True
        return any(self.program_hit(self.programs[n]) for n in p.needs)

    def check_hit(self, c):
        if self.changed is None:
            return True
        roots = (*_repo_paths(c.argv, self.root), *c.watch)
        if any(under(x, roots) for x in self.changed):
            return True
        return any(self.program_hit(self.programs[n]) for n in c.needs if n in self.programs)

    # the run

    def execute(self, name, argv):
        """Run one test in a session of its own, with a log of its own: (ok, what it printed). What
        it leaves running is ended (procs.py), and one still running after FT_TEST_TIMEOUT seconds
        is killed and fails."""
        log = os.path.join(self.root, cbuild.OUT, "logs", name.replace("/", "_") + ".log")
        os.makedirs(os.path.dirname(log), exist_ok=True)
        limit = procs.timeout()
        start = time.monotonic()
        with open(log, "w") as f:
            try:
                code = procs.run_session(argv, self.live, limit, cwd=self.root, stdout=f, stderr=subprocess.STDOUT)
            except OSError as e:    # not there, not executable: this test fails, the rest run
                self.times[name] = 0
                return False, f"ctests: can't run {argv[0]}: {e}"
        self.times[name] = round(time.monotonic() - start, 2)
        with open(log, errors="replace") as f:
            printed = f.read().rstrip()
        if code is None:
            printed += f"\nctests: still running after {limit:g} s, killed"
        return code == 0, printed

    def prep(self, argv):
        """Run a Check's prep once: None when it worked, else its output."""
        if argv not in self.preps:
            p = subprocess.run(argv, cwd=self.root, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
            self.preps[argv] = None if p.returncode == 0 else f"{' '.join(argv)} failed:\n{p.stdout.rstrip()}"
        return self.preps[argv]

    def planned(self):
        """(name, argv) to run, or (line, failed) to say, for each item in order."""
        for i in self.items:
            if isinstance(i, (Skip, Fail)):
                yield "say", (i.line, isinstance(i, Fail))
            elif isinstance(i, Program) and i.test:
                if not self.program_hit(i):
                    self.not_run += 1
                elif self.broken(i.path):
                    yield "say", (f"{i.test}: FAILED ({self.broken(i.path)})", True)
                elif self.missing(i.needs):
                    yield "say", (f"{i.test}: FAILED (needs {', '.join(self.missing(i.needs))}, which didn't build)", True)
                elif i.syntax:
                    yield "say", (f"{i.test}: ok", False)
                else:
                    yield "run", (i.test, [i.path, *i.args])
            elif isinstance(i, Check):
                if not self.check_hit(i):
                    self.not_run += 1
                    continue
                gone = self.missing(i.needs)
                err = (f"needs {', '.join(gone)}, which didn't build" if gone
                       else self.prep(i.prep) if i.prep else None)
                if err:
                    yield "say", (f"{i.test}: FAILED ({err})", True)
                else:
                    yield "run", (i.test, list(i.argv))

    def run(self):
        """The tests side by side (njobs at once: each has its own ports, sockets and folders),
        each reported in order as soon as those before it are."""
        def show(kind, what):
            if kind == "say":
                line, failed = what
                self.say(line)
                self.status |= failed
                return
            name, future = what
            ok, printed = future.result()
            self.say(f"{name}: ok" if ok else f"{name}: FAILED" + (f"\n{printed}" if printed else ""))
            self.status |= not ok

        pool = concurrent.futures.ThreadPoolExecutor(max_workers=max(1, self.njobs))
        try:
            pending = [(k, w if k == "say" else (w[0], pool.submit(self.execute, *w))) for k, w in self.planned()]
            for kind, what in pending:
                show(kind, what)
        except BaseException:   # stopped (Ctrl-C, SIGTERM): what runs goes, what waits doesn't start
            pool.shutdown(wait=False, cancel_futures=True)
            procs.stop(self.live)
            raise
        pool.shutdown()

    def report(self):
        with open(os.path.join(self.root, cbuild.OUT, "times-c.json"), "w") as f:
            json.dump(self.times, f, indent=1, sort_keys=True)
        ran = {k: v for k, v in self.times.items() if k != "(build)"}
        slow = sorted(ran.items(), key=lambda kv: -kv[1])[:5]
        line = f"C tests ({self.njobs} at once): built in {self.times.get('(build)', 0):g} s, ran {len(ran)}"
        if self.not_run:
            line += f", {self.not_run} not reached by the change"
        if slow:
            line += "; slowest: " + ", ".join(f"{k} {v:g} s" for k, v in slow)
        self.say(line)


def main(argv=None):
    a = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    a.add_argument("--jobs", type=int, default=0)
    a.add_argument("--changed", help="a file of changed repo paths, one a line (- for stdin)")
    args = a.parse_args(argv)
    changed = None
    if args.changed:
        f = sys.stdin if args.changed == "-" else open(args.changed)
        changed = [line.strip() for line in f if line.strip()]
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(128 + signal.SIGTERM))   # so the tests running end too
    cprograms.prepare(TREE)
    r = Run(cprograms.items(TREE), TREE, args.jobs or jobs(), changed)
    r.build()
    r.run()
    r.report()
    return r.status


if __name__ == "__main__":
    sys.exit(main())
