#!/usr/bin/env python3
"""ft_store: the JSON files Frametop keeps its state in, read and written safely (DEE-300).

    data = ft_store.load_json(path, {})                  # a file that doesn't read: {}, said once
    data = ft_store.load_json_strict(path, {}, check)    # one that doesn't read: StoreError
    ft_store.write_json(path, data, indent=2)            # temp file, fsync, rename
    ft_store.update_json(path, fn, {})                   # lock, strict read, fn, write

A file that doesn't read (not JSON, or check(data) says why not) is never written over: the
strict reader copies it to PATH.bad-TIME (once per content) and raises StoreError, whose text
says where it is and that Frametop won't save over it. Writers read strictly; readers that only
show things use load_json and go on with the default.

Every write goes to a temp file of its own in the same folder, is flushed to disk and renamed
over the file, so a reader (or a crash) never sees half a file. update_json() also locks
.NAME.lock next to the file, so writers in different programs take turns and none loses
another's change; it waits up to `timeout` seconds, then refuses (StoreError).
Standard library only: ft-eyes runs it in the eye tracker's own venv.
"""
import contextlib
import fcntl
import json
import os
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ft_conf  # noqa: E402  (ft_conf.write_atomic: temp file, fsync, rename; mode kept)


class StoreError(RuntimeError):
    """The file is there but doesn't read, or couldn't be locked: nothing was saved over it."""


def backup(path):
    """A copy of path at PATH.bad-TIME, unless one with the same bytes is there already. Its
    name, or None when no copy could be made."""
    try:
        with open(path, "rb") as f:
            data = f.read()
        folder, name = os.path.split(path)
        for other in sorted(os.listdir(folder or ".")):
            if other.startswith(name + ".bad-"):
                with open(os.path.join(folder, other), "rb") as f:
                    if f.read() == data:
                        return os.path.join(folder, other)
        stamp = f"{path}.bad-{time.strftime('%Y%m%d-%H%M%S')}"
        for n in range(100):   # another bad version in the same second: -1, -2, ...
            copy = stamp + (f"-{n}" if n else "")
            try:
                with open(copy, "xb") as f:
                    f.write(data)
                return copy
            except FileExistsError:
                continue
    except OSError:
        pass
    return None


def refuse(path, why):
    """StoreError for path (after keeping a copy of it), its text saying why."""
    copy = backup(path)
    return StoreError(f"{path}: {why}. Fix it or move it away: Frametop won't save over it"
                      + (f" (a copy: {copy})" if copy else ""))


def is_object(data):
    """A check: why data isn't a JSON object, or None."""
    return None if isinstance(data, dict) else "it isn't a JSON object"


def load_json_strict(path, default=None, check=None):
    """The file's data; default when there's none. StoreError (a copy kept) when it isn't JSON
    or check(data) gives a reason."""
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except FileNotFoundError:
        return default
    except (OSError, ValueError) as e:
        raise refuse(path, f"it doesn't read ({e})") from None
    why = check(data) if check else None
    if why:
        raise refuse(path, why)
    return data


_said = set()


def load_json(path, default=None, check=None, who="frametop"):
    """load_json_strict, but a file that doesn't read gives default, said on stderr once per
    process (some readers re-read often)."""
    try:
        return load_json_strict(path, default, check)
    except StoreError as e:
        if str(e) not in _said:
            _said.add(str(e))
            print(f"{who}: {e}", file=sys.stderr, flush=True)
        return default


def dumps(data, indent=2, newline=True, **kw):
    """data as the file's text: json.dumps(indent, **kw), with a newline at the end or not."""
    return json.dumps(data, indent=indent, **kw) + ("\n" if newline else "")


def write_json(path, data, indent=2, newline=True, **kw):
    """data into path through a temp file of its own, flushed to disk and renamed (the file's
    mode kept). A symlink is followed, not replaced."""
    ft_conf.write_atomic(os.path.realpath(path), dumps(data, indent, newline, **kw))


_held = threading.local()


@contextlib.contextmanager
def locked(path, timeout=10.0):
    """The write lock for path (.NAME.lock next to it), waiting up to timeout seconds;
    StoreError when another program keeps it. Not re-entrant: taking it again in the same
    thread is a StoreError at once (it would wait on itself)."""
    path = os.path.realpath(path)
    held = _held.__dict__.setdefault("paths", set())
    if path in held:
        raise StoreError(f"{path}: already being saved by this program")
    folder, name = os.path.split(path)
    os.makedirs(folder or ".", exist_ok=True)
    with open(os.path.join(folder, "." + name + ".lock"), "a") as lock:
        deadline = time.monotonic() + timeout
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() > deadline:
                    raise StoreError(f"{path}: another program kept it locked for {timeout:g} s; "
                                     "not saved") from None
                time.sleep(0.02)
        held.add(path)
        try:
            yield
        finally:
            held.discard(path)


def update_json(path, fn, default=None, check=None, timeout=10.0, **fmt):
    """Change the file under its lock: fn gets the data as it is now (strictly read: a file that
    doesn't read is a StoreError and stays as it is) and returns the new data, or None when it
    changed it in place. The new data is written as write_json(**fmt) does and returned."""
    with locked(path, timeout):
        data = load_json_strict(path, default, check)
        new = fn(data)
        if new is None:
            new = data
        write_json(path, new, **fmt)
    return new
