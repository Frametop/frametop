#!/usr/bin/env python3
"""The hand recorder's sessions and takes on disk: listing, review, deleting, export.

Standard library only (no Qt, no NumPy), so the window and the command line share it.
The layout is hands/rec/DESIGN.md's "Files":
  BASE/profile.json
  BASE/contributions.json  (finished uploads, kept after their sessions go: recorder/contributions.py)
  BASE/sessions/<YYYYMMDD-HHMMSS>/session.json, calibration.json
  BASE/sessions/<id>/takes/<NN>-<section>/sets.bin (sets-2.bin, ... after pauses),
                                          prompts.jsonl, poses.jsonl, take.json
  BASE/exports/<session>/
A take's recording is one or more FHSET01 files (hands/track/record.h): per set a header
(magic, ncams, bytes), one fh_set_cam_t per camera, then each camera's 8-bit pixels.
Deleted ranges live in take.json ("deleted": [[from_ns, to_ns], ...], CLOCK_MONOTONIC, the
clock of dqbuf_ns); the files keep every set until export leaves them out.

Side cameras (sides.py): a part recorded before the live tracker decided which side camera is
which may carry slam_left's and slam_right's names the wrong way round. TakeIndex renames them as
it reads (session.json "sides" against take.json "parts"), so review and export see the right
names, and export writes them so: an export's files are always named right, its take.json says
"parts": {"sets.bin": {"names_swapped": <the session's swapped>}}, and its manifest has each
take's "sides". `takes.py sides SESSION` shows the decision; --set swapped|named records one by
hand (e.g. from tools/check_sides.py on a session no live tracker decided).

usage: takes.py [--base DIR] list | takes SESSION | export SESSION | sides SESSION [--set swapped|named]
"""
import argparse
import datetime
import errno
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sides  # noqa: E402  (hands/rec/sides.py, next to this file)
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from recorder import age  # noqa: E402  (a debug session's recordings are encrypted)
from recorder import contributions  # noqa: E402  (the log of uploads that outlives the sessions)
from recorder.common import tree_version  # noqa: E402
import eventlog  # noqa: E402  (hands/rec/eventlog.py: a debug session's panel and pointer decisions)
from recorder.files import (Cancelled, free_bytes, human_bytes, in_ranges, merge_ranges, read_json,  # noqa: E402,F401
                            sha256_file, tree_bytes, tree_stat, write_sums)

DEFAULT_BASE = os.path.expanduser("~/.local/share/frametop/hands/contrib")
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MAGIC = b"FHSET01"
HDR = struct.Struct("<8sII")       # fh_set_hdr_t: magic, ncams, bytes (the whole record)
CAM = struct.Struct("<16sIIQQ")    # fh_set_cam_t: name, width, height, capture_ns, dqbuf_ns
MAX_CAMS = 16                      # SetReader's limit
PART_RE = re.compile(r"^sets(?:-(\d+))?\.bin$")
LIVE_RE = re.compile(r"^live(?:-(\d+))?\.bin$")    # a debug session's copies of the live tracker's output
LIVE_HDR = struct.Struct("<8sII")                   # livelog.py: magic, version, 0
LIVE_REC = struct.Struct("<B3xIQQ")                 # source, length, read_ns, seq
VOICE_RE = re.compile(r"^voice(?:-(\d+))?\.bin$")  # a debug session's microphone, if ticked
VOICE_HDR = struct.Struct("<8sII")                  # voicelog.py: magic, version, sample rate
VOICE_REC = struct.Struct("<QI")                    # read_ns, bytes of s16le samples
EVENTS_RE = re.compile(r"^events(?:-(\d+))?\.jsonl$")  # a debug session's panel and pointer decisions
TEST_RECIPIENTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test-recipients.txt")
# The bounties' test phase (DESIGN.md "Testing"): while it's on, bounty rounds are recorded under
# TESTING.md's consent (profile.json "testing_consent") like debug sessions, and their exports are
# encrypted for the test dataset. It ended 2026-10-14: bounty rounds go under CONSENT.md to the public
# dataset, like full sessions and quick rounds; only debug (testing) sessions stay under TESTING.md.
TEST_PHASE = False
CONSENT_KEYS = {"public": "consent", "testing": "testing_consent"}   # profile.json's record of each


def round_consent_kind(bounty=False, debug=False):
    """Which consent a new session is recorded under: "testing" for a debug session, and for a
    bounty's round in the test phase; else "public"."""
    return "testing" if debug or (bounty and TEST_PHASE) else "public"


def consent_kind(meta):
    """The consent a session was recorded under (session.json "consent", session.py): "testing"
    or "public" (sessions from before it was recorded, and full sessions)."""
    kind = (meta.get("consent") or {}).get("kind") if isinstance(meta.get("consent"), dict) else None
    return "testing" if kind == "testing" or meta.get("debug") else "public"
SESSION_RE = re.compile(r"^\d{8}-\d{6}(?:-\d+)?$")   # session.py adds -2, -3 to a second one in a second
EXPORT_SCHEMA = 1
# zstd as DESIGN.md has it: level 10, two threads, at the lowest CPU priority (CPU work while
# someone is in VR makes the headset stutter).
ZSTD_ARGS = ["-10", "-T2", "-q", "-c"]
ZSTD_PATHS = ("/usr/bin/zstd", "/usr/local/bin/zstd", "/run/host/usr/bin/zstd")
# An export is about three quarters of the recording's size (2026-10-17, the Frame's five
# exports: 0.72 to 0.76). Export starts only with room for EXPORT_RATIO_NEED of it plus
# EXPORT_RESERVE, and stops when the disk gets down to EXPORT_MIN_FREE: a full home disk breaks
# more than the export (SteamVR, Steam and the desktop write there too). Until 2026-10-17 it
# started with half free and ran until zstd failed on a full disk.
EXPORT_RATIO = 0.75
EXPORT_RATIO_NEED = 0.8
EXPORT_RESERVE = 2 * 1000 ** 3
EXPORT_MIN_FREE = 1.5e9       # as session.py's MIN_FREE for recording
ROOM_CHECK_S = 1.0


class ZstdStopped(RuntimeError):
    """zstd ended before the export was through with it. Writing into its pipe then fails with a
    broken pipe, which says nothing: this carries what zstd said (a full disk, mostly)."""


class DiskFull(RuntimeError):
    """Not enough free space for the export, before it started or as it went."""


class NearlyFull(OSError):
    """The export stopped itself: the disk got down to EXPORT_MIN_FREE."""

    def __init__(self, free):
        super().__init__(errno.ENOSPC, f"only {human_bytes(free)} left on the disk")


def export_need(kept):
    """The free space an export of kept bytes of recording needs to start."""
    return int(kept * EXPORT_RATIO_NEED + EXPORT_RESERVE)


_room_checked = [0.0]   # when check_room last looked (monotonic s)


def check_room(path):
    """Raise NearlyFull when the disk holding path is down to EXPORT_MIN_FREE. Looks at most once
    every ROOM_CHECK_S (a statvfs per set would cost more than it saves)."""
    now = time.monotonic()
    if now - _room_checked[0] < ROOM_CHECK_S:
        return
    _room_checked[0] = now
    free = free_bytes(path)
    if 0 < free < EXPORT_MIN_FREE:
        raise NearlyFull(free)


def disk_full(e):
    """True if e (an export's error) or what it came from is a full disk or a quota."""
    seen = set()
    while e is not None and id(e) not in seen:
        seen.add(id(e))
        if isinstance(e, OSError) and e.errno in (errno.ENOSPC, errno.EDQUOT, errno.EFBIG):
            return True
        if isinstance(e, ZstdStopped) and any(s in str(e).lower() for s in ("no space left", "quota exceeded",
                                                                           "file too large")):
            return True
        e = e.__cause__ or e.__context__
    return False


def write_json(path, obj):
    """Write through a temporary file, so a crash never leaves half a file."""
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)


def find_zstd():
    """The zstd binary, or None. The dev container gets it from setup/dev-container.sh."""
    found = shutil.which("zstd")
    if found:
        return found
    return next((p for p in ZSTD_PATHS if os.access(p, os.X_OK)), None)


def standalone(path=None):
    """The standalone Hand Recorder's build info, or None in a Frametop checkout. Its release
    (github.com/Frametop/frametop-hand-recorder) ships this repo's tree with standalone.json at
    the top: {"name", "version", "frametop": this repo's git describe, "reinstall": how to repair
    an install}. Its binaries are built for the SteamOS host, so nothing runs in the dev container."""
    try:
        with open(path or os.path.join(REPO, "standalone.json")) as f:
            info = json.load(f)
    except (OSError, ValueError):
        return None
    return info if isinstance(info, dict) else None


def tool_version():
    """ft-handrec plus the tree's version (a checkout's git describe, a release's version and
    commit), for session.json and the manifest. The standalone Hand Recorder has neither: the
    describe it was built from, and its version."""
    info = standalone()
    if info:
        return "ft-handrec %s (%s %s)" % (info.get("frametop") or "unknown", info.get("name") or "standalone",
                                          info.get("version") or "unknown")
    return "ft-handrec " + (tree_version(REPO) or "unknown")


def export_jsonl(src, dst, ranges, keep_controllers):
    """Copy a take's poses.jsonl or prompts.jsonl for export:
    - poses in the deleted ranges are left out, and the live tracker's feedback there (the
      prompt timeline stays: it says what was asked, and shows nothing);
    - without controllers (the checklist's "none"), the controllers' poses are null and feedback
      carries no controller state: controllers lying about still get logged, and those poses
      would read as hands' ground truth.
    Lines that don't parse are copied as they are (validate.py reports them)."""
    poses = os.path.basename(src) == "poses.jsonl"
    with open(src, encoding="utf-8") as f, open(dst, "w", encoding="utf-8", newline="\n") as out:
        for line in f:
            try:
                obj = json.loads(line)
            except ValueError:
                out.write(line)
                continue
            if not isinstance(obj, dict):
                out.write(line)
                continue
            t = obj.get("t")
            gone = isinstance(t, int) and in_ranges(t, ranges)
            if poses:
                if gone:
                    continue
                if not keep_controllers:
                    obj["left"] = obj["right"] = None
            elif obj.get("event") == "feedback":
                if gone:
                    continue
                if not keep_controllers:
                    obj.pop("controller", None)
            out.write(json.dumps(obj, separators=(", ", ": ")) + "\n")


def live_parts(take_dir, pattern=LIVE_RE):
    """A debug take's live.bin, live-2.bin, ... in order (pattern=VOICE_RE: voice.bin, ...)."""
    try:
        names = os.listdir(take_dir)
    except OSError:
        return []
    found = [(int(m.group(1) or 1), name) for name in names for m in [pattern.match(name)] if m]
    return [os.path.join(take_dir, name) for _, name in sorted(found)]


def export_live(take_dir, ranges, zstd, dest, recipients=None):
    """A debug take's live parts as one live.bin (livelog.py's format), records in deleted ranges
    left out, through zstd into dest, encrypted to recipients if given. None without live
    parts; else {"records": {"hands": n, "gestures": n}, "raw_bytes": n}."""
    parts = live_parts(take_dir)
    if not parts:
        return None
    out = bytearray(LIVE_HDR.pack(b"FHLIVE01", 1, 0))
    counts = {"hands": 0, "gestures": 0}
    for path in parts:
        with open(path, "rb") as f:
            data = f.read()
        if len(data) < LIVE_HDR.size or data[:8] != b"FHLIVE01":
            continue
        off = LIVE_HDR.size
        while off + LIVE_REC.size <= len(data):
            src, n, read_ns, _ = LIVE_REC.unpack_from(data, off)
            end = off + LIVE_REC.size + n
            if end > len(data) or src > 1:
                break   # cut off: the session ended mid-write
            if not in_ranges(read_ns, ranges):
                out += data[off:end]
                counts["hands" if src == 0 else "gestures"] += 1
            off = end
    r = subprocess.run(["nice", "-n", "19", zstd] + ZSTD_ARGS, input=bytes(out), capture_output=True)
    if r.returncode != 0:
        raise RuntimeError("zstd failed: %s" % (r.stderr.decode(errors="replace").strip() or r.returncode))
    payload = age.encrypt_bytes(r.stdout, recipients) if recipients else r.stdout
    with open(dest, "wb") as f:
        f.write(payload)
    return {"records": counts, "raw_bytes": len(out)}


def export_voice(take_dir, ranges, zstd, dest, recipients):
    """A debug take's voice parts as one voice.bin (voicelog.py's format), chunks touching a
    deleted range left out, through zstd into dest, always encrypted (no recipients: nothing
    written, None). None without voice parts; else {"chunks": n, "seconds": s, "raw_bytes": n}."""
    parts = live_parts(take_dir, VOICE_RE)
    if not parts or not recipients:
        return None
    out, rate, chunks, samples = bytearray(), None, 0, 0
    for path in parts:
        with open(path, "rb") as f:
            data = f.read()
        if len(data) < VOICE_HDR.size or data[:8] != b"FHVOIC01":
            continue
        _, _, part_rate = VOICE_HDR.unpack_from(data, 0)
        if rate is None:
            rate = part_rate
            out += VOICE_HDR.pack(b"FHVOIC01", 1, rate)
        elif part_rate != rate:
            continue
        off = VOICE_HDR.size
        while off + VOICE_REC.size <= len(data):
            read_ns, n = VOICE_REC.unpack_from(data, off)
            end = off + VOICE_REC.size + n
            if end > len(data):
                break   # cut off: the session ended mid-write
            first_ns = read_ns - n // 2 * 1_000_000_000 // max(1, rate)
            if not any(a <= read_ns and first_ns <= b for a, b in ranges):
                out += data[off:end]
                chunks += 1
                samples += n // 2
            off = end
    if rate is None:
        return None
    r = subprocess.run(["nice", "-n", "19", zstd] + ZSTD_ARGS, input=bytes(out), capture_output=True)
    if r.returncode != 0:
        raise RuntimeError("zstd failed: %s" % (r.stderr.decode(errors="replace").strip() or r.returncode))
    with open(dest, "wb") as f:
        f.write(age.encrypt_bytes(r.stdout, recipients))
    return {"chunks": chunks, "seconds": round(samples / rate, 2), "raw_bytes": len(out)}


def export_events(take_dir, ranges, zstd, dest, recipients):
    """A debug take's events parts (eventlog.py) as one events.jsonl, events in deleted ranges left
    out, through zstd into dest, always encrypted (no recipients: nothing written, None). None
    without events parts; else {"events": n, "by": {kind: {outcome: n}}, "raw_bytes": n}."""
    parts = live_parts(take_dir, EVENTS_RE)
    if not parts or not recipients:
        return None
    events = [e for path in parts for e in eventlog.read_lines(path) if not in_ranges(e["t"], ranges)]
    out = "".join(json.dumps(e, separators=(", ", ": ")) + "\n" for e in events).encode()
    r = subprocess.run(["nice", "-n", "19", zstd] + ZSTD_ARGS, input=out, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError("zstd failed: %s" % (r.stderr.decode(errors="replace").strip() or r.returncode))
    with open(dest, "wb") as f:
        f.write(age.encrypt_bytes(r.stdout, recipients))
    return {"events": len(events), "by": eventlog.summary(events), "raw_bytes": len(out)}


# ---------------------------------------------------------------- one take's recording

class TakeIndex:
    """Where each set of a take's recording is, without reading pixels. Sets are numbered
    across the parts (sets.bin, sets-2.bin, ...) in order."""

    def __init__(self, take_dir):
        self.dir = take_dir
        self.parts = self._parts(take_dir)
        self.sets = []   # (part, offset, bytes, time_ns): time_ns is the cameras' earliest dqbuf_ns
        self.part_starts = []   # index of each part's first set
        self.cams = []   # the first set's [{name, width, height}], side cameras named right
        # per part: are its side cameras named the wrong way round (sides.py)? read_set and
        # read_raw rename them.
        session = read_json(os.path.join(os.path.dirname(os.path.dirname(take_dir)), "session.json"))
        take = read_json(os.path.join(take_dir, "take.json"))
        self.swapped = sides.session_sides(session).get("swapped")
        self.names_swapped = [sides.part_names_swapped(take, os.path.basename(p)) for p in self.parts]
        self.rename = [sides.needs_rename(self.swapped, n) for n in self.names_swapped]
        for p, path in enumerate(self.parts):
            self.part_starts.append(len(self.sets))
            self._index(p, path)

    @staticmethod
    def _parts(take_dir):
        found = []
        try:
            names = os.listdir(take_dir)
        except OSError:
            return []
        for name in names:
            m = PART_RE.match(name)
            if m:
                found.append((int(m.group(1) or 1), os.path.join(take_dir, name)))
        return [path for _, path in sorted(found)]

    def _index(self, p, path):
        try:
            size = os.path.getsize(path)
            f = open(path, "rb")
        except OSError:
            return
        with f:
            off = 0
            while off + HDR.size <= size:
                f.seek(off)
                head = f.read(HDR.size)
                if len(head) < HDR.size:
                    break
                magic, ncams, nbytes = HDR.unpack(head)
                # A truncated last set (the recorder was killed mid-write) ends the part.
                if magic[:7] != MAGIC or not 0 < ncams <= MAX_CAMS or nbytes < HDR.size or off + nbytes > size:
                    break
                cams = f.read(CAM.size * ncams)
                if len(cams) < CAM.size * ncams:
                    break
                fields = [CAM.unpack_from(cams, i * CAM.size) for i in range(ncams)]
                if not self.cams:
                    self.cams = [{"name": self._name(p, n), "width": w, "height": h} for n, w, h, _, _ in fields]
                self.sets.append((p, off, nbytes, min(c[4] for c in fields)))
                off += nbytes

    def _name(self, p, raw):
        name = raw.split(b"\0", 1)[0].decode(errors="replace")
        return sides.other_name(name) if self.rename[p] else name

    def renamed_sets(self, keep=None):
        """How many of the sets (all, or the indices in keep) read renamed."""
        return sum(1 for i in (range(len(self.sets)) if keep is None else keep) if self.rename[self.sets[i][0]])

    def __len__(self):
        return len(self.sets)

    def time_ns(self, i):
        return self.sets[i][3]

    def duration_s(self):
        """Recorded time: each part's first to last set, so pauses don't count."""
        total = 0
        ends = self.part_starts[1:] + [len(self.sets)]
        for start, end in zip(self.part_starts, ends):
            if end > start:
                total += self.sets[end - 1][3] - self.sets[start][3]
        return total / 1e9

    def bytes(self):
        return sum(s[2] for s in self.sets)

    def read_set(self, i, only=None):
        """Set i's cameras: [{name, width, height, capture_ns, dqbuf_ns, pixels}], pixels as raw
        8-bit grey bytes (width x height, packed). only: a camera name, to read just that one."""
        p, off, nbytes, _ = self.sets[i]
        with open(self.parts[p], "rb") as f:
            f.seek(off)
            ncams = HDR.unpack(f.read(HDR.size))[1]
            heads = f.read(CAM.size * ncams)
            out = []
            px_off = off + HDR.size + CAM.size * ncams
            for k in range(ncams):
                name, w, h, capture_ns, dqbuf_ns = CAM.unpack_from(heads, k * CAM.size)
                name = self._name(p, name)
                if only is None or name == only:
                    f.seek(px_off)
                    out.append({"name": name, "width": w, "height": h, "capture_ns": capture_ns,
                                "dqbuf_ns": dqbuf_ns, "pixels": f.read(w * h)})
                px_off += w * h
            return out

    def read_raw(self, i):
        """Set i's whole record, as it is in the file but with the side cameras named right."""
        p, off, nbytes, _ = self.sets[i]
        with open(self.parts[p], "rb") as f:
            f.seek(off)
            record = f.read(nbytes)
        return sides.rename_record(record) if self.rename[p] else record


_index_cache = {}
_index_lock = threading.Lock()


def take_index(take_dir):
    """A TakeIndex, cached until one of the take's recording files, its take.json or its
    session.json (the side cameras' decision) changes size or time."""
    key_parts = []
    meta = [os.path.join(take_dir, "take.json"), os.path.join(os.path.dirname(os.path.dirname(take_dir)), "session.json")]
    for path in TakeIndex._parts(take_dir) + meta:
        try:
            st = os.stat(path)
            key_parts.append((path, st.st_size, st.st_mtime_ns))
        except OSError:
            pass
    key = tuple(key_parts)
    with _index_lock:
        cached = _index_cache.get(take_dir)
        if cached and cached[0] == key:
            return cached[1]
    index = TakeIndex(take_dir)
    with _index_lock:
        _index_cache[take_dir] = (key, index)
    return index


def export_sha(export_dir):
    """The export's identity, as hub.py records it: the SHA256 of its SHA256SUMS ("" if none)."""
    try:
        with open(os.path.join(export_dir, "SHA256SUMS"), "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()
    except OSError:
        return ""


def _epoch(iso):
    try:
        return datetime.datetime.fromisoformat(iso).timestamp()
    except (TypeError, ValueError):
        return None


def upload_state(meta, changed, export_dir):
    """A session's uploads, from its session.json (meta) and its files' newest change (changed,
    tree_stat's): {"uploaded": a finished upload exists, "pr_url": the last one's, "freeable":
    deleting the local copy loses nothing that wasn't uploaded}. That's when the recordings haven't
    changed since the last finished upload, or (an upload without a readable time) the export
    there now is the one uploaded."""
    done = contributions.done_uploads(meta)
    if not done:
        return {"uploaded": False, "pr_url": "", "freeable": False}
    last = done[-1]
    when = _epoch(last.get("uploaded", ""))
    if when is not None:
        freeable = changed <= when
    else:
        sha = export_sha(export_dir)
        freeable = bool(sha) and any(u.get("export_sha") == sha for u in done)
    return {"uploaded": True, "pr_url": last.get("pr_url", ""), "freeable": freeable}


# ---------------------------------------------------------------- the store

class Store:
    def __init__(self, base=DEFAULT_BASE):
        self.base = os.path.abspath(os.path.expanduser(base))
        self.sessions_dir = os.path.join(self.base, "sessions")
        self.exports_dir = os.path.join(self.base, "exports")
        self.profile_path = os.path.join(self.base, "profile.json")

    # --- paths, checked: ids come from the window, so nothing may climb out of the base
    def session_dir(self, session):
        if not SESSION_RE.match(session or ""):
            raise ValueError(f"not a session id: {session!r}")
        return os.path.join(self.sessions_dir, session)

    def take_dir(self, session, take):
        if not take or "/" in take or take.startswith("."):
            raise ValueError(f"not a take id: {take!r}")
        return os.path.join(self.session_dir(session), "takes", take)

    def export_dir(self, session):
        self.session_dir(session)  # checks the id
        return os.path.join(self.exports_dir, session)

    def profile(self):
        return read_json(self.profile_path)

    # --- listing
    def sessions(self):
        """Newest first: [{id, started, takes, bytes, lighting, status, dry_run, contributor, exported,
        export_stale (changed since), export_bytes, uploaded, pr_url, freeable}] (upload_state's
        last three)."""
        out = []
        try:
            names = os.listdir(self.sessions_dir)
        except OSError:
            return out
        for sid in names:
            path = os.path.join(self.sessions_dir, sid)
            if not SESSION_RE.match(sid) or not os.path.isdir(path):
                continue
            meta = read_json(os.path.join(path, "session.json"))
            size, changed = tree_stat(path)
            manifest = os.path.join(self.export_dir(sid), "manifest.json")
            exported = os.path.getmtime(manifest) if os.path.isfile(manifest) else 0.0
            out.append({"id": sid, "started": meta.get("started", ""), "takes": len(self.take_ids(sid)),
                        "bytes": size, "contributor": meta.get("contributor", ""),
                        "lighting": (meta.get("lighting") or {}).get("chosen", ""),
                        "status": meta.get("status", ""), "dry_run": bool(meta.get("dry_run")),
                        "bounty": ((meta.get("bounty") or {}).get("title") or "") if isinstance(meta.get("bounty"), dict) else "",
                        "debug": bool(meta.get("debug")), "testing": consent_kind(meta) == "testing",
                        "exported": bool(exported), "export_stale": bool(exported) and changed > exported,
                        "export_bytes": tree_bytes(self.export_dir(sid)) if exported else 0,
                        **upload_state(meta, changed, self.export_dir(sid))})
        return sorted(out, key=lambda s: s["id"], reverse=True)

    def take_ids(self, session):
        try:
            names = os.listdir(os.path.join(self.session_dir(session), "takes"))
        except OSError:
            return []
        return sorted(n for n in names if not n.startswith(".")
                      and os.path.isdir(os.path.join(self.session_dir(session), "takes", n)))

    def takes(self, session):
        """[{id, section, title, status, sets, deleted_sets, ranges, duration_s, bytes, cams}]."""
        out = []
        for tid in self.take_ids(session):
            path = self.take_dir(session, tid)
            meta = read_json(os.path.join(path, "take.json"))
            index = take_index(path)
            ranges = merge_ranges(meta.get("deleted") or [])
            out.append({"id": tid, "section": meta.get("section", tid.split("-", 1)[-1]),
                        "title": meta.get("title") or tid, "status": meta.get("status", "recording"),
                        "sets": len(index), "parts": len(index.parts),
                        "deleted_sets": sum(1 for s in index.sets if in_ranges(s[3], ranges)),
                        "ranges": ranges, "duration_s": index.duration_s(), "bytes": index.bytes(),
                        "cams": [c["name"] for c in index.cams]})
        return out

    def take_meta(self, session, take):
        return read_json(os.path.join(self.take_dir(session, take), "take.json"))

    # --- deleted ranges
    def ranges(self, session, take):
        return merge_ranges(self.take_meta(session, take).get("deleted") or [])

    def _set_ranges(self, session, take, ranges):
        path = os.path.join(self.take_dir(session, take), "take.json")
        meta = read_json(path)
        meta["deleted"] = merge_ranges(ranges)
        write_json(path, meta)
        return meta["deleted"]

    def delete_range(self, session, take, from_ns, to_ns):
        """Leave sets from from_ns to to_ns (dqbuf_ns, inclusive) out of the export."""
        return self._set_ranges(session, take, self.ranges(session, take) + [[int(from_ns), int(to_ns)]])

    def restore_range(self, session, take, i):
        """Take deleted range i (in ranges()' order) back."""
        ranges = self.ranges(session, take)
        if 0 <= i < len(ranges):
            del ranges[i]
        return self._set_ranges(session, take, ranges)

    # --- deleting files
    def delete_take(self, session, take):
        path = self.take_dir(session, take)
        if os.path.isdir(path):
            shutil.rmtree(path)
        meta_path = os.path.join(self.session_dir(session), "session.json")
        meta = read_json(meta_path)
        if take in (meta.get("takes") or []):
            meta["takes"] = [t for t in meta["takes"] if t != take]
            meta.setdefault("deleted_takes", []).append(take)
            write_json(meta_path, meta)

    def delete_session(self, session):
        """The session and its export, if any."""
        for path in (self.session_dir(session), self.export_dir(session)):
            if os.path.isdir(path):
                shutil.rmtree(path)

    def delete_export(self, session):
        path = self.export_dir(session)
        if os.path.isdir(path):
            shutil.rmtree(path)

    # --- freeing space: uploaded sessions go, their record stays (recorder/contributions.py)
    def log_uploads(self, session=None):
        """Copy finished uploads from session.json into the contributions log: one session's, or
        every session's (uploads older recorders recorded only there). Returns how many were new."""
        items = []
        for sid in [session] if session else [s for s in os.listdir(self.sessions_dir)
                                              if SESSION_RE.match(s)] if os.path.isdir(self.sessions_dir) else []:
            meta = read_json(os.path.join(self.session_dir(sid), "session.json"))
            export = self.export_dir(sid)
            size = tree_bytes(export) if os.path.isdir(export) else 0
            items += [contributions.entry(sid, meta, u, size if u.get("export_sha") == export_sha(export) else 0)
                      for u in contributions.done_uploads(meta)]
        return contributions.add(self.base, items) if items else 0

    def free_session(self, session):
        """Delete an uploaded session's recordings and export, keeping its uploads in the
        contributions log. Refuses (ValueError) a session that isn't uploaded as it is now.
        Returns the bytes freed."""
        path = self.session_dir(session)
        meta = read_json(os.path.join(path, "session.json"))
        if not meta:
            raise ValueError(f"no session {session}")
        _, changed = tree_stat(path)
        state = upload_state(meta, changed, self.export_dir(session))
        if not state["freeable"]:
            raise ValueError("This session isn't uploaded as it is now: upload it first, or delete it")
        freed = tree_bytes(path) + (tree_bytes(self.export_dir(session)) if os.path.isdir(self.export_dir(session)) else 0)
        self.log_uploads(session)   # the record first: if it can't be written, nothing is deleted
        if session not in contributions.sessions(self.base):
            raise OSError(f"couldn't keep the record of {session} in {contributions.FILE}")
        self.delete_session(session)
        contributions.mark_deleted(self.base, session)
        return freed

    def freeable(self):
        """The sessions free_session would take: [{id, bytes (recordings + export)}], newest first."""
        return [{"id": s["id"], "bytes": s["bytes"] + s["export_bytes"]} for s in self.sessions()
                if s["freeable"] and s["status"] != "recording"]

    def contributions(self):
        """The contributions log, newest first, each with "local": its session is still here."""
        out = []
        for c in reversed(contributions.load(self.base)):
            local = bool(SESSION_RE.match(c["session"])) and os.path.isdir(os.path.join(self.sessions_dir, c["session"]))
            out.append(dict(c, local=local))
        return out

    # --- export
    def sides(self, session, swapped=None):
        """session.json's side camera decision (sides.py) plus each take's parts; with swapped
        (True/False), record that by hand first ("decided_by": "manual"). No set is rewritten:
        reading and export rename."""
        path = os.path.join(self.session_dir(session), "session.json")
        meta = read_json(path)
        if not meta:
            raise RuntimeError(f"no session {session}")
        if swapped is not None:
            old = sides.session_sides(meta)
            new = {"swapped": bool(swapped), "decided_by": "manual",
                   "decided_at": datetime.datetime.now().astimezone().isoformat(timespec="seconds")}
            if old.get("swapped") is not None:
                new["replaced"] = old
            meta["sides"] = new
            write_json(path, meta)
        out = {"sides": sides.session_sides(meta), "takes": {}}
        for tid in self.take_ids(session):
            index = take_index(self.take_dir(session, tid))
            out["takes"][tid] = {os.path.basename(p): {"names_swapped": n, "renamed_when_read": r}
                                 for p, n, r in zip(index.parts, index.names_swapped, index.rename)}
        return out

    def export(self, session, progress=None, cancel=None, keep_notes=False, low_priority=True):
        """Write exports/<session>/ (DESIGN.md "Export"): manifest.json, calibration.json, per
        take prompts.jsonl, poses.jsonl, take.json and sets.bin.zst (sets in deleted ranges
        left out), then SHA256SUMS. progress(fraction 0..1, text) is called as it goes;
        cancel() (or cancel.is_set()) returning true stops it, raising Cancelled. Nothing is
        left behind on failure or cancel. Returns the export's path.
        low_priority: run this thread at nice 19 as well as zstd (Linux nice is per thread)."""
        zstd = find_zstd()
        if not zstd:
            raise RuntimeError("zstd isn't installed (in the dev container: sudo dnf install zstd)")
        cancelled = getattr(cancel, "is_set", cancel) or (lambda: False)
        report = progress or (lambda fraction, text: None)
        src = self.session_dir(session)
        if not os.path.isdir(src):
            raise RuntimeError(f"no session {session}")
        if low_priority:
            try:
                os.setpriority(os.PRIO_PROCESS, threading.get_native_id(), 19)
            except OSError:
                pass
        final = self.export_dir(session)
        work = final + ".partial"
        if os.path.isdir(work):
            shutil.rmtree(work)
        kept = self.kept_bytes(session)
        free = free_bytes(self.exports_dir)   # 0: couldn't tell, so no check
        if 0 < free < export_need(kept):
            raise DiskFull(self.space_text(session, kept, "Not enough free space to export this session"))
        os.makedirs(work)
        try:
            self._export(session, src, work, zstd, report, cancelled, keep_notes)
            if os.path.isdir(final):
                shutil.rmtree(final)
            os.replace(work, final)
        except BaseException as e:
            shutil.rmtree(work, ignore_errors=True)
            if isinstance(e, NearlyFull):
                raise DiskFull(self.space_text(session, kept, "The disk got nearly full, so the export "
                                                              "stopped and removed its half")) from e
            if isinstance(e, Exception) and not isinstance(e, DiskFull) and disk_full(e):
                raise DiskFull(self.space_text(session, kept, "The disk filled up while exporting")) from e
            raise
        report(1.0, "Done")
        return final

    def kept_bytes(self, session):
        """About how much of the session's recording an export keeps (deleted ranges left out)."""
        return int(sum(t["bytes"] * (t["sets"] - t["deleted_sets"]) / max(1, t["sets"]) for t in self.takes(session)))

    def space_text(self, session, kept, first):
        """The disk-full message: what's free, about what the export needs, and what to do."""
        text = (f"{first}: {human_bytes(free_bytes(self.exports_dir))} free, and the export needs about "
                f"{human_bytes(export_need(kept))} to start. Free some space and export again. Rounds that "
                "are already uploaded can be deleted on the Review page, and your record of them stays.")
        old = tree_bytes(self.export_dir(session)) if os.path.isdir(self.export_dir(session)) else 0
        if old:
            text += f" Its old export ({human_bytes(old)}) stays until the new one is done: deleting it frees that too."
        return text

    def _export(self, session, src, work, zstd, report, cancelled, keep_notes):
        profile = self.profile()
        meta = read_json(os.path.join(src, "session.json"))
        meta.pop("uploads", None)   # earlier uploads' records (hub.py) aren't part of the contribution
        takes = self.takes(session)
        total = sum(t["bytes"] for t in takes) or 1
        done = 0
        take_entries = []
        keep_controllers = (meta.get("checklist") or {}).get("controllers") == "straps"
        # A test-phase session's recordings go to the test dataset encrypted (DESIGN.md "Testing")
        kind = consent_kind(meta)
        recipients = age.read_recipients(TEST_RECIPIENTS) if kind == "testing" else None
        encrypted = []
        for t in takes:
            if cancelled():
                raise Cancelled()
            tdir = self.take_dir(session, t["id"])
            out = os.path.join(work, "takes", t["id"])
            os.makedirs(out)
            ranges = t["ranges"]
            for name in ("prompts.jsonl", "poses.jsonl"):
                if os.path.isfile(os.path.join(tdir, name)):
                    export_jsonl(os.path.join(tdir, name), os.path.join(out, name), ranges, keep_controllers)
            index = take_index(tdir)
            keep = [i for i in range(len(index)) if not in_ranges(index.time_ns(i), ranges)]
            # one file, named right where the decision is known (sides.py): its names_swapped is
            # then the session's swapped; unknown, it's the parts' own (None if they differ)
            if index.swapped is not None:
                names_swapped = bool(index.swapped)
            else:
                names_swapped = index.names_swapped[0] if len(set(index.names_swapped)) == 1 else None
            take_meta = read_json(os.path.join(tdir, "take.json"))
            if take_meta:
                take_meta["parts"] = {"sets.bin": {"names_swapped": names_swapped}}
                write_json(os.path.join(out, "take.json"), take_meta)
            entry = {"id": t["id"], "section": t["section"], "title": t["title"], "status": t["status"],
                     "sets": len(keep), "sets_deleted": len(index) - len(keep), "cameras": index.cams,
                     "duration_s": round(t["duration_s"], 2), "file": None,
                     "sides": {"names_swapped": names_swapped, "renamed_sets": index.renamed_sets(keep)}}
            done += index.bytes() - sum(index.sets[i][2] for i in keep)   # deleted sets count as done
            suffix = ".age" if recipients else ""
            if keep:
                entry["file"] = "takes/%s/sets.bin.zst%s" % (t["id"], suffix)

                def step(nbytes, title=t["title"]):
                    nonlocal done
                    done += nbytes
                    report(min(done / total, 0.99), f"{'Compressing and encrypting' if recipients else 'Compressing'} {title}")
                entry["raw_bytes"] = self._compress(index, keep, zstd, os.path.join(work, entry["file"]),
                                                    step, cancelled, recipients)
                if recipients:
                    encrypted.append(entry["file"])
            live = export_live(tdir, ranges, zstd, os.path.join(out, "live.bin.zst" + suffix), recipients)
            if live is not None:
                entry["live"] = dict(live, file="takes/%s/live.bin.zst%s" % (t["id"], suffix))
                if recipients:
                    encrypted.append(entry["live"]["file"])
            voice = export_voice(tdir, ranges, zstd, os.path.join(out, "voice.bin.zst.age"), recipients)
            if voice is not None:
                entry["voice"] = dict(voice, file="takes/%s/voice.bin.zst.age" % t["id"])
                encrypted.append(entry["voice"]["file"])
            events = export_events(tdir, ranges, zstd, os.path.join(out, "events.jsonl.zst.age"), recipients)
            if events is not None:
                entry["events"] = dict(events, file="takes/%s/events.jsonl.zst.age" % t["id"])
                encrypted.append(entry["events"]["file"])
            take_entries.append(entry)
        if cancelled():
            raise Cancelled()
        for name in ("calibration.json", "device.json"):
            if os.path.isfile(os.path.join(src, name)):
                shutil.copyfile(os.path.join(src, name), os.path.join(work, name))
        # the profile as the dataset sees it: the consent this session was recorded under, not the
        # window's own keys (the bounties done) nor the other consent's record
        shown_profile = json.loads(json.dumps({k: profile[k] for k in ("schema", "contributor", CONSENT_KEYS[kind],
                                                                       "optional") if k in profile}))
        if not keep_notes and isinstance(shown_profile.get("optional"), dict):
            shown_profile["optional"].pop("notes", None)
        manifest = {"schema": EXPORT_SCHEMA, "tool": tool_version(),
                    "exported": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                    "session_id": session, "contributor": profile.get("contributor", meta.get("contributor", "")),
                    "consent_kind": kind,
                    "consent_version": (profile.get(CONSENT_KEYS[kind]) or {}).get("version", ""),
                    "profile": shown_profile, "session": meta, "takes": take_entries}
        if recipients:
            manifest["encryption"] = {"scheme": "age-v1-x25519", "recipients": recipients, "files": encrypted}
        write_json(os.path.join(work, "manifest.json"), manifest)
        report(0.995, "Writing checksums")
        write_sums(work, cancelled)

    @staticmethod
    def _compress(index, keep, zstd, dest, step, cancelled, recipients=None):
        """Stream the kept sets through zstd into dest, encrypted to recipients if given (age,
        as zstd writes). Returns the uncompressed size.
        Pipes (2026-10-15): zstd's messages go to a temporary file, as a pipe read only at the end
        could fill and stop zstd and this thread both. zstd ending early (a full disk, a signal)
        makes the next write into its pipe a broken pipe: ZstdStopped with what zstd said instead.
        The sealer drains what it can't encrypt a piece at a time, not into memory all at once."""
        raw = 0
        with open(dest, "wb") as out, tempfile.TemporaryFile(prefix="handrec-zstd-") as errors:
            proc = subprocess.Popen(["nice", "-n", "19", zstd] + ZSTD_ARGS, stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE if recipients else out, stderr=errors)
            sealer, sealed = None, {}
            if recipients:
                def seal():
                    try:
                        age.encrypt_stream(proc.stdout, out, recipients)
                    except BaseException as e:   # reported below, after zstd
                        sealed["error"] = e
                        try:   # keep zstd from blocking on a full pipe
                            while proc.stdout.read(1 << 20):
                                pass
                        except (OSError, ValueError):
                            pass
                sealer = threading.Thread(target=seal, name="handrec-seal", daemon=True)
                sealer.start()

            def stopped():
                try:
                    code = proc.wait(30)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    code = proc.wait()
                errors.seek(0)
                said = errors.read()[-4000:].decode(errors="replace").strip()
                return ZstdStopped(f"zstd stopped: {said}" if said else f"zstd stopped (exit status {code})")
            try:
                try:
                    for i in keep:
                        if cancelled():
                            raise Cancelled()
                        check_room(dest)
                        record = index.read_raw(i)
                        proc.stdin.write(record)
                        raw += len(record)
                        step(len(record))
                    proc.stdin.close()
                except BrokenPipeError:
                    raise stopped() from None
                if proc.wait() != 0:
                    raise stopped()
                if sealer:
                    sealer.join()
                    if "error" in sealed:
                        raise RuntimeError(f"encrypting failed: {sealed['error']}") from sealed["error"]
            except BaseException:
                proc.kill()
                proc.wait()
                if sealer:
                    sealer.join()   # zstd has gone, so its output ends and the sealer with it
                raise
            finally:
                for pipe in (proc.stdin, proc.stdout):
                    try:
                        if pipe:
                            pipe.close()   # a write cut short leaves bytes that won't flush
                    except OSError:
                        pass
        return raw


def main():
    ap = argparse.ArgumentParser(description="List, check and export hand recorder sessions.")
    ap.add_argument("--base", default=DEFAULT_BASE)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    sub.add_parser("takes").add_argument("session")
    sub.add_parser("export").add_argument("session")
    sp = sub.add_parser("sides", help="show (or record) which way round the side cameras were")
    sp.add_argument("session")
    sp.add_argument("--set", choices=("swapped", "named"),
                    help="record the decision by hand (e.g. from tools/check_sides.py on a take)")
    a = ap.parse_args()
    store = Store(a.base)
    if a.cmd == "list":
        for s in store.sessions():
            print(f"{s['id']}  {s['takes']} takes  {human_bytes(s['bytes'])}  {s['lighting'] or '-'}"
                  + ("  exported" if s["exported"] else ""))
    elif a.cmd == "takes":
        for t in store.takes(a.session):
            print(f"{t['id']}  {t['status']}  {t['sets']} sets ({t['deleted_sets']} deleted)  "
                  f"{t['duration_s']:.1f} s  {human_bytes(t['bytes'])}")
    elif a.cmd == "sides":
        print(json.dumps(store.sides(a.session, None if a.set is None else a.set == "swapped"), indent=1))
    else:
        def show(f, text):
            try:
                print(f"\r{f * 100:5.1f}% {text:40.40}", end="", flush=True)
            except BrokenPipeError:   # whatever read the output went (| head, a closed terminal): export on
                os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        path = store.export(a.session, progress=show)
        print(f"\n{path}  {human_bytes(tree_bytes(path))}")


if __name__ == "__main__":
    main()
