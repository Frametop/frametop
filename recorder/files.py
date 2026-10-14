"""The recorders' file bookkeeping: deleted ranges, checksums, sizes, and the Cancelled an
export raises. hands/rec/takes.py and gaze/rec/takes.py both use these.

Standard library only."""
import hashlib
import json
import os
import shutil

CHUNK = 1 << 20


class Cancelled(Exception):
    pass


def read_json(path, default=None):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {} if default is None else default


def in_ranges(t, ranges):
    return any(a <= t <= b for a, b in ranges)


def merge_ranges(ranges):
    """Sorted, with overlapping or touching ranges joined."""
    out = []
    for a, b in sorted((min(a, b), max(a, b)) for a, b in ranges):
        if out and a <= out[-1][1] + 1:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return out


def sha256_file(path, cancelled=lambda: False):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(CHUNK):
            if cancelled():
                raise Cancelled()
            h.update(chunk)
    return h.hexdigest()


def write_sums(work, cancelled=lambda: False):
    """SHA256SUMS over every file under work, sorted by path."""
    sums = []
    for root, _, files in os.walk(work):
        for name in files:
            path = os.path.join(root, name)
            sums.append((os.path.relpath(path, work), sha256_file(path, cancelled)))
    with open(os.path.join(work, "SHA256SUMS"), "w") as f:
        for rel, digest in sorted(sums):
            f.write(f"{digest}  {rel}\n")


def free_bytes(path):
    """Free space on the file system that holds path (or its nearest existing parent)."""
    while path and not os.path.exists(path):
        path = os.path.dirname(path)
    try:
        return shutil.disk_usage(path or "/").free
    except OSError:
        return 0


def tree_stat(path):
    """(bytes, newest mtime) of the files under path."""
    total, newest = 0, 0.0
    for root, _, files in os.walk(path):
        for name in files:
            try:
                st = os.lstat(os.path.join(root, name))
            except OSError:
                continue
            total += st.st_size
            newest = max(newest, st.st_mtime)
    return total, newest


def tree_bytes(path):
    return tree_stat(path)[0]


def human_bytes(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1000 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1000
    return f"{n:.1f} TB"
