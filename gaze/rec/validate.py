#!/usr/bin/env python3
"""Check an eye recorder export before it's uploaded, or when it's received (takes.py's
exports/<round>/).

The window runs it before an upload and refuses on errors; the maintainer runs it on each
submission. The images are encrypted, so without a key it checks everything around them:
  - SHA256SUMS: every file listed and matching, nothing missing from it;
  - an allow-list of files: anything else (an unencrypted chunk, health.json, a symlink) is an error;
  - every .age file is age v1 to as many X25519 recipients as the manifest names, and its size
    is one a whole age file can have; the manifest's recipients are recipients.txt's;
  - manifest.json: the schema, required keys and types, the consent, a random uuid4 contributor,
    no dry run or replay, no health answers outside health.json.age;
  - each take's index.tsv against its chunks (every chunk named is there, every one there is
    named, frames numbered from 0, times going forward) and against the manifest's counts;
  - every .jsonl line is a JSON object with a time "t", and take.json and fit.json parse;
  - the total size.
With --identity KEYFILE (one of the dataset's keys, the maintainer's side) it also decrypts each
chunk in memory and checks it decompresses to the frames index.tsv gives it.

Standard library only without --identity (zstd and the cryptography library with it). Python
3.12 or later, Linux or Windows.

usage: validate.py DIR [--json] [--identity KEYFILE]
       exit status 0: no errors, 1: errors, 2: not an export
"""
import argparse
import json
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))
from recorder import age  # noqa: E402
from recorder.checks import (Cancelled, Report, check_sums, is_uuid4, listed, need,  # noqa: E402,F401
                             read_json_file)

EXPORT_SCHEMA = 1
RECIPIENTS = os.path.join(HERE, "recipients.txt")
TOP_FILES = {"manifest.json", "fit.json", "health.json.age", "SHA256SUMS"}
TAKE_FILES = {"take.json", "index.tsv", "targets.jsonl", "ipd.jsonl", "prompts.jsonl", "own.jsonl",
              "steamvr.jsonl", "poses.jsonl"}
CAM_DIRS = ("cam0", "cam1")
ROUND_RE = re.compile(r"^\d{8}-\d{6}-[a-z0-9][a-z0-9-]{0,39}$")
TAKE_RE = re.compile(r"^\d{2,}-[a-z0-9][a-z0-9_-]{0,63}$")
CHUNK_FILE_RE = re.compile(r"^(\d{6})\.zst\.age$")
TAKE_STATUSES = ("complete", "stopped", "skipped")
PROFILE_KEYS = {"schema", "contributor", "consent", "optional", "checklist", "bounties", "eyes_ipd"}
INDEX_HEADER = "# camera\tchunk\tframe\tslot\tt_raw"
FRAME = 512 * 400
CHUNK_FRAMES_MAX = 1000   # eyerec.py writes 90 a chunk
# One round is about 2.6 GB (2026-10-14 replay: 20 MB/s on disk); encryption adds next to nothing.
WARN_BYTES = 6 * 1000 ** 3
MAX_BYTES = 15 * 1000 ** 3


# ---------------------------------------------------------------- the files

def walk_files(root, report):
    """The export's files as relative paths, and allow-list errors on the way."""
    files = []
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root).replace(os.sep, "/")
        rel_dir = "" if rel_dir == "." else rel_dir
        parts = rel_dir.split("/") if rel_dir else []
        for d in list(dirnames):
            rel = f"{rel_dir}/{d}" if rel_dir else d
            ok = ((not parts and d == "takes") or (parts == ["takes"] and TAKE_RE.match(d))
                  or (len(parts) == 2 and parts[0] == "takes" and d in CAM_DIRS))
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
            ok = ((not parts and name in TOP_FILES) or (len(parts) == 2 and name in TAKE_FILES)
                  or (len(parts) == 3 and CHUNK_FILE_RE.match(name)))
            if not ok:
                hint = " (unencrypted: export it again)" if name.endswith(".zst") or name == "health.json" else ""
                report.error(f"{rel}: not an allowed file{hint}")
            files.append(rel)
    return sorted(files)


age_shape = age.shape   # recorder/age.py: shared with the hand recorder's debug exports


def check_jsonl(path, rel, report, cancelled):
    """Every line a JSON object with a finite time "t" (RAW seconds, or MONOTONIC ns in prompts)."""
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
                t = obj.get("t") if isinstance(obj, dict) else None
                if isinstance(t, bool) or not isinstance(t, (int, float)) or not math.isfinite(t):
                    report.error(f"{rel} line {n}: not an object with a time \"t\"")
                    return
    except UnicodeDecodeError:
        report.error(f"{rel}: not UTF-8 text")
    except OSError as e:
        report.error(f"{rel}: can't read it: {e.strerror or e}")


def read_index(path, rel, report):
    """index.tsv as {(camera, chunk): [(frame, t)]}, or None when it doesn't parse."""
    out = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if n == 1:
                if line != INDEX_HEADER:
                    report.error(f"{rel}: the first line isn't the header")
                    return None
                continue
            if not line:
                continue
            fields = line.split("\t")
            try:
                c, chunk, i, slot = (int(x) for x in fields[:4])
                t = float(fields[4])
                if len(fields) != 5 or not math.isfinite(t) or c not in (0, 1) or min(chunk, i, slot) < 0:
                    raise ValueError
            except (ValueError, IndexError):
                report.error(f"{rel} line {n}: not camera, chunk, frame, slot, time")
                return None
            out.setdefault((c, chunk), []).append((i, t))
    return out


def check_take(root, tid, entry, sizes, recipients, identity, report, step, cancelled):
    base = f"takes/{tid}/"
    for name in sorted(TAKE_FILES - {"take.json", "index.tsv"}):
        rel = base + name
        if rel in sizes:
            check_jsonl(os.path.join(root, rel), rel, report, cancelled)
        elif name in ("targets.jsonl", "prompts.jsonl", "own.jsonl"):
            report.warn(f"{rel} is missing")
    if base + "take.json" in sizes:
        tj = read_json_file(os.path.join(root, base + "take.json"), base + "take.json", report)
        if tj is not None and not isinstance(tj, dict):
            report.error(f"{base}take.json: not a JSON object")
    else:
        report.warn(f"{base}take.json is missing")
    chunks = {}
    for rel in sizes:
        if rel.startswith(base) and rel.count("/") == 3:
            m = CHUNK_FILE_RE.match(rel.rsplit("/", 1)[1])
            if m:
                chunks[(int(rel.split("/")[2][3:]), int(m.group(1)))] = rel
    if base + "index.tsv" not in sizes:
        if chunks:
            report.error(f"{base}index.tsv is missing, but there are {len(chunks)} chunks")
        return
    index = read_index(os.path.join(root, base + "index.tsv"), base + "index.tsv", report)
    if index is None:
        return
    frames = [0, 0]
    unnamed = sorted(set(chunks) - set(index))
    if unnamed:
        report.error(f"{base}: chunks index.tsv doesn't name: {listed(chunks[k] for k in unnamed)}")
    last = [-math.inf, -math.inf]
    for key in sorted(index):
        if cancelled():
            raise Cancelled()
        c, chunk = key
        rows = index[key]
        frames[c] += len(rows)
        name = f"{base}cam{c}/{chunk:06d}.zst.age"
        if key not in chunks:
            report.error(f"{name}: in index.tsv but missing")
            continue
        if [i for i, _ in rows] != list(range(len(rows))) or len(rows) > CHUNK_FRAMES_MAX:
            report.error(f"{base}index.tsv: chunk {c}/{chunk:06d}'s frames aren't numbered 0 to {len(rows) - 1}")
        times = [t for _, t in rows]
        if any(b < a for a, b in zip(times, times[1:])) or times[0] < last[c]:
            report.error(f"{base}index.tsv: camera {c}'s times go back at chunk {chunk:06d}")
        last[c] = times[-1]
        problem = age_shape(os.path.join(root, name), recipients)
        if problem:
            report.error(f"{name}: {problem}")
        elif identity:
            problem = decrypted_problem(os.path.join(root, name), identity, len(rows))
            if problem:
                report.error(f"{name}: {problem}")
        step(sizes[name])
    if entry is not None and isinstance(entry.get("frames"), list) and entry["frames"] != frames:
        report.error(f"take {tid}: the manifest says {entry['frames']} frames, index.tsv has {frames}")


def decrypted_problem(path, identity, nframes):
    sys.path.insert(0, HERE)
    import eyerec   # zstd, as the recorder has it
    try:
        with open(path, "rb") as f:
            data = eyerec.decompress(age.decrypt_bytes(f.read(), identity))
    except ValueError as e:
        return f"doesn't decrypt: {e}"
    except Exception as e:
        return f"doesn't decompress: {e}"
    if len(data) != nframes * FRAME:
        return f"{len(data)} bytes, but index.tsv gives it {nframes} frames"
    return None


# ---------------------------------------------------------------- the manifest

def check_manifest(m, report, root, shipped):
    """The manifest's keys and types. Returns ({take id: entry}, the recipients)."""
    if not isinstance(m, dict):
        report.error("manifest: not a JSON object")
        return {}, []
    if m.get("schema") != EXPORT_SCHEMA:
        report.error(f"manifest: schema {m.get('schema')!r}, expected {EXPORT_SCHEMA}")
    tool = need(m, "tool", str, "", report)
    if tool is not None and not tool.startswith("ft-eyerec"):
        report.warn(f"manifest: tool is {tool!r}")
    need(m, "exported", str, "", report)
    need(m, "bounty", str, "", report)
    rid = need(m, "round_id", str, "", report)
    if rid is not None and not ROUND_RE.match(rid):
        report.error(f"manifest: round_id {rid!r} isn't a round id")
    if rid and ROUND_RE.match(os.path.basename(root)) and os.path.basename(root) != rid:
        report.warn(f"the folder is named {os.path.basename(root)}, the manifest's round is {rid}")
    contributor = need(m, "contributor", str, "", report)
    if contributor is not None and not is_uuid4(contributor):
        report.error(f"manifest: the contributor id {contributor!r} isn't a random uuid4")
    parent = os.path.basename(os.path.dirname(root))
    if contributor and is_uuid4(parent) and parent != contributor:
        report.error(f"the export sits under contributor {parent}, but its manifest says {contributor}")
    consent = need(m, "consent_version", str, "", report)
    if consent is not None and not consent.strip():
        report.error("manifest: the consent version is empty")

    recipients = []
    enc = need(m, "encryption", dict, "", report)
    if enc is not None:
        if enc.get("format") != "age-encryption.org/v1":
            report.error(f"manifest: encryption.format {enc.get('format')!r}")
        r = enc.get("recipients")
        if not isinstance(r, list) or not r or not all(isinstance(x, str) for x in r):
            report.error("manifest: encryption.recipients isn't a list of keys")
        else:
            recipients = r
            try:
                for x in r:
                    age.parse_recipient(x)
            except ValueError as e:
                report.error(f"manifest: encryption.recipients: {e}")
            if shipped is not None and sorted(r) != sorted(shipped):
                report.error("manifest: encrypted to other keys than the dataset's (recipients.txt)")

    profile = need(m, "profile", dict, "", report)
    if profile is not None:
        if profile.get("contributor") != contributor:
            report.error("manifest: profile.contributor differs from contributor")
        pc = need(profile, "consent", dict, "profile.", report)
        if pc is not None:
            if pc.get("version") != consent:
                report.error("manifest: profile.consent.version differs from consent_version")
            if pc.get("adult") is not True:
                report.error("manifest: the contributor didn't confirm being 18 or older")
            if pc.get("region_ok") is not True:
                report.error("manifest: the contributor didn't confirm not living in Illinois, Texas or Washington")
            need(pc, "accepted", str, "profile.consent.", report)
        optional = profile.get("optional")
        if isinstance(optional, dict) and str(optional.get("notes") or "").strip():
            report.warn("the profile's notes are included: check they hold nothing private")
        extra = set(profile) - PROFILE_KEYS
        if extra:
            report.error(f"manifest: the profile has unexpected keys: {listed(sorted(extra))}")

    session = need(m, "session", dict, "", report)
    if session is not None:
        for key, t in (("started", str), ("checklist", dict), ("takes", list), ("bounty", str)):
            need(session, key, t, "session.", report)
        if session.get("dry_run"):
            report.error("this is a dry-run round: it has no real recordings")
        if session.get("replay"):
            report.error("this round was recorded from a replay, not the headset's cameras")
        if session.get("status") not in ("done", None):
            report.warn(f"the round's status is {session.get('status')!r}")
        if "uploads" in session:
            report.warn("manifest: session.uploads is included (earlier uploads' links)")
        if "health" in session:
            report.error("manifest: health answers outside health.json.age")
        checklist = session.get("checklist")
        if isinstance(checklist, dict) and str(checklist.get("notes") or "").strip():
            report.warn("the checklist's notes are included: check they hold nothing private")
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
        for key, typ in (("group", str), ("status", str), ("frames", list), ("frames_deleted", list),
                         ("duration_s", (int, float)), ("chunks", int)):
            need(t, key, typ, where, report)
        if t.get("status") not in TAKE_STATUSES:
            report.warn(f"manifest: take {tid} has status {t.get('status')!r}")
        left = t.get("left_out")
        if isinstance(left, dict) and any(left.values()):
            report.warn(f"take {tid}: chunks the export left out: {left}")
    return entries, recipients


def summarize(m):
    """What a pull request's description says about the export."""
    if not isinstance(m, dict):
        return {}
    session = m.get("session") if isinstance(m.get("session"), dict) else {}
    checklist = session.get("checklist") if isinstance(session.get("checklist"), dict) else {}
    takes = [t for t in m.get("takes") or [] if isinstance(t, dict)]
    seconds = sum(t.get("duration_s") or 0 for t in takes if isinstance(t.get("duration_s"), (int, float)))
    frames = sum(sum(f for f in t.get("frames") or [] if isinstance(f, int)) for t in takes
                 if isinstance(t.get("frames"), list))
    enc = m.get("encryption") if isinstance(m.get("encryption"), dict) else {}
    return {"round": m.get("round_id", ""), "bounty": m.get("bounty", ""), "contributor": m.get("contributor", ""),
            "takes": len(takes), "frames": frames, "minutes": round(seconds / 60, 1),
            "glasses": checklist.get("glasses", ""), "health": "health.json.age" in (enc.get("files") or []),
            "consent_version": m.get("consent_version", ""), "tool": m.get("tool", "")}


# ---------------------------------------------------------------- all of it

def validate(root, progress=None, cancel=None, identity=None, recipients_path=RECIPIENTS):
    """Check the export at root. progress(fraction 0..1, text) is called as it goes; cancel()
    (or cancel.is_set()) returning true raises Cancelled. identity: an "AGE-SECRET-KEY-1..."
    string, to check inside the chunks. recipients_path: the keys the export must be encrypted
    to (None: don't compare). Returns a Report."""
    root = os.path.abspath(root)
    report = Report(root)
    cancelled = getattr(cancel, "is_set", cancel) or (lambda: False)
    tell = progress or (lambda fraction, text: None)
    if not os.path.isdir(root):
        report.error(f"{root}: no such folder")
        return report
    shipped = None
    if recipients_path:
        try:
            shipped = age.read_recipients(recipients_path)
        except (OSError, ValueError) as e:
            report.error(f"the dataset's keys ({recipients_path}): {e}")
    files = walk_files(root, report)
    sizes = {rel: os.path.getsize(os.path.join(root, rel)) for rel in files}
    report.bytes = sum(sizes.values())
    total = max(1, 2 * report.bytes)   # checksums, then the chunks again
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
    entries, recipients = check_manifest(manifest, report, root, shipped) if manifest is not None else ({}, [])
    report.summary = summarize(manifest)

    if "fit.json" in files:
        fit = read_json_file(os.path.join(root, "fit.json"), "fit.json", report)
        if fit is not None and not isinstance(fit, dict):
            report.error("fit.json: not a JSON object")
    else:
        report.warn("no fit.json: the round's fit readings are missing")
    enc_files = ((manifest or {}).get("encryption") or {}).get("files") if isinstance(manifest, dict) else None
    listed_health = isinstance(enc_files, list) and "health.json.age" in enc_files
    if ("health.json.age" in files) != listed_health:
        report.error("health.json.age and the manifest's encryption.files don't agree")
    if "health.json.age" in files and recipients:
        problem = age_shape(os.path.join(root, "health.json.age"), recipients)
        if problem:
            report.error(f"health.json.age: {problem}")

    take_dirs = sorted({rel.split("/")[1] for rel in files if rel.startswith("takes/") and rel.count("/") >= 2})
    for tid in take_dirs:
        if tid not in entries:
            report.error(f"takes/{tid}/ isn't in the manifest")
    for tid, entry in entries.items():
        if tid not in take_dirs:
            report.error(f"take {tid} is in the manifest but has no folder")
    for tid in take_dirs:
        if cancelled():
            raise Cancelled()
        check_take(root, tid, entries.get(tid), sizes, recipients, identity, report,
                   lambda n, tid=tid: step(n, f"Checking {tid}"), cancelled)

    if report.bytes > MAX_BYTES:
        report.error(f"the export is {report.bytes / 1000 ** 3:.1f} GB, over the {MAX_BYTES / 1000 ** 3:.0f} GB limit")
    elif report.bytes > WARN_BYTES:
        report.warn(f"the export is large: {report.bytes / 1000 ** 3:.1f} GB")
    tell(1.0, "Checked")
    return report


def main():
    ap = argparse.ArgumentParser(description="Check an eye recorder export (an exports/<round>/ folder).")
    ap.add_argument("dir")
    ap.add_argument("--json", action="store_true", help="print the result as JSON")
    ap.add_argument("--identity", help="an age identity file: also decrypt and check every chunk")
    a = ap.parse_args()
    if not os.path.isdir(a.dir):
        print(f"{a.dir}: no such folder", file=sys.stderr)
        return 2
    if hasattr(os, "setpriority"):   # not on Windows
        try:
            os.setpriority(os.PRIO_PROCESS, 0, 19)   # stay out of VR's way
        except OSError:
            pass
    identity = None
    if a.identity:
        with open(a.identity) as f:
            identity = next((line.strip() for line in f if line.strip().startswith("AGE-SECRET-KEY-1")), None)
        if not identity:
            print(f"{a.identity}: no age identity in it", file=sys.stderr)
            return 2
    report = validate(a.dir, identity=identity)
    if a.json:
        print(json.dumps(report.as_dict(), indent=2))
    else:
        for e in report.errors:
            print(f"error: {e}")
        for w in report.warnings:
            print(f"warning: {w}")
        s = report.summary
        if s:
            print(f"{s.get('round')}: {s.get('takes')} takes, {s.get('frames')} frames, {s.get('minutes')} min, "
                  f"{report.bytes / 1000 ** 2:.1f} MB, consent {s.get('consent_version') or '-'}")
        print("OK" if report.ok else f"{len(report.errors)} errors")
    return 0 if report.ok else 1


if __name__ == "__main__":
    sys.exit(main())
