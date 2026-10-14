#!/usr/bin/env python3
"""Check a hand recorder export before it's uploaded, or when it's received (DESIGN.md "Upload").

The window runs it before an upload and refuses on errors; the maintainer runs it on each
submission. Standard library only. Given an export folder (takes.py's exports/<session>/), it
checks:
  - SHA256SUMS: every file listed and matching, nothing missing from it;
  - an allow-list of files: anything else (or a symlink) is an error;
  - manifest.json: the schema, required keys and types, and that it agrees with the files;
  - the consent version is there, the contributor id is a random uuid4, the contributor is an adult;
  - calibration.json has nothing session.py's strip_calibration would still remove;
  - device.json (the rig's pose in the CAD frame) holds only cv.cad_from_cal and head, as
    plus_x, plus_z and position; sessions recorded before it existed get a warning;
  - each sets.bin.zst decompresses to the end (so a truncated one fails) and parses as FHSET01:
    every set's header is checked (camera names, sizes, record length) and counted against the
    manifest; the pixels themselves aren't looked at;
  - every .jsonl line parses, and take.json does;
  - a test-phase export (a bounty round or debug session under TESTING.md, DESIGN.md "Testing"):
    its recordings (sets.bin.zst.age, live.bin.zst.age) are whole age files to the test dataset's
    keys (test-recipients.txt), nothing of them unencrypted; their insides aren't checked (that
    needs a key);
  - the side cameras (sides.py): the session's "sides" decision, and each take's files named
    right by it (a warning when it wasn't decided: the maintainer's check tells then);
  - the total size.
It runs on Linux and Windows, with Python 3.12 or later. It reports errors (don't upload or accept this) and warnings (look at it).

usage: validate.py DIR [--json]      exit status 0: no errors, 1: errors, 2: not an export
"""
import argparse
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from session import _key_identifies, strip_calibration  # noqa: E402  (standard library only, next to this file)
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))
from recorder import age  # noqa: E402  (its shape check needs no cryptography library)
from recorder.checks import (Cancelled, Report, check_sums, is_uuid4, listed, need,  # noqa: E402,F401
                             read_json_file)

# Decompression, the first that's there: Python 3.14's compression.zstd, the zstandard package
# (pip install zstandard: the maintainer's Windows PC), the zstd program.
try:
    from compression import zstd as _zstd
except ImportError:
    _zstd = None
try:
    import zstandard as _zstandard
except ImportError:
    _zstandard = None

EXPORT_SCHEMA = 1
TOP_FILES = {"manifest.json", "calibration.json", "device.json", "SHA256SUMS"}
REGION_CONSENT = "2026-10-03"   # the consent version that added the residency confirmation
TAKE_FILES = {"prompts.jsonl", "poses.jsonl", "take.json", "sets.bin.zst", "sets.bin.zst.age", "live.bin.zst.age"}
TEST_RECIPIENTS = os.path.join(HERE, "test-recipients.txt")
CONSENT_KEYS = {"public": "consent", "testing": "testing_consent"}   # profile.json's record of each consent
TAKE_RE = re.compile(r"^\d{2}-[a-z0-9][a-z0-9-]*$")
SESSION_RE = re.compile(r"^\d{8}-\d{6}(?:-\d+)?$")
CAM_NAME_RE = re.compile(r"^[a-z][a-z0-9_]{0,15}$")
TAKE_STATUSES = ("complete", "stopped", "skipped")
HDR = struct.Struct("<8sII")       # fh_set_hdr_t: magic, ncams, bytes (hands/track/record.h)
CAM = struct.Struct("<16sIIQQ")    # fh_set_cam_t: name, width, height, capture_ns, dqbuf_ns
MAX_CAMS = 16
MIN_SIDE, MAX_SIDE = 16, 4096
# One round is about 10 GB before compression (ft_handrec.ROUND_BYTES), a few GB after it.
WARN_BYTES = 15 * 1000 ** 3
MAX_BYTES = 40 * 1000 ** 3
CHUNK = 1 << 20
OUT_CHUNK = 8 << 20
# zstandard's decompressobj has no output limit: it gets the input this much at a time, so even a
# stream made to inflate hugely gives out a few hundred MB at most per call.
ZSTANDARD_PIECE = 8 << 10


# ---------------------------------------------------------------- the files

def walk_files(root, report):
    """The export's files as relative paths, and allow-list errors on the way."""
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root).replace(os.sep, "/")
        rel_dir = "" if rel_dir == "." else rel_dir
        depth = 0 if not rel_dir else rel_dir.count("/") + 1
        for d in list(dirnames):
            rel = f"{rel_dir}/{d}" if rel_dir else d
            ok = (depth == 0 and d == "takes") or (depth == 1 and rel_dir == "takes" and TAKE_RE.match(d))
            if os.path.islink(os.path.join(dirpath, d)):
                report.error(f"{rel}: a symlink, not allowed in an export")
                dirnames.remove(d)
            elif not ok:
                report.error(f"{rel}/: not an allowed folder")
                dirnames.remove(d)
        for name in filenames:
            rel = f"{rel_dir}/{name}" if rel_dir else name
            full = os.path.join(dirpath, name)
            if os.path.islink(full) or not os.path.isfile(full):
                report.error(f"{rel}: a symlink or special file, not allowed in an export")
                continue
            if not ((depth == 0 and name in TOP_FILES) or (depth == 2 and name in TAKE_FILES)):
                report.error(f"{rel}: not an allowed file")
            files.append(rel)
    return sorted(files)


def check_jsonl(path, rel, report, cancelled):
    """Every line a JSON object with a time "t"."""
    try:
        with open(path, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                if n % 50000 == 0 and cancelled():
                    raise Cancelled()
                if not line.strip():
                    continue
                try:
                    obj = json.loads(line)
                except ValueError as e:
                    report.error(f"{rel} line {n}: not valid JSON: {e}")
                    return
                if not isinstance(obj, dict) or not isinstance(obj.get("t"), int):
                    report.error(f"{rel} line {n}: not an object with a time \"t\"")
                    return
    except UnicodeDecodeError:
        report.error(f"{rel}: not UTF-8 text")
    except OSError as e:
        report.error(f"{rel}: can't read it: {e.strerror or e}")


# ---------------------------------------------------------------- FHSET01 inside the zstd stream

class SetParser:
    """Feed it the decompressed stream in pieces: it checks each set's header and skips the
    pixels. problem holds the first thing wrong (parsing stops there)."""

    def __init__(self):
        self.buf = bytearray()
        self.skip = 0           # pixel bytes of the current set still to pass over
        self.sets = 0
        self.raw = 0
        self.cams = {}          # name -> (width, height), every camera seen
        self.first = None       # the first set's cameras, [{name, width, height}]
        self.last_t = 0
        self.backwards = 0      # sets whose earliest dqbuf_ns went back in time
        self.problem = ""

    def feed(self, data):
        if self.problem:
            return
        self.raw += len(data)
        view = memoryview(data)
        while len(view) and not self.problem:
            if self.skip:
                n = min(self.skip, len(view))
                self.skip -= n
                view = view[n:]
                continue
            self.buf += view
            view = view[len(view):]
            self._parse()

    def _parse(self):
        while not self.problem and not self.skip:
            if len(self.buf) < HDR.size:
                return
            magic, ncams, nbytes = HDR.unpack_from(self.buf, 0)
            where = f"set {self.sets + 1}"
            if magic != b"FHSET01\0":
                self.problem = f"{where}: not an FHSET01 record"
                return
            if not 0 < ncams <= MAX_CAMS:
                self.problem = f"{where}: {ncams} cameras"
                return
            head = HDR.size + CAM.size * ncams
            if len(self.buf) < head:
                return
            pixels, cams, times = 0, [], []
            for k in range(ncams):
                raw_name, w, h, _, dqbuf_ns = CAM.unpack_from(self.buf, HDR.size + k * CAM.size)
                name_bytes, _, pad = raw_name.partition(b"\0")
                name = name_bytes.decode("ascii", errors="replace")
                if pad.strip(b"\0") or not CAM_NAME_RE.match(name):
                    self.problem = f"{where}: a camera name that isn't one: {raw_name!r}"
                    return
                if not (MIN_SIDE <= w <= MAX_SIDE and MIN_SIDE <= h <= MAX_SIDE):
                    self.problem = f"{where}: camera {name} is {w}x{h}"
                    return
                if self.cams.setdefault(name, (w, h)) != (w, h):
                    self.problem = f"{where}: camera {name} changed size to {w}x{h}"
                    return
                pixels += w * h
                cams.append({"name": name, "width": w, "height": h})
                times.append(dqbuf_ns)
            if nbytes != head + pixels:
                self.problem = f"{where}: {nbytes} bytes, but its cameras need {head + pixels}"
                return
            if self.first is None:
                self.first = cams
            t = min(times)
            if t < self.last_t:
                self.backwards += 1
            self.last_t = t
            self.sets += 1
            rest = len(self.buf) - head
            if rest >= pixels:
                del self.buf[:head + pixels]
            else:
                self.skip = pixels - rest
                self.buf.clear()

    def finish(self):
        """After the stream's end: a set cut short is a problem."""
        if not self.problem and (self.skip or self.buf):
            self.problem = f"the stream ends inside set {self.sets + 1}"


def find_zstd():
    found = shutil.which("zstd")
    if found:
        return found
    return next((p for p in ("/usr/bin/zstd", "/run/host/usr/bin/zstd") if os.access(p, os.X_OK)), None)


class Frames:
    """A zstd stream, decompressed piece by piece with compression.zstd or zstandard, frame after
    frame (zstd -T2 may write several). feed(data) yields the output; complete is false while a
    frame is unfinished, which at the end of the input means the stream was cut short."""

    def __init__(self):
        self.dec = self._new()
        self.fed = False   # the current decompressor has had input

    @staticmethod
    def _new():
        return _zstd.ZstdDecompressor() if _zstd is not None else _zstandard.ZstdDecompressor().decompressobj()

    @property
    def complete(self):
        return self.dec.eof or not self.fed

    def feed(self, data):
        while data:
            if self.dec.eof:   # another frame follows the one that ended
                self.dec, self.fed = self._new(), False
            if _zstd is not None:
                out = self.dec.decompress(data, max_length=OUT_CHUNK)
                data = b""
                self.fed = True
                yield out
                # More output may wait without more input.
                while not self.dec.eof and not self.dec.needs_input:
                    yield self.dec.decompress(b"", max_length=OUT_CHUNK)
            else:
                piece, data = data[:ZSTANDARD_PIECE], data[ZSTANDARD_PIECE:]
                self.fed = True
                out = self.dec.decompress(piece)
                if out:
                    yield out
            if self.dec.eof:
                data = self.dec.unused_data + data


def check_sets(path, rel, entry, report, step, cancelled):
    """Decompress rel as a stream and parse it. entry: its take in the manifest, or None."""
    parser = SetParser()
    try:
        if _zstd is not None or _zstandard is not None:
            frames = Frames()
            with open(path, "rb") as f:
                while not parser.problem and (data := f.read(CHUNK)):
                    if cancelled():
                        raise Cancelled()
                    step(len(data))
                    for out in frames.feed(data):
                        parser.feed(out)
                        if parser.problem:
                            break
            if not parser.problem and not frames.complete:
                report.error(f"{rel}: the zstd stream is cut short")
                return
        else:
            zstd = find_zstd()
            if not zstd:
                report.warn(f"{rel}: not checked: no zstd (Python 3.14's compression.zstd, the zstandard "
                            "package or the zstd program)")
                return
            argv = [zstd, "-dcq", path]
            if os.name == "posix" and shutil.which("nice"):
                argv = ["nice", "-n", "19"] + argv
            proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            try:
                while not parser.problem and (data := proc.stdout.read(CHUNK)):
                    if cancelled():
                        raise Cancelled()
                    parser.feed(data)
            except BaseException:
                proc.kill()
                proc.wait()
                proc.stdout.close()
                raise
            if parser.problem:
                proc.kill()
            code = proc.wait()
            proc.stdout.close()
            step(os.path.getsize(path))
            if not parser.problem and code != 0:
                report.error(f"{rel}: doesn't decompress (zstd exit {code})")
                return
    except Cancelled:
        raise
    except Exception as e:   # OSError, compression.zstd.ZstdError
        report.error(f"{rel}: doesn't decompress: {e}")
        return
    parser.finish()
    if parser.problem:
        report.error(f"{rel}: not a valid FHSET01 recording: {parser.problem}")
        return
    if parser.sets == 0:
        report.error(f"{rel}: holds no frame sets")
    if parser.backwards:
        report.warn(f"{rel}: {parser.backwards} sets go back in time")
    if entry is not None:
        if entry.get("sets") != parser.sets:
            report.error(f"{rel}: {parser.sets} sets, the manifest says {entry.get('sets')}")
        if "raw_bytes" in entry and entry.get("raw_bytes") != parser.raw:
            report.error(f"{rel}: {parser.raw} bytes uncompressed, the manifest says {entry.get('raw_bytes')}")
        if isinstance(entry.get("cameras"), list) and parser.first is not None and entry["cameras"] != parser.first:
            report.error(f"{rel}: its cameras don't match the manifest's")
    return parser


# ---------------------------------------------------------------- the manifest

def check_pose(pose, where, report, extra=()):
    """A pose as device_config.json has it: plus_x, plus_z, position, each 3 finite numbers."""
    if not isinstance(pose, dict):
        report.error(f"device.json: {where} isn't an object")
        return
    for key in ("plus_x", "plus_z", "position"):
        v = pose.get(key)
        if not (isinstance(v, list) and len(v) == 3 and all(isinstance(x, (int, float)) and not isinstance(x, bool)
                                                            and math.isfinite(x) for x in v)):
            report.error(f"device.json: {where}.{key} isn't 3 numbers")
    unknown = set(pose) - {"plus_x", "plus_z", "position"} - set(extra)
    if unknown:
        report.error(f"device.json: {where} has unexpected keys: {listed(sorted(unknown))}")


def check_device(dev, report):
    """device.json: only cv.cad_from_cal and head (session.py's _write_device)."""
    if not isinstance(dev, dict):
        report.error("device.json: not a JSON object")
        return
    if set(dev) != {"cv", "head"}:
        report.error(f"device.json: keys {listed(sorted(dev))}, expected cv and head only")
    cv = dev.get("cv")
    if not isinstance(cv, dict) or set(cv) != {"cad_from_cal"}:
        report.error("device.json: cv should hold cad_from_cal only")
    else:
        check_pose(cv["cad_from_cal"], "cv.cad_from_cal", report, extra=("method",))
        if not isinstance(cv["cad_from_cal"].get("method", ""), str):
            report.error("device.json: cv.cad_from_cal.method isn't text")
    if "head" in dev:
        check_pose(dev["head"], "head", report)
    _, removed = strip_calibration(dev)
    if removed:
        report.error(f"device.json: identifying fields: {listed(removed)}")


def identifying_keys(o, path):
    """Paths of keys under o that name a serial, uuid, mac or id (session.py's rule)."""
    out = []
    if isinstance(o, dict):
        for k, v in o.items():
            p = f"{path}.{k}"
            out += [p] if _key_identifies(k) else identifying_keys(v, p)
    elif isinstance(o, list):
        for i, v in enumerate(o):
            out += identifying_keys(v, f"{path}[{i}]")
    return out


def is_debug(m):
    """A debug session's export: session.debug in the manifest."""
    return isinstance(m, dict) and isinstance(m.get("session"), dict) and isinstance(m["session"].get("debug"), dict)


def is_testing(m):
    """An export for the test dataset (DESIGN.md "Testing"): recorded under TESTING.md's consent,
    its recordings encrypted. A debug session always is."""
    return isinstance(m, dict) and (m.get("consent_kind") == "testing" or is_debug(m))


def check_encryption(m, report):
    """A test export's manifest "encryption": to the test dataset's keys. Returns the keys."""
    enc = m.get("encryption")
    if not isinstance(enc, dict):
        report.error("manifest: a test-phase export without encryption (export it again)")
        return []
    try:
        shipped = age.read_recipients(TEST_RECIPIENTS)
    except (OSError, ValueError) as e:
        report.error(f"the test dataset's keys ({TEST_RECIPIENTS}): {e}")
        shipped = None
    r = enc.get("recipients")
    if not isinstance(r, list) or not r or not all(isinstance(x, str) for x in r):
        report.error("manifest: encryption.recipients isn't a list of keys")
        return []
    if shipped is not None and sorted(r) != sorted(shipped):
        report.error("manifest: encrypted to other keys than the test dataset's (test-recipients.txt)")
    if not isinstance(enc.get("files"), list):
        report.error("manifest: encryption.files isn't a list")
    return r


def check_manifest(m, report, root):
    """The manifest's keys and types. Returns {take id: entry} for the file checks."""
    if not isinstance(m, dict):
        report.error("manifest: not a JSON object")
        return {}
    debug, testing = is_debug(m), is_testing(m)
    suffix = ".age" if testing else ""
    kind = "testing" if testing else "public"
    if m.get("consent_kind", "public") not in CONSENT_KEYS:
        report.error(f"manifest: consent_kind {m.get('consent_kind')!r}")
    if debug and m.get("consent_kind") != "testing":
        report.error("manifest: a debug session not recorded under the testing consent")
    if debug and m["session"]["debug"].get("use") != "testing":
        report.error("manifest: a debug session not marked for testing only")
    enc_files = None
    if testing:
        enc_files = (m.get("encryption") or {}).get("files") if isinstance(m.get("encryption"), dict) else None
    elif "encryption" in m:
        report.error("manifest: encryption in an export that isn't for the test dataset")
    if m.get("schema") != EXPORT_SCHEMA:
        report.error(f"manifest: schema {m.get('schema')!r}, expected {EXPORT_SCHEMA}")
    tool = need(m, "tool", str, "", report)
    if tool is not None and not tool.startswith("ft-handrec"):
        report.warn(f"manifest: tool is {tool!r}")
    need(m, "exported", str, "", report)
    sid = need(m, "session_id", str, "", report)
    if sid is not None and not SESSION_RE.match(sid):
        report.error(f"manifest: session_id {sid!r} isn't a session id")
    if sid and SESSION_RE.match(os.path.basename(root)) and os.path.basename(root) != sid:
        report.warn(f"the folder is named {os.path.basename(root)}, the manifest's session is {sid}")
    contributor = need(m, "contributor", str, "", report)
    if contributor is not None and not is_uuid4(contributor):
        report.error(f"manifest: the contributor id {contributor!r} isn't a random uuid4")
    parent = os.path.basename(os.path.dirname(root))
    if contributor and is_uuid4(parent) and parent != contributor:
        report.error(f"the export sits under contributor {parent}, but its manifest says {contributor}")
    consent = need(m, "consent_version", str, "", report)
    if consent is not None and not consent.strip():
        report.error("manifest: the consent version is empty")

    profile = need(m, "profile", dict, "", report)
    if profile is not None:
        if profile.get("contributor") != contributor:
            report.error("manifest: profile.contributor differs from contributor")
        pc = need(profile, CONSENT_KEYS[kind], dict, "profile.", report)
        if pc is not None:
            if pc.get("version") != consent:
                report.error("manifest: profile.consent.version differs from consent_version")
            if pc.get("adult") is not True:
                report.error("manifest: the contributor didn't confirm being 18 or older")
            # from consent 2026-10-03: not living in Illinois, Texas or Washington (CONSENT.md)
            if str(pc.get("version") or "") >= REGION_CONSENT and pc.get("region_ok") is not True:
                report.error("manifest: the contributor didn't confirm not living in Illinois, Texas or Washington")
            need(pc, "accepted", str, "profile.consent.", report)
        optional = profile.get("optional")
        if isinstance(optional, dict) and str(optional.get("notes") or "").strip():
            report.warn("the profile's notes are included: check they hold nothing private")
        extra = set(profile) - {"schema", "contributor", CONSENT_KEYS[kind], "optional"}
        if extra:
            report.error(f"manifest: the profile has unexpected keys: {listed(sorted(extra))}")

    session = need(m, "session", dict, "", report)
    if session is not None:
        if session.get("contributor") not in (contributor, ""):
            report.error("manifest: session.contributor differs from contributor")
        for key, t in (("started", str), ("lighting", dict), ("checklist", dict), ("device", dict),
                       ("takes", list)):
            need(session, key, t, "session.", report)
        if session.get("dry_run"):
            report.error("this is a dry-run session: it has no real recordings")
        if "uploads" in session:
            report.warn("manifest: session.uploads is included (earlier uploads' links)")
        if not isinstance(session.get("calibration_removed", []), list):
            report.error("manifest: session.calibration_removed isn't a list")
        checklist = session.get("checklist")
        if isinstance(checklist, dict) and str(checklist.get("notes") or "").strip():
            report.warn("the checklist's notes are included: check they hold nothing private")
        # Keys only: version strings like SteamVR's "r25358740+28b72a4f-1" look like serials.
        keys = identifying_keys(session.get("device"), "session.device")
        if keys:
            report.error(f"manifest: identifying fields: {listed(keys)}")
    if "/home/" in json.dumps(m):
        report.warn("manifest: it mentions a home folder path (a username may show)")

    entries = {}
    takes = need(m, "takes", list, "", report)
    for i, t in enumerate(takes or []):
        where = f"takes[{i}]."
        if not isinstance(t, dict):
            report.error(f"manifest: takes[{i}] isn't an object")
            continue
        tid = need(t, "id", str, where, report)
        if tid is None:
            continue
        if not TAKE_RE.match(tid):
            report.error(f"manifest: take id {tid!r} isn't one")
            continue
        if tid in entries:
            report.error(f"manifest: take {tid} is listed twice")
        entries[tid] = t
        for key, typ in (("section", str), ("title", str), ("status", str), ("sets", int),
                         ("sets_deleted", int), ("cameras", list), ("duration_s", (int, float))):
            need(t, key, typ, where, report)
        if t.get("status") not in TAKE_STATUSES:
            report.warn(f"manifest: take {tid} has status {t.get('status')!r}")
        f = t.get("file")
        if f is None:
            if t.get("sets"):
                report.error(f"manifest: take {tid} has {t.get('sets')} sets but no file")
        elif f != f"takes/{tid}/sets.bin.zst{suffix}":
            report.error(f"manifest: take {tid}'s file is {f!r}")
        elif not isinstance(t.get("raw_bytes"), int):
            report.error(f"manifest: take {tid} has a file but no raw_bytes")
        live = t.get("live")
        if live is not None:
            if not debug:
                report.error(f"manifest: take {tid} has a live log, but this isn't a debug session")
            elif not isinstance(live, dict) or live.get("file") != f"takes/{tid}/live.bin.zst.age":
                report.error(f"manifest: take {tid}'s live log is {live!r}")
        if testing and isinstance(enc_files, list):
            for name in (f, (live or {}).get("file") if isinstance(live, dict) else None):
                if name and name not in enc_files:
                    report.error(f"manifest: {name} isn't in encryption.files")
    check_sides(m, entries, report)
    return entries


def check_sides(m, entries, report):
    """Which side camera is which (sides.py): session.sides.swapped is true, false or null, and
    every take's files carry names_swapped equal to it (export renames them so)."""
    session = m.get("session") if isinstance(m.get("session"), dict) else {}
    s = session.get("sides")
    if s is None:
        report.warn("manifest: no session.sides (recorded before the side cameras were checked): "
                    "the maintainer's check tells which way round they are")
        return
    if not isinstance(s, dict) or s.get("swapped") not in (True, False, None):
        report.error("manifest: session.sides isn't {\"swapped\": true|false|null, ...}")
        return
    swapped = s["swapped"]
    if swapped is None:
        report.warn("the side cameras' naming wasn't decided while recording (no live tracker): "
                    "the maintainer's check tells which way round they are")
    for tid, t in entries.items():
        ts = t.get("sides")
        if ts is None:
            continue
        if not isinstance(ts, dict) or ts.get("names_swapped") not in (True, False, None):
            report.error(f"manifest: take {tid}'s sides isn't {{\"names_swapped\": true|false|null, ...}}")
        elif swapped is not None and ts["names_swapped"] != swapped:
            report.error(f"take {tid}: its side cameras aren't named right (names_swapped "
                         f"{ts['names_swapped']}, the session's swapped {swapped}): export it again")


def summarize(m):
    """What a pull request's description says about the export: takes, minutes, lighting,
    objects, controllers, the bounty and sets a second, the consent and tool versions."""
    if not isinstance(m, dict):
        return {}
    session = m.get("session") if isinstance(m.get("session"), dict) else {}
    checklist = session.get("checklist") if isinstance(session.get("checklist"), dict) else {}
    lighting = session.get("lighting") if isinstance(session.get("lighting"), dict) else {}
    bounty = session.get("bounty") if isinstance(session.get("bounty"), dict) else {}
    takes = [t for t in m.get("takes") or [] if isinstance(t, dict)]
    seconds = sum(t.get("duration_s") or 0 for t in takes if isinstance(t.get("duration_s"), (int, float)))
    return {"session": m.get("session_id", ""), "contributor": m.get("contributor", ""),
            "takes": sum(1 for t in takes if t.get("file")), "takes_listed": len(takes),
            "sets": sum(t.get("sets") or 0 for t in takes if isinstance(t.get("sets"), int)),
            "minutes": round(seconds / 60, 1), "lighting": lighting.get("chosen", ""),
            "objects": list(checklist.get("objects") or []), "own_objects": len(checklist.get("own_objects") or []),
            "controllers": checklist.get("controllers", ""), "consent_version": m.get("consent_version", ""),
            "tool": m.get("tool", ""), "bounty": bounty.get("id", "") if isinstance(bounty.get("id"), str) else "",
            "record_hz": session.get("record_hz") if isinstance(session.get("record_hz"), (int, float)) else None,
            "debug": is_debug(m), "testing": is_testing(m)}


# ---------------------------------------------------------------- all of it

def validate(root, progress=None, cancel=None):
    """Check the export at root. progress(fraction 0..1, text) is called as it goes; cancel()
    (or cancel.is_set()) returning true raises Cancelled. Returns a Report."""
    root = os.path.abspath(root)
    report = Report(root)
    cancelled = getattr(cancel, "is_set", cancel) or (lambda: False)
    tell = progress or (lambda fraction, text: None)
    if not os.path.isdir(root):
        report.error(f"{root}: no such folder")
        return report
    files = walk_files(root, report)
    sizes = {rel: os.path.getsize(os.path.join(root, rel)) for rel in files}
    report.bytes = sum(sizes.values())
    # Work: each file's checksum, plus reading each sets.bin.zst again to decompress it.
    total = max(1, report.bytes + sum(n for rel, n in sizes.items() if rel.endswith(".zst")))
    done = 0

    def step(n, text):
        nonlocal done
        done += n
        tell(min(done / total, 0.999), text)

    tell(0.0, "Checking checksums")
    check_sums(root, files, report, lambda n: step(n, "Checking checksums"), cancelled)

    manifest = None
    if "manifest.json" in files:
        manifest = read_json_file(os.path.join(root, "manifest.json"), "manifest.json", report)
    else:
        report.error("manifest.json is missing")
    entries = check_manifest(manifest, report, root) if manifest is not None else {}
    testing = is_testing(manifest)
    recipients = check_encryption(manifest, report) if testing else []
    report.summary = summarize(manifest)

    if "calibration.json" in files:
        cal = read_json_file(os.path.join(root, "calibration.json"), "calibration.json", report)
        if cal is not None:
            if not isinstance(cal, dict):
                report.error("calibration.json: not a JSON object")
            else:
                _, removed = strip_calibration(cal)
                if removed:
                    report.error(f"calibration.json: identifying fields left in: {listed(removed)}")
    else:
        report.warn("no calibration.json: the recordings can't be used in 3D without it")
    if "device.json" in files:
        dev = read_json_file(os.path.join(root, "device.json"), "device.json", report)
        if dev is not None:
            check_device(dev, report)
    else:
        report.warn("no device.json (a session from before it was recorded): the labeller falls back to "
                    "another unit's head frame")

    take_dirs = sorted({rel.split("/")[1] for rel in files if rel.startswith("takes/") and rel.count("/") == 2})
    for tid in take_dirs:
        if tid not in entries:
            report.error(f"takes/{tid}/ isn't in the manifest")
    for tid, entry in entries.items():
        if tid not in take_dirs:
            report.error(f"take {tid} is in the manifest but has no folder")
    for tid in take_dirs:
        if cancelled():
            raise Cancelled()
        base = f"takes/{tid}/"
        title = (entries.get(tid) or {}).get("title") or tid
        for name in ("prompts.jsonl", "poses.jsonl"):
            rel = base + name
            if rel in sizes:
                check_jsonl(os.path.join(root, rel), rel, report, cancelled)
            else:
                report.warn(f"{rel} is missing")
        if base + "take.json" in sizes:
            tj = read_json_file(os.path.join(root, base + "take.json"), base + "take.json", report)
            if tj is not None and not isinstance(tj, dict):
                report.error(f"{base}take.json: not a JSON object")
        else:
            report.warn(f"{base}take.json is missing")
        entry = entries.get(tid)
        if testing:
            if base + "sets.bin.zst" in sizes:
                report.error(f"{base}sets.bin.zst: a test-phase recording unencrypted (export it again)")
            for name, listed_file in (("sets.bin.zst.age", (entry or {}).get("file")),
                                      ("live.bin.zst.age", ((entry or {}).get("live") or {}).get("file"))):
                rel = base + name
                if rel in sizes:
                    problem = age.shape(os.path.join(root, rel), recipients)
                    if problem:
                        report.error(f"{rel}: {problem}")
                    step(sizes[rel], f"Checking {title}")
                elif listed_file:
                    report.error(f"{rel} is missing")
            continue
        for name in ("sets.bin.zst.age", "live.bin.zst.age"):
            if base + name in sizes:
                report.error(f"{base}{name}: encrypted, but this export isn't for the test dataset")
        rel = base + "sets.bin.zst"
        if rel in sizes:
            check_sets(os.path.join(root, rel), rel, entry, report,
                       lambda n, title=title: step(n, f"Checking {title}"), cancelled)
        elif entry is not None and entry.get("file"):
            report.error(f"{rel} is missing")

    if report.bytes > MAX_BYTES:
        report.error(f"the export is {report.bytes / 1000 ** 3:.1f} GB, over the {MAX_BYTES / 1000 ** 3:.0f} GB limit")
    elif report.bytes > WARN_BYTES:
        report.warn(f"the export is large: {report.bytes / 1000 ** 3:.1f} GB")
    tell(1.0, "Checked")
    return report


def main():
    ap = argparse.ArgumentParser(description="Check a hand recorder export (an exports/<session>/ folder).")
    ap.add_argument("dir")
    ap.add_argument("--json", action="store_true", help="print the result as JSON")
    a = ap.parse_args()
    if not os.path.isdir(a.dir):
        print(f"{a.dir}: no such folder", file=sys.stderr)
        return 2
    if hasattr(os, "setpriority"):   # not on Windows
        try:
            os.setpriority(os.PRIO_PROCESS, 0, 19)   # checksums and decompression: stay out of VR's way
        except OSError:
            pass
    report = validate(a.dir)
    if a.json:
        print(json.dumps(report.as_dict(), indent=2))
    else:
        for e in report.errors:
            print(f"error: {e}")
        for w in report.warnings:
            print(f"warning: {w}")
        s = report.summary
        if s:
            print(f"{s.get('session')}: {s.get('takes')} takes, {s.get('sets')} sets, {s.get('minutes')} min, "
                  f"{report.bytes / 1000 ** 2:.1f} MB, consent {s.get('consent_version') or '-'}")
        print("OK" if report.ok else f"{len(report.errors)} errors")
    return 0 if report.ok else 1


if __name__ == "__main__":
    sys.exit(main())
