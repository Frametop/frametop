#!/usr/bin/env python3
"""ft_conf: ~/.config/frametop.conf, read and written by one grammar (docs/config.md).

The session sources the file with bash (`. "$conf"`), so it has to stay shell; every other
program reads it here (C and C++: conf/ft_conf.h, the same grammar). A line is a comment, blank,
or NAME=value with an optional "# comment" after whitespace. The value is literal: bare
characters, 'single quotes' or "double quotes" without $ ` \\, side by side ('a b'c is "a bc").
Anything else (a second word, $(...), ;, a backslash) makes the line one bash would read
differently or run as a command: readers warn and leave it out, so the key keeps its default.

    conf = ft_conf.read()                       # {NAME: value}, the last line of a name wins
    ft_conf.number(conf, "POINTER_IDLE", 30.0)  # a bad value warns and gives the default
    ft_conf.set_value("POINTER_IGNORE", "a,b")  # shell-quoted, written to a temp file and renamed

Run as a program: `check FILE` (exit 3 when a line is bad, each said on stderr; 1 is Python's own
failure) and `shell FILE`
(the valid settings as KEY=value lines bash can source safely: the session's fallback).
Standard library only: ft-eyes runs it in the eye tracker's own venv.
"""
import math
import os
import re
import sys
import tempfile

PATH = os.path.expanduser("~/.config/frametop.conf")

NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\Z")
# Names a setting can't have: bash's read-only ones (setting one stops the session, which runs
# under set -e) and the ones that change how the session's shell runs. Lower-case names are the
# session script's own variables, so a setting's name is upper-case too.
# Every BASH_* name is the shell's too.
RESERVED = frozenset({"BASHOPTS", "CDPATH", "ENV", "EUID", "EXECIGNORE", "FUNCNEST", "GLOBIGNORE", "HOME", "IFS",
                      "OPTIND", "PATH", "POSIXLY_CORRECT", "PPID", "PS4", "SHELLOPTS", "TMOUT", "UID"})
# What a value can be written as without quotes: nothing bash would expand, split or run
# (~ is left out: bash expands it at the start and after a ':' in an assignment).
BARE = re.compile(r"[A-Za-z0-9_.,:/@%+=*?^-]+\Z")
# Outside quotes these end the value or mean shell syntax the readers don't follow.
SPECIAL = frozenset(" \t'\"\\`$;&|<>()")
# Inside double quotes bash expands these.
DQ_SPECIAL = frozenset("$`\\")
LIST_SEP = ","
# A number, the same in every reader (strtod and float() each take more: hex, 1_000, inf).
NUMBER = re.compile(r"[+-]?([0-9]+\.?[0-9]*|\.[0-9]+)([eE][+-]?[0-9]+)?\Z")


class ConfError(ValueError):
    pass


def name_error(name):
    """Why name can't be a setting's, or None."""
    if not NAME.match(name):
        return "not NAME=value (no spaces around the =)"
    if not name.isupper() and name.strip("_0123456789"):
        return "a lower-case name (the session script's own variables): settings are upper-case"
    if name in RESERVED or name.startswith("BASH_"):
        return "a name bash keeps for itself (read-only, or how the shell runs)"
    return None


def parse_line(line):
    """(name, value) for an assignment, None for a blank or comment line. ConfError (its text
    says why) for a line outside the grammar."""
    if "\r" in line or "\0" in line or "\n" in line:
        raise ConfError("a carriage return, newline or NUL in the line")
    i, n = 0, len(line)
    while i < n and line[i] in " \t":
        i += 1
    if i == n or line[i] == "#":
        return None
    start = i
    while i < n and (line[i].isascii() and (line[i].isalnum() or line[i] == "_")):
        i += 1
    name = line[start:i]
    if not NAME.match(name) or i == n or line[i] != "=":
        raise ConfError("not NAME=value (no spaces around the =)")
    if name_error(name):
        raise ConfError(name_error(name))
    i += 1
    value = []
    while i < n and line[i] not in " \t":
        c = line[i]
        if c == "'":
            end = line.find("'", i + 1)
            if end < 0:
                raise ConfError("a ' with no closing '")
            value.append(line[i + 1:end])
            i = end + 1
        elif c == '"':
            j = i + 1
            while j < n and line[j] != '"':
                if line[j] in DQ_SPECIAL:
                    raise ConfError('$, ` or \\ inside "double quotes" (bash expands them): use \'single quotes\'')
                j += 1
            if j == n:
                raise ConfError('a " with no closing "')
            value.append(line[i + 1:j])
            i = j + 1
        elif c in SPECIAL:
            raise ConfError(f"{c!r} outside quotes (shell syntax): quote the value")
        else:
            value.append(c)
            i += 1
    while i < n and line[i] in " \t":
        i += 1
    if i < n and line[i] != "#":
        raise ConfError("more than one word after the = (bash runs the rest as a command): quote the value")
    return name, "".join(value)


_said = set()


def warn(text):
    """One line on stderr, once per process for the same text (some readers re-read often)."""
    if text not in _said:
        _said.add(text)
        print(text, file=sys.stderr, flush=True)


def parse(text, warn=warn, where="frametop.conf"):
    """{name: value} from the file's text: the last line of a name wins, as in bash. A bad line
    is left out (the name keeps an earlier line's value, or the reader's default) and warned."""
    conf = {}
    for no, line in enumerate(text.split("\n"), 1):
        try:
            kv = parse_line(line)
        except ConfError as e:
            if warn:
                head = line.split("=", 1)[0].strip()[:40]
                warn(f"{where}:{no}: {head or 'line'} left out: {e}")
            continue
        if kv:
            conf[kv[0]] = kv[1]
    return conf


def read(path=None, warn=warn):
    """{name: value} from frametop.conf (or path); {} when it's missing or unreadable."""
    path = path or PATH
    try:
        with open(path, encoding="utf-8", errors="replace", newline="") as f:
            text = f.read()
    except OSError:
        return {}
    return parse(text, warn, os.path.basename(path))


def number(conf, key, default, cast=float, warn=warn):
    """conf[key] as a number (cast float or int), or default when it's unset or empty. A value
    that isn't a finite number (garbage, nan, 2.5 for an int) warns and gives default."""
    v = conf.get(key, "")
    if v == "":
        return default
    try:
        if not NUMBER.match(v):
            raise ValueError
        x = float(v)
        if not math.isfinite(x) or (cast is int and x != int(x)):
            raise ValueError
        return int(x) if cast is int else cast(x)
    except ValueError:
        if warn:
            warn(f"frametop.conf: {key}={v!r} isn't a number; using {default}")
        return default


def items(value):
    """A list value (POINTER_IGNORE): its comma-separated items, trimmed, empty ones dropped."""
    return [p.strip() for p in value.split(LIST_SEP) if p.strip()]


def quote(value):
    """value as the conf writes it: bare when that's safe, else in single quotes ('\"'\"' for a
    quote inside). A newline, carriage return or NUL can't be written: ValueError."""
    value = str(value)
    if any(c in value for c in "\n\r\0"):
        raise ValueError("a frametop.conf value can't hold a newline, carriage return or NUL")
    if value == "" or BARE.match(value):
        return value
    return "'" + value.replace("'", "'\"'\"'") + "'"


def _comment(line):
    """The line's trailing "# comment", or ""."""
    try:
        parse_line(line)
        ok = True
    except ConfError:
        ok = False
    # A valid line's comment is the first # after whitespace outside quotes; the value can't
    # hold unquoted whitespace, so the first whitespace past the = ends it.
    eq = line.find("=")
    rest = line[eq + 1:] if eq >= 0 else line
    if ok:
        quote_ch = None
        for j, c in enumerate(rest):
            if quote_ch:
                if c == quote_ch:
                    quote_ch = None
            elif c in "'\"":
                quote_ch = c
            elif c in " \t":
                k = rest.find("#", j)
                return rest[k:] if k >= 0 else ""
        return ""
    m = re.search(r"[ \t]#.*\Z", rest)
    # without a CR or NUL (a file saved with CRLF endings): kept, the new line would be bad again
    return re.sub(r"[\r\0]", "", m.group(0)).lstrip() if m else ""


def set_value(key, value, path=None):
    """Set KEY=value in frametop.conf (or path), keeping its comment and every other line. The
    last line that sets KEY (even one bash can't read) is the one changed, as it's the one that
    counts; none: added at the end. Written to a temp file next to it and renamed over it, so a
    reader never sees half a file."""
    if name_error(key):
        raise ValueError(f"not a setting name: {key!r}")
    text = quote(value)
    path = os.path.realpath(path or PATH)
    try:
        with open(path, encoding="utf-8", errors="surrogateescape", newline="") as f:
            lines = f.read().split("\n")
        if lines and lines[-1] == "":
            lines.pop()
    except FileNotFoundError:
        lines = []
    sets = re.compile(r"[ \t]*" + re.escape(key) + r"[ \t]*=")
    new = f"{key}={text}"
    for i in range(len(lines) - 1, -1, -1):
        if sets.match(lines[i]):
            comment = _comment(lines[i])
            lines[i] = new + (f"   {comment}" if comment else "")
            break
    else:
        lines.append(new)
    write_atomic(path, "\n".join(lines) + "\n")


def write_atomic(path, text):
    """text into path by a temp file in the same folder, fsynced and renamed over it; the old
    file's mode is kept (a new one gets 0644)."""
    folder = os.path.dirname(path) or "."
    os.makedirs(folder, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=folder, prefix="." + os.path.basename(path) + ".", suffix=".tmp")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        try:
            mode = os.stat(path).st_mode & 0o7777
        except FileNotFoundError:
            mode = 0o644
        os.chmod(tmp, mode)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def shell(path):
    """The file's valid settings as KEY=value lines bash can source without running anything."""
    return "".join(f"{k}={quote(v)}\n" for k, v in read(path).items())


def check(path):
    """True when every line of path is in the grammar (each bad one is said on stderr)."""
    bad = []
    try:
        with open(path, encoding="utf-8", errors="replace", newline="") as f:
            parse(f.read(), bad.append, os.path.basename(path))
    except OSError as e:
        print(f"frametop.conf: {e}", file=sys.stderr)
        return False
    for b in bad:
        print(b, file=sys.stderr)
    return not bad


BAD_LINES = 3   # check's exit for a file with bad lines (1 is any Python error)


def main(argv):
    if len(argv) == 3 and argv[1] == "check":
        return 0 if check(argv[2]) else BAD_LINES
    if len(argv) == 3 and argv[1] == "shell":
        sys.stdout.write(shell(argv[2]))
        return 0
    print("usage: ft_conf.py check FILE | shell FILE", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
