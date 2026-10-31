"""Frametop commands: shell commands of your own, saved under a name, run in the background.

The list is ~/.config/frametop-commands.json:
  {"commands": [{"name": "stop-framecorder", "run": "killall framecorder"}]}

A name is a shell alias's name (letters, digits, ".", "_", "-"; at most 40), and it can't be a
program on the PATH or a shell word, so an alias never hides one. A command runs with sh -c, in
the background, as a transient systemd user unit (frametop-cmd-NAME-TIME): nothing waits for it,
its output goes to the journal (journalctl --user -u 'frametop-cmd-NAME-*'), and `ft-cmd stop
NAME` stops it. Without systemd-run it starts in a session of its own instead.

Where a name works (docs/commands.md):
  - the input relay's action "run:NAME", for mouse and controller buttons and key combinations
    (Frametop Input Settings), also while Frametop is paused for a game;
  - frunner's search (Meta+Space): type the name, Enter runs it;
  - Frametop Voice: the wake word, then "run NAME" (dashes and dots said as spaces);
  - a terminal: `ft-cmd NAME`, and NAME itself, an alias from ~/.config/frametop/aliases.sh, which
    ~/.bashrc sources once a command is saved (BASHRC_LINES).

Standard library only: the relay runs it with the host's python3, and the tests import it.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.realpath(__file__))), "conf"))
import ft_store  # noqa: E402  (JSON files read and written safely, DEE-300)

COMMANDS_PATH = os.path.expanduser("~/.config/frametop-commands.json")
ALIASES_PATH = os.path.expanduser("~/.config/frametop/aliases.sh")
BASHRC = os.path.expanduser("~/.bashrc")
CLI_LINK = os.path.expanduser("~/.local/bin/ft-cmd")
HERE = os.path.dirname(os.path.realpath(__file__))
REPO = os.path.dirname(HERE)
FT_CMD = os.path.join(HERE, "ft-cmd")
# Commands find Frametop's own tools (ft-layout, ft-float, ft-steam, ft-voice) on their PATH, as
# the relay's "command:CMD" always did.
TOOL_DIRS = [os.path.join(REPO, d) for d in ("layout", "float", "steam", "voice", "commands")]
UNIT_PREFIX = "frametop-cmd-"
NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,39}")
# bash's reserved words and builtins: an alias by one of these names would hide it.
SHELL_WORDS = set("""
! [[ ]] case coproc do done elif else esac fi for function if in select then time until while { }
. : [ alias bg bind break builtin caller cd command compgen complete compopt continue declare dirs
disown echo enable eval exec exit export false fc fg getopts hash help history jobs kill let local
logout mapfile popd printf pushd pwd read readarray readonly return set shift shopt source suspend
test times trap true type typeset ulimit umask unalias unset wait
""".split())
# ft-cmd's own words (ft-cmd list, ft-cmd stop NAME...): no command can have them as its name.
RESERVED = {"run", "list", "add", "remove", "stop", "log", "help"}
BASHRC_MARK = "# Frametop commands as shell aliases"
BASHRC_LINES = [BASHRC_MARK + " (Frametop Input Settings, Commands; uninstall.sh removes these lines)",
                '[ -f ~/.config/frametop/aliases.sh ] && . ~/.config/frametop/aliases.sh']


def tool_path(path=None):
    """PATH for a command: Frametop's tool folders first, then `path` (the caller's PATH)."""
    rest = path if path is not None else os.environ.get("PATH", "/usr/local/bin:/usr/bin")
    return ":".join(TOOL_DIRS + [rest])


def spoken(name):
    """A name as it's said: "stop-framecorder" -> "stop framecorder"."""
    return " ".join(w for w in re.split(r"[._-]+", name.lower()) if w)


def name_problem(name, taken=(), path=None):
    """Why `name` can't be a command's name, or "" if it can. taken: names already in use."""
    if not isinstance(name, str) or not NAME.fullmatch(name):
        return "use letters, digits, '.', '_' and '-' (no spaces), starting with a letter or digit, at most 40"
    if name in taken:
        return f"there's already a command called {name}"
    if name in SHELL_WORDS:
        return f"{name} is a shell word; an alias by that name would hide it"
    if name == "ft-cmd" or name in RESERVED:
        return f"{name} is one of ft-cmd's own words (ft-cmd {name} ...)" if name in RESERVED else "ft-cmd runs the commands"
    if shutil.which(name, path=path):
        return f"{name} is a program on your PATH; an alias by that name would hide it"
    return ""


def parse(data):
    """The valid commands in a commands file's data, and a warning for each entry left out."""
    entries = data.get("commands") if isinstance(data, dict) else None
    out, warnings, seen = [], [], set()
    if entries is None:
        return out, warnings
    if not isinstance(entries, list):
        return out, [f"commands: expected a list, got {type(entries).__name__}"]
    for i, e in enumerate(entries):
        name = e.get("name") if isinstance(e, dict) else None
        run = e.get("run") if isinstance(e, dict) else None
        if not isinstance(name, str) or not NAME.fullmatch(name):
            warnings.append(f"commands[{i}] left out: a bad name ({name!r})")
        elif name in seen:
            warnings.append(f"commands[{i}] left out: {name} again")
        elif not isinstance(run, str) or not run.strip():
            warnings.append(f"commands[{i}] left out: {name} has nothing to run")
        else:
            seen.add(name)
            out.append({"name": name, "run": run.strip()})
    return out, warnings


# The commands file is there but doesn't read (or has entries that would be dropped): it's kept
# as it is, with a copy next to it, and nothing is saved over it (DEE-295).
CommandsFileError = ft_store.StoreError


def _why_not(data):
    """Why data can't be saved back without losing something, or None."""
    if not isinstance(data, dict) or ("commands" in data and not isinstance(data["commands"], list)):
        return 'it isn\'t {"commands": [...]}'
    return "; ".join(parse(data)[1]) or None


def read_strict(path=None):
    """The saved commands ([] when there's no file). CommandsFileError when the file doesn't read
    as JSON, isn't {"commands": [...]}, or has an entry parse() would leave out: saving the list
    back would lose what's there."""
    data = ft_store.load_json_strict(path or COMMANDS_PATH, None, _why_not)
    return [] if data is None else parse(data)[0]


def load(path=None, strict=False):
    """The saved commands ([] when there's no file). A file that doesn't read gives the commands
    it could read, said on stderr; strict: CommandsFileError (writers read this way)."""
    try:
        return read_strict(path)
    except CommandsFileError as e:
        if strict:
            raise
        print(f"frametop commands: {e}", file=sys.stderr)
        try:
            with open(path or COMMANDS_PATH, encoding="utf-8") as f:
                return parse(json.load(f))[0]
        except (OSError, ValueError):
            return []


def find(name, commands=None):
    return next((c for c in (load() if commands is None else commands) if c["name"] == name), None)


def update(fn, path=None, aliases=True):
    """Change the list under its lock: fn gets the saved commands and returns the new list, which
    is written atomically; then the aliases file and ~/.bashrc's lines (aliases=False: the list
    only). A file that doesn't read is never saved over: CommandsFileError (DEE-295)."""
    path = path or COMMANDS_PATH
    with ft_store.locked(path):   # the aliases too, so they're never an older list's
        out = fn(read_strict(path))
        ft_store.write_json(path, {"commands": [{"name": c["name"], "run": c["run"]} for c in out]})
        if aliases:
            write_aliases(out)
    return out


def save(commands, path=None, aliases=True):
    """Write the list as it is (see update)."""
    return update(lambda _: list(commands), path, aliases)


def add(name, run, path=None):
    """Save NAME (one saved meanwhile by that name is replaced, never doubled)."""
    return update(lambda cs: [c for c in cs if c["name"] != name] + [{"name": name, "run": run}], path)


def remove(name, path=None):
    return update(lambda cs: [c for c in cs if c["name"] != name], path)


def quote(text):
    return "'" + text.replace("'", "'\\''") + "'"


def aliases_text(commands, ft_cmd=None):
    ft_cmd = ft_cmd or CLI_LINK
    lines = ["# Frametop commands (Frametop Input Settings, Commands). Frametop writes this file:",
             "# change the commands there, or in ~/.config/frametop-commands.json, not here.",
             "# Each runs in the background; ft-cmd stop NAME stops it."]
    lines += [f"alias {c['name']}={quote(ft_cmd + ' ' + c['name'])}" for c in commands]
    return "\n".join(lines) + "\n"


def write_aliases(commands, path=None, bashrc=None):
    """The aliases file, and ~/.bashrc's lines that source it (added once, when there's a command)."""
    path = path or ALIASES_PATH
    text = aliases_text(commands)
    try:
        with open(path) as f:
            same = f.read() == text
    except OSError:
        same = False
    if not same:
        ft_store.write_text(path, text)
    if commands:
        add_bashrc_lines(bashrc)


def add_bashrc_lines(bashrc=None):
    """Append BASHRC_LINES to ~/.bashrc unless it has them. True if it added them."""
    bashrc = bashrc or BASHRC
    try:
        with open(bashrc, encoding="utf-8", errors="surrogateescape", newline="") as f:  # bytes kept
            text = f.read()
    except FileNotFoundError:
        text = ""
    if BASHRC_MARK in text:
        return False
    _write_bashrc(bashrc, text + ("\n" if text and not text.endswith("\n") else "") + "\n"
                  + "\n".join(BASHRC_LINES) + "\n")
    return True


def _write_bashrc(bashrc, text):
    """~/.bashrc rewritten whole, atomically (temp file, fsync, rename; mode kept, a symlink
    followed), so a crash never leaves half of it. One we can't write stays as it is."""
    if os.path.exists(bashrc) and not (os.access(bashrc, os.W_OK) and os.stat(bashrc).st_mode & 0o222):
        raise PermissionError(f"{bashrc} is read-only: left as it is")
    ft_store.write_text(bashrc, text)


def remove_bashrc_lines(bashrc=None):
    """Take BASHRC_LINES (and the blank line before them) out of ~/.bashrc. True if it did."""
    bashrc = bashrc or BASHRC
    try:
        with open(bashrc, encoding="utf-8", errors="surrogateescape", newline="") as f:  # bytes kept
            lines = f.read().split("\n")
    except FileNotFoundError:
        return False
    out, i, changed = [], 0, False
    while i < len(lines):
        if lines[i].startswith(BASHRC_MARK):
            changed = True
            i += 1
            if i < len(lines) and lines[i] == BASHRC_LINES[1]:
                i += 1
            if out and out[-1] == "":
                out.pop()
            continue
        out.append(lines[i])
        i += 1
    if changed:
        _write_bashrc(bashrc, "\n".join(out))
    return changed


def link_cli(link=None):
    """~/.local/bin/ft-cmd -> this checkout's ft-cmd (the aliases call it there). True if changed."""
    link = link or CLI_LINK
    if os.path.islink(link) and os.readlink(link) == FT_CMD:
        return False
    if os.path.exists(link) and not os.path.islink(link):
        return False  # someone's own file: leave it
    os.makedirs(os.path.dirname(link), exist_ok=True)
    tmp = f"{link}.tmp{os.getpid()}"
    os.symlink(FT_CMD, tmp)
    os.replace(tmp, link)
    return True


def refresh():
    """At the relay's start: the CLI link points at this install, and the aliases file matches
    the list (a release update moves the install)."""
    link_cli()
    commands = load()
    if commands or os.path.exists(ALIASES_PATH):
        write_aliases(commands)


def unit_name(name, now=None):
    """frametop-cmd-NAME-TIME (a name's characters are all valid in unit names)."""
    stamp = int((time.time() if now is None else now) * 1000)
    return f"{UNIT_PREFIX}{name}-{stamp}"


def user_bus_env(env=None):
    """The environment with the user's systemd bus: the Frametop desktop runs on a private D-Bus
    session bus, where `systemctl --user` and systemd-run --user don't reach the user manager."""
    env = dict(os.environ if env is None else env)
    runtime = env.get("XDG_RUNTIME_DIR") or f"/run/user/{os.getuid()}"
    env["XDG_RUNTIME_DIR"] = runtime
    env["DBUS_SESSION_BUS_ADDRESS"] = f"unix:path={runtime}/bus"
    return env


def run(command, name, env=None):
    """Start `command` (sh -c) in the background. Returns how: "unit UNIT" or "process PID".
    Raises OSError if it couldn't start at all."""
    env = dict(os.environ if env is None else env)
    path = tool_path(env.get("PATH"))
    # FT_COMMANDS_NO_SYSTEMD=1: always a plain process (the tests: no real units on the host).
    systemd_run = None if env.get("FT_COMMANDS_NO_SYSTEMD") == "1" else shutil.which("systemd-run")
    if systemd_run:
        unit = unit_name(name)
        r = subprocess.run([systemd_run, "--user", "--collect", "--quiet", "--no-block",
                            f"--unit={unit}", f"--description=Frametop command {name}",
                            f"--setenv=PATH={path}", "--setenv=FRAMETOP_COMMAND=" + name,
                            "/bin/sh", "-c", command],
                           env=user_bus_env(env), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE, text=True, timeout=10)
        if r.returncode == 0:
            return f"unit {unit}"
        why = r.stderr.strip().splitlines()[-1] if r.stderr.strip() else f"status {r.returncode}"
        log_fallback = f"systemd-run: {why}; started as a plain process"
    else:
        log_fallback = "without systemd-run"
    p = subprocess.Popen(["/bin/sh", "-c", command], env=dict(env, PATH=path, FRAMETOP_COMMAND=name),
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True)
    return f"process {p.pid} ({log_fallback})"


def run_named(name, env=None):
    """Run the saved command `name`. Returns (ok, message)."""
    c = find(name)
    if c is None:
        return False, f"no command called {name}"
    try:
        how = run(c["run"], name, env)
    except (OSError, subprocess.SubprocessError) as e:
        return False, f"{name}: couldn't start: {e}"
    return True, f"{name}: started ({how})"


def running(name=None, env=None):
    """The commands' units running now: [(unit, name)], for `name` only if given."""
    pattern = f"{UNIT_PREFIX}{name}-*" if name else f"{UNIT_PREFIX}*"
    try:
        r = subprocess.run(["systemctl", "--user", "list-units", "--plain", "--no-legend", "--state=active",
                            pattern + ".service"], env=user_bus_env(env), capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return []
    out = []
    for line in r.stdout.splitlines():
        unit = line.split()[0] if line.split() else ""
        m = re.fullmatch(re.escape(UNIT_PREFIX) + r"(.+)-\d+\.service", unit)
        if m and (name is None or m.group(1) == name):  # the glob for "stop" takes "stop-x"'s too
            out.append((unit, m.group(1)))
    return out


def stop(name, env=None):
    """Stop `name`'s running units. Returns how many there were."""
    units = [u for u, _ in running(name, env)]
    if units:
        subprocess.run(["systemctl", "--user", "stop", *units], env=user_bus_env(env),
                       stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
    return len(units)
