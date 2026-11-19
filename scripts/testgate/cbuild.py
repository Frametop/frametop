"""What `just test-c` builds and runs (scripts/testgate/cprograms.py lists it), and the
build.ninja that builds it: each source is compiled once per set of flags, into an object that
every program using it shares, and ninja rebuilds only what a changed file (header included,
through gcc's depfiles) reaches.

The items, in the order the run reports them:

- Program: a C or C++ program from its sources. With a `test` name it is a test (run as
  `build/tests/OUT ARGS...`, ok when it exits 0); without one it is a helper other items run.
  `syntax` only compiles it (-fsyntax-only). `needs`: the helpers it runs; `watch`: files it
  reads when it runs (not through #include).
- Check: a command, ok when it exits 0, run after the programs it `needs` are built, with
  an optional `prep` command first (run once for every Check that names it).
- Generate: a file a command writes (`{out}` in its argv) before the build; it is replaced only
  when its text changed, so what includes it isn't rebuilt for nothing.
- Skip, Fail: a line the run prints as is; a Fail fails the run.
"""
import contextlib
import fcntl
import hashlib
import json
import os
import shlex
import subprocess
import time
from dataclasses import dataclass, field

OUT = "build/tests"
# what objects have read since the last green `just test` (changed.py --mark-green empties it)
SEEN = OUT + "/deps-since-green.json"


@dataclass(frozen=True)
class Program:
    out: str                 # build/tests/OUT
    sources: tuple
    flags: str               # compiling and linking: -std, -O, warnings, sanitizers, -pthread
    cflags: str = ""         # compiling only: -I, -D, -isystem, -x
    libs: str = ""           # linking only: -l, -L, -Wl, a library's path
    pkgs: str = ""           # pkg-config packages: --cflags to compile, --libs to link
    pkglibs: str = ""        # pkg-config packages linked only (--libs)
    cc: str = "g++"
    test: str = None         # the name the run reports; None: a helper, built and not run
    args: tuple = ()
    syntax: bool = False
    needs: tuple = ()        # Program outs it runs (in args)
    watch: tuple = ()        # repo paths it reads at run time, for `--changed`

    @property
    def path(self):
        return f"{OUT}/{self.out}" + (".syntax" if self.syntax else "")


@dataclass(frozen=True)
class Check:
    test: str
    argv: tuple
    needs: tuple = ()        # Program outs it runs
    prep: tuple = None       # a command to run (once) before it
    watch: tuple = ()        # more repo paths whose change makes `--changed` run it


@dataclass(frozen=True)
class Generate:
    out: str                 # a repo path, under build/
    argv: tuple              # with "{out}" where the file goes
    watch: tuple = ()        # repo paths it is made from, for `--changed`


@dataclass(frozen=True)
class Skip:
    line: str


@dataclass(frozen=True)
class Fail:
    line: str


@dataclass
class Plan:
    """build.ninja's text, each program's object paths, and the programs that can't be built
    here (with why)."""
    ninja: str
    objects: dict = field(default_factory=dict)     # Program.path -> [object paths]
    unbuildable: dict = field(default_factory=dict)  # Program.path -> reason


def under(path, roots):
    """path is one of roots or inside one (a root may end in /)."""
    return any(path == r.rstrip("/") or path.startswith(r.rstrip("/") + "/") for r in roots)


def _pkg(what, pkgs, cache):
    """pkg-config's --cflags or --libs for pkgs, as a string; raises ValueError with its error."""
    if not pkgs:
        return ""
    key = (what, pkgs)
    if key not in cache:
        p = subprocess.run(["pkg-config", what, *pkgs.split()], capture_output=True, text=True)
        cache[key] = p.stdout.strip() if p.returncode == 0 else ValueError(
            f"pkg-config {what} {pkgs}: {p.stderr.strip() or 'failed'}")
    if isinstance(cache[key], ValueError):
        raise cache[key]
    return cache[key]


def _object(cc, flags, cflags, source):
    """Where source compiles to with these flags: one folder per set of flags."""
    tag = hashlib.sha1(f"{cc}\0{flags}\0{cflags}".encode()).hexdigest()[:10]
    return f"{OUT}/obj/{tag}/{source}.o"


def _esc(path):
    """A path in a ninja build line."""
    return path.replace("$", "$$").replace(" ", "$ ").replace(":", "$:")


def _var(text):
    """A value in a ninja variable line. It goes into the shell as it is (ninja quotes only $in
    and $out): a path in flags or libs is shell-quoted where it's made (cprograms.py)."""
    return text.replace("$", "$$").replace("\n", " ")


def _vars(**values):
    """A build line's variables."""
    return [f"  {k} = {_var(v)}" for k, v in values.items()]


def plan(items, root):
    """The build.ninja for the Programs in items, from the repo at root."""
    lines = [
        f"builddir = {OUT}",
        "rule cc",
        "  command = $cc -MMD -MF $out.d $flags $cflags -c $in -o $out",
        "  depfile = $out.d",
        "  deps = gcc",
        "  description = $out",
        "rule link",
        "  command = $cc $flags $in -o $out $libs",
        "  description = $out",
        "rule syntax",
        "  command = $cc -MMD -MF $out.d $flags $cflags -fsyntax-only $in && touch $out",
        "  depfile = $out.d",
        "  deps = gcc",
        "  description = $out",
    ]
    result = Plan("")
    cache, compiled = {}, set()
    for p in items:
        if not isinstance(p, Program):
            continue
        missing = [s for s in p.sources if not os.path.exists(os.path.join(root, s))]
        try:
            if missing:
                raise ValueError("no " + ", ".join(missing))
            cflags = " ".join(x for x in (p.cflags, _pkg("--cflags", p.pkgs, cache)) if x)
            libs = " ".join(x for x in (p.libs, _pkg("--libs", " ".join(x for x in (p.pkgs, p.pkglibs) if x), cache)) if x)
        except ValueError as e:
            result.unbuildable[p.path] = str(e)
            continue
        if p.syntax:
            lines += [f"build {_esc(p.path)}: syntax {' '.join(map(_esc, p.sources))}",
                      *_vars(cc=p.cc, flags=p.flags, cflags=cflags)]
            result.objects[p.path] = []
            continue
        objs = []
        for s in p.sources:
            o = _object(p.cc, p.flags, cflags, s)
            objs.append(o)
            if o not in compiled:
                compiled.add(o)
                lines += [f"build {_esc(o)}: cc {_esc(s)}", *_vars(cc=p.cc, flags=p.flags, cflags=cflags)]
        lines += [f"build {_esc(p.path)}: link {' '.join(map(_esc, objs))}", *_vars(cc=p.cc, flags=p.flags, libs=libs)]
        result.objects[p.path] = objs
    result.ninja = "\n".join(lines) + "\n"
    return result


def generate(item, root):
    """Run a Generate's command into a temp file and put it in place if its text changed.
    None when it worked, else the command's output."""
    out = os.path.join(root, item.out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    tmp = out + ".new"
    argv = [a.replace("{out}", tmp) for a in item.argv]
    p = subprocess.run(argv, cwd=root, capture_output=True, text=True)
    if p.returncode != 0 or not os.path.exists(tmp):
        return f"{shlex.join(argv)}: exit {p.returncode}\n{p.stdout}{p.stderr}"
    with open(tmp, "rb") as f:
        new = f.read()
    try:
        with open(out, "rb") as f:
            same = f.read() == new
    except FileNotFoundError:
        same = False
    if same:
        os.unlink(tmp)
    else:
        os.replace(tmp, out)
    return None


def stale(root, ninja_file, targets):
    """The targets ninja would still build (a dry run): the ones whose build failed."""
    if not targets:
        return set()
    p = subprocess.run(["ninja", "-f", ninja_file, "-n", "-k", "0", *targets], cwd=root,
                       capture_output=True, text=True, env=dict(os.environ, NINJA_STATUS="[%f/%t] "))
    left = set()
    for line in p.stdout.splitlines():
        if line.startswith("["):    # "[3/9] build/tests/x": the description is the output
            left.add(line.split("] ", 1)[-1].strip())
    return left & set(targets)


def deps(root, ninja_file):
    """Each object's inputs as gcc's depfiles last recorded them (ninja -t deps): object -> set
    of repo-relative paths (system headers left out)."""
    p = subprocess.run(["ninja", "-f", ninja_file, "-t", "deps"], cwd=root, capture_output=True, text=True)
    out, current = {}, None
    for line in p.stdout.splitlines():
        if line and not line.startswith(" "):
            current = line.split(":", 1)[0]
            out[current] = set()
        elif current and line.strip():
            path = os.path.normpath(line.strip())
            if not os.path.isabs(path) and not path.startswith(".."):
                out[current].add(path)
    return out


def _seen(root):
    """SEEN as it is: object -> {path: when a build last recorded it (time.time())}."""
    try:
        with open(os.path.join(root, SEEN)) as f:
            data = json.load(f)
        return {o: dict(paths) if isinstance(paths, dict) else dict.fromkeys(paths, 0.0) for o, paths in data.items()}
    except (OSError, ValueError, TypeError, AttributeError):
        return {}


@contextlib.contextmanager
def _seen_lock(root):
    """SEEN's read-change-write, one at a time (a `just test` and a `just test-changed` at once)."""
    os.makedirs(os.path.join(root, OUT), exist_ok=True)
    with open(os.path.join(root, SEEN + ".lock"), "w") as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        yield


def _write_seen(root, seen):
    path = os.path.join(root, SEEN)
    with open(path + ".tmp", "w") as f:     # whole or not at all
        json.dump(seen, f, sort_keys=True)
    os.replace(path + ".tmp", path)


def remembered(root):
    """What each object has read in any build since the last green `just test` (SEEN)."""
    return {o: set(paths) for o, paths in _seen(root).items()}


def remember(root, reads):
    """Add reads (object -> paths) to SEEN, as recorded now (a later time already there stays)."""
    with _seen_lock(root):
        now = time.time()   # in the lock: one that waited for it doesn't record an earlier time
        seen = _seen(root)
        for o, paths in reads.items():
            kept = seen.setdefault(o, {})
            for path in paths:
                kept[path] = max(kept.get(path, 0.0), now)
        _write_seen(root, seen)


def forget(root, before):
    """A `just test` that started at before (time.time()) passed: SEEN keeps only what builds
    recorded after that (a `just test-changed` beside it saw a tree the green run didn't)."""
    if not os.path.exists(os.path.join(root, SEEN)):
        return
    with _seen_lock(root):
        seen = {o: kept for o, paths in _seen(root).items()
                if (kept := {p: t for p, t in paths.items() if t >= before})}
        _write_seen(root, seen)
