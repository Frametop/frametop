"""ft-npud runs as root: every file it executes or loads must be one only root can change, or uid 1000 could run code
as root by editing a .py, a .so or a graph. untrusted() lists what fails that: a file, or any directory above it,
that isn't owned by root or is writable by group or others (a sticky directory still counts: its owner's files
could be swapped out by the owner), along the way the kernel itself resolves the path."""
import errno, os, stat, sys
from pathlib import PurePosixPath


def _problem(st, what, files_only=False):
    # files_only (the install tree): files, directories and links only; a device node or a FIFO there, or a link to
    # one (/dev/mem, say), would pass every ownership check while handing out the disk or blocking the load
    if files_only and not (stat.S_ISREG(st.st_mode) or stat.S_ISDIR(st.st_mode) or stat.S_ISLNK(st.st_mode)):
        return '%s: not a file, directory or symlink' % what
    if st.st_uid != 0:
        return '%s: owned by uid %d' % (what, st.st_uid)
    # a symlink's own mode is always 0777 and means nothing: only its owner and the directory it sits in count
    if not stat.S_ISLNK(st.st_mode) and st.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
        return '%s: writable by group or others (%o)' % (what, stat.S_IMODE(st.st_mode))
    return None


def _walk(path, lstat, readlink, max_links=40):
    """every path the kernel looks at to open path, in order, with its lstat result: each directory entered and
    each symlink met (then its target, followed the same way, '..' taken from where the walk really is, not from
    the spelling). Ends at the file itself; raises OSError (with the path) where the kernel would fail."""
    if not str(path):
        raise FileNotFoundError(errno.ENOENT, 'empty path', '')    # as the kernel: '' is no path, not the cwd
    todo = [p for p in str(path).split('/')][::-1] if str(path).startswith('/') else \
        (os.getcwd().split('/') + str(path).split('/'))[::-1]
    here, links = PurePosixPath('/'), 0
    yield here, lstat(here)
    while todo:
        part = todo.pop()
        if part in ('', '.'):
            continue
        if part == '..':
            here = here.parent      # here is a real directory, so its parent is too
            continue
        nxt = here / part
        st = lstat(nxt)
        yield nxt, st
        if stat.S_ISLNK(st.st_mode):
            links += 1
            if links > max_links:
                raise OSError(errno.ELOOP, 'too many symlinks', str(nxt))
            target = readlink(nxt)
            todo.extend(target.split('/')[::-1])
            if target.startswith('/'):
                here = PurePosixPath('/')
        else:
            if todo and not stat.S_ISDIR(st.st_mode):     # more to walk, even a trailing / or /.: ENOTDIR
                raise OSError(errno.ENOTDIR, 'not a directory', str(nxt))
            here = nxt


def untrusted(paths, lstat=os.lstat, readlink=os.readlink, files_only=False):
    """the problems with paths (files the daemon loads): everything the kernel passes through to open each one
    (every directory entered, every symlink met and where it points) must be root's and not writable by others;
    with files_only, also a regular file, a directory or a symlink (not for the loaded-files audit: a process
    maps device files)"""
    out, seen = [], set()
    for p in paths:
        try:
            for q, st in _walk(p, lstat, readlink):
                if q in seen:
                    continue
                seen.add(q)
                problem = _problem(st, str(q), files_only)
                if problem:
                    out.append(problem)
        except FileNotFoundError as e:
            out.append('%s: missing' % (e.filename or p))
        except OSError as e:        # not a directory, a loop, ...: never a pass
            out.append('%s: %s' % (e.filename or p, e.strerror or e))
    return out


def tree_untrusted(roots, lstat=os.lstat, readlink=os.readlink):
    """the problems with whole install trees: the roots and everything under them, each checked by untrusted().
    For the check before the daemon imports anything (the unit's ExecStartPre), so it uses the stdlib only. A
    symlinked directory is not entered by the walk; its target is walked too unless it lies inside a root."""
    out, paths, walked = [], [str(r) for r in roots], []
    todo = []
    for r in roots:     # strict: a root that doesn't resolve ('', file/.., missing) is walked nowhere; untrusted says why
        try:
            if str(r):      # realpath('', strict=True) is the cwd
                os.stat(r)  # the kernel refuses file/.. (ENOTDIR); realpath's strict does only from Python 3.13
                todo.append(os.path.realpath(r, strict=True))
        except OSError:
            pass
    inside = lambda p: any(p == w or p.startswith(w.rstrip('/') + '/') for w in walked)
    while todo:
        r = todo.pop(0)
        if inside(r) or not os.path.isdir(r):
            continue
        walked.append(r)
        for top, dirs, files in os.walk(r, followlinks=False,
                                        onerror=lambda e: out.append('%s: %s' % (e.filename, e.strerror or e))):
            for n in dirs + files:
                p = os.path.join(top, n)
                paths.append(p)
                if os.path.islink(p) and os.path.isdir(p):
                    todo.append(os.path.realpath(p))
    return out + untrusted(paths, lstat, readlink, files_only=True)   # roots, entries and link targets alike


def run_dir_untrusted(path):
    """the problems with the daemon's run dir: root makes and chowns files in it, so a user-writable one (or one
    under a user's symlink) would let a client race it. lstat only; a missing dir is checked by its parent, and only
    a missing one: anything else lstat can't reach (file/.., a loop) is refused, never resolved around"""
    try:
        os.lstat(path)
        return untrusted([path])
    except FileNotFoundError:
        return untrusted([os.path.dirname(os.path.abspath(path)) or '/'])
    except OSError as e:
        return ['%s: %s' % (path, e.strerror or e)]


def loaded_files(extra=()):
    """the files this process has loaded code from (every imported module's .py and .pyc, the extension modules
    and shared objects mapped in), plus extra (graphs, the heads file's source)"""
    files = set()
    for m in list(sys.modules.values()):
        # __cached__: the .pyc Python actually ran (read, not mapped, so not in /proc/self/maps), when one exists
        f, cached = getattr(m, '__file__', None), getattr(m, '__cached__', None)
        if f:
            files.add(f)            # always: one untrusted() can't open (a zip member, say) is reported, not skipped
        if cached and os.path.exists(cached):
            files.add(cached)
    try:
        with open('/proc/self/maps') as f:
            for line in f:
                part = line.split(None, 5)
                if len(part) == 6 and part[5].startswith('/') and ' (deleted)' not in part[5]:
                    files.add(part[5].strip())
    except OSError:
        pass
    files.update(map(str, extra))
    files.add(sys.executable)          # as given: a venv's bin/python is a symlink, and untrusted() walks both
    return sorted(files)


if __name__ == '__main__':
    # python -I -s trust.py ROOT...: exit 1, naming each problem, unless every root is root-only all the way down
    bad = tree_untrusted(sys.argv[1:]) if len(sys.argv) > 1 else ['usage: trust.py ROOT...']
    for b in bad:
        print('ft-npud preflight: %s' % b, file=sys.stderr)
    sys.exit(1 if bad else 0)
