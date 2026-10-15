#!/usr/bin/env python3
"""The eye recorder's rounds and takes on disk: listing, review, deleting, and the encrypted export.

The layout is DESIGN.md's "Files":
  BASE/profile.json
  BASE/rounds/<YYYYMMDD-HHMMSS>-<bounty>/session.json, fit.json, health.json, session.log
  BASE/rounds/<round>/takes/<NN>-<group>/cam0/*.zst, cam1/*.zst, index.tsv (eyerec.py),
      targets.jsonl, poses.jsonl, ipd.jsonl, own.jsonl, steamvr.jsonl, prompts.jsonl, take.json
  BASE/exports/<round>/

Deleted ranges live in take.json ("deleted": [[from_ns, to_ns], ...]) on CLOCK_MONOTONIC, the
clock of prompts.jsonl, which the window's timeline shows. The frames and the other logs are on
CLOCK_MONOTONIC_RAW seconds; take.json's clock samples ([mono_ns, raw - mono ns]) turn one into
the other. The files keep every frame until export leaves the deleted ones out.

The export (exports/<round>/, built in <round>.partial and moved when it's whole):
  manifest.json                 the round, its profile and session, the recipients, each take
  fit.json                      the fit coach's readings, as recorded
  health.json.age               the opt-in health answers, encrypted (when there are any)
  takes/<id>/take.json          as recorded, plus "export": the deleted ranges in RAW seconds
  takes/<id>/index.tsv          the kept frames; a chunk keeps its number, frames renumbered in it
  takes/<id>/cam0/NNNNNN.zst.age ...   each chunk encrypted to every recipient (recorder/age.py)
  takes/<id>/targets.jsonl, ipd.jsonl, prompts.jsonl   whole: they say what was shown and asked
  takes/<id>/own.jsonl, steamvr.jsonl, poses.jsonl     rows in deleted ranges left out
  SHA256SUMS                    over every file above, the encrypted ones as they are
A chunk none of whose frames are deleted is encrypted as it is; one with some deleted frames is
decompressed, the rest compressed again (zstd 1, as eyerec.py) and encrypted; one with none left
is dropped. Every chunk is decompressed once to check it holds the frames index.tsv says; a
chunk that doesn't, or one index.tsv doesn't name (the recorder was killed before the index
line), is left out and counted in the manifest. session.log isn't exported (it names paths).

Needs the cryptography library to export (recorder/age.py). The rest is standard library.

usage: takes.py [--base DIR] list | takes ROUND | export ROUND
       takes.py unpack EXPORT --identity KEYFILE --out DIR    decrypt an export (the dataset's side)
"""
import argparse
import datetime
import json
import os
import re
import shutil
import sys
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, REPO)
sys.path.insert(0, HERE)
import eyerec  # noqa: E402  (gaze/rec/eyerec.py: the chunks' format)
from recorder import age  # noqa: E402
from recorder.common import tree_version, write_json  # noqa: E402
from recorder.files import (Cancelled, human_bytes, in_ranges, merge_ranges, read_json, sha256_file,  # noqa: E402,F401
                            tree_bytes, tree_stat, write_sums)

DEFAULT_BASE = os.path.expanduser("~/.local/share/frametop/eyes/contrib")
RECIPIENTS = os.path.join(HERE, "recipients.txt")
ROUND_RE = re.compile(r"^\d{8}-\d{6}-[a-z0-9][a-z0-9-]{0,39}$")   # session.py: <time>-<bounty>[-N]
TAKE_RE = re.compile(r"^\d{2,}-[a-z0-9][a-z0-9_-]{0,63}$")
CHUNK_RE = re.compile(r"^(\d{6})\.zst$")
EXPORT_SCHEMA = 1
WHOLE_LOGS = ("targets.jsonl", "ipd.jsonl", "prompts.jsonl")
CUT_LOGS = ("own.jsonl", "steamvr.jsonl", "poses.jsonl")   # "t" in RAW seconds
GAP_S = 0.5            # frames further apart than this are a pause, not recorded time


def tool_version():
    return "ft-eyerec " + (tree_version(REPO) or "unknown")


# ---------------------------------------------------------------- clocks

def raw_offset_ns(t_ns, clock):
    """RAW - MONOTONIC at t_ns (MONOTONIC), from take.json's clock samples: linear between the
    two around it, the nearest outside them, 0 without any."""
    pts = sorted((int(m), int(d)) for m, d in clock or [] if isinstance(m, (int, float)) and isinstance(d, (int, float)))
    if not pts:
        return 0
    if t_ns <= pts[0][0]:
        return pts[0][1]
    for (m0, d0), (m1, d1) in zip(pts, pts[1:]):
        if t_ns <= m1:
            return d0 + (d1 - d0) * (t_ns - m0) // max(1, m1 - m0)
    return pts[-1][1]


def raw_ranges(ranges, clock):
    """Deleted ranges (MONOTONIC ns) as [[from, to], ...] in RAW seconds."""
    return [[(a + raw_offset_ns(a, clock)) / 1e9, (b + raw_offset_ns(b, clock)) / 1e9] for a, b in merge_ranges(ranges)]


def recorded_seconds(times):
    """Time spent recording: the gaps under GAP_S between sorted frame times."""
    total = 0.0
    for a, b in zip(times, times[1:]):
        if b - a < GAP_S:
            total += b - a
    return total


def cut_jsonl(src, dst, ranges):
    """Copy a jsonl log, leaving out rows whose "t" (RAW seconds) is in ranges. Lines that don't
    parse are copied as they are (validate.py reports them)."""
    kept = cut = 0
    with open(src, encoding="utf-8", errors="replace") as f, open(dst, "w", encoding="utf-8", newline="\n") as out:
        for line in f:
            if not line.strip():
                continue
            try:
                t = json.loads(line).get("t")
            except (ValueError, AttributeError):
                t = None
            if isinstance(t, (int, float)) and in_ranges(t, ranges):
                cut += 1
                continue
            out.write(line if line.endswith("\n") else line + "\n")
            kept += 1
    return kept, cut


# ---------------------------------------------------------------- the store

class Store:
    def __init__(self, base=DEFAULT_BASE):
        self.base = os.path.abspath(os.path.expanduser(base))
        self.rounds_dir = os.path.join(self.base, "rounds")
        self.exports_dir = os.path.join(self.base, "exports")
        self.profile_path = os.path.join(self.base, "profile.json")

    # --- paths, checked: ids come from the window, so nothing may climb out of the base
    def round_dir(self, rnd):
        if not ROUND_RE.match(rnd or ""):
            raise ValueError(f"not a round id: {rnd!r}")
        return os.path.join(self.rounds_dir, rnd)

    def take_dir(self, rnd, take):
        if not TAKE_RE.match(take or ""):
            raise ValueError(f"not a take id: {take!r}")
        return os.path.join(self.round_dir(rnd), "takes", take)

    def export_dir(self, rnd):
        self.round_dir(rnd)  # checks the id
        return os.path.join(self.exports_dir, rnd)

    def profile(self):
        return read_json(self.profile_path)

    # --- listing
    def rounds(self):
        """Newest first: [{id, bounty, title, started, status, dry_run, takes, bytes, exported,
        export_stale (changed since), export_bytes}]."""
        out = []
        try:
            names = os.listdir(self.rounds_dir)
        except OSError:
            return out
        for rid in names:
            path = os.path.join(self.rounds_dir, rid)
            if not ROUND_RE.match(rid) or not os.path.isdir(path):
                continue
            meta = read_json(os.path.join(path, "session.json"))
            size, changed = tree_stat(path)
            manifest = os.path.join(self.export_dir(rid), "manifest.json")
            exported = os.path.getmtime(manifest) if os.path.isfile(manifest) else 0.0
            out.append({"id": rid, "bounty": meta.get("bounty", ""), "title": meta.get("title", ""),
                        "started": meta.get("started", ""), "status": meta.get("status", ""),
                        "dry_run": bool(meta.get("dry_run")), "takes": len(self.take_ids(rid)), "bytes": size,
                        "exported": bool(exported), "export_stale": bool(exported) and changed > exported,
                        "export_bytes": tree_bytes(self.export_dir(rid)) if exported else 0})
        return sorted(out, key=lambda r: r["id"], reverse=True)

    def take_ids(self, rnd):
        d = os.path.join(self.round_dir(rnd), "takes")
        try:
            names = os.listdir(d)
        except OSError:
            return []
        return sorted(n for n in names if TAKE_RE.match(n) and os.path.isdir(os.path.join(d, n)))

    def takes(self, rnd):
        """[{id, group, status, frames: [right, left], deleted_frames: [..], ranges, duration_s,
        bytes, started_ns, ended_ns}]."""
        out = []
        for tid in self.take_ids(rnd):
            path = self.take_dir(rnd, tid)
            meta = read_json(os.path.join(path, "take.json"))
            ranges = merge_ranges(meta.get("deleted") or [])
            raw = raw_ranges(ranges, meta.get("clock"))
            times = {c: [] for c in eyerec.CAMERAS}
            for c, _chunk, _i, _slot, t in eyerec.read_index(path):
                if c in times:
                    times[c].append(t)
            for c in times:
                times[c].sort()
            out.append({"id": tid, "group": meta.get("group", tid.split("-", 1)[-1]),
                        "status": meta.get("status", "recording"),
                        "frames": [len(times[c]) for c in eyerec.CAMERAS],
                        "deleted_frames": [sum(1 for t in times[c] if in_ranges(t, raw)) for c in eyerec.CAMERAS],
                        "ranges": ranges, "duration_s": max(recorded_seconds(times[c]) for c in eyerec.CAMERAS),
                        "bytes": sum(tree_bytes(os.path.join(path, "cam%d" % c)) for c in eyerec.CAMERAS),
                        "started_ns": meta.get("started_ns"), "ended_ns": meta.get("ended_ns")})
        return out

    def take_meta(self, rnd, take):
        return read_json(os.path.join(self.take_dir(rnd, take), "take.json"))

    # --- deleted ranges
    def ranges(self, rnd, take):
        return merge_ranges(self.take_meta(rnd, take).get("deleted") or [])

    def _set_ranges(self, rnd, take, ranges):
        path = os.path.join(self.take_dir(rnd, take), "take.json")
        meta = read_json(path)
        if not meta:
            raise RuntimeError(f"no take {rnd}/{take}")
        meta["deleted"] = merge_ranges(ranges)
        write_json(path, meta)
        return meta["deleted"]

    def delete_range(self, rnd, take, from_ns, to_ns):
        """Leave the frames from from_ns to to_ns (CLOCK_MONOTONIC, inclusive) out of the export."""
        return self._set_ranges(rnd, take, self.ranges(rnd, take) + [[int(from_ns), int(to_ns)]])

    def restore_range(self, rnd, take, i):
        """Take deleted range i (in ranges()' order) back."""
        ranges = self.ranges(rnd, take)
        if 0 <= i < len(ranges):
            del ranges[i]
        return self._set_ranges(rnd, take, ranges)

    # --- deleting files
    def delete_take(self, rnd, take):
        path = self.take_dir(rnd, take)
        if os.path.isdir(path):
            shutil.rmtree(path)
        meta_path = os.path.join(self.round_dir(rnd), "session.json")
        meta = read_json(meta_path)
        if take in (meta.get("takes") or []):
            meta["takes"] = [t for t in meta["takes"] if t != take]
            meta.setdefault("deleted_takes", []).append(take)
            write_json(meta_path, meta)

    def delete_round(self, rnd):
        """The round and its export, if any."""
        for path in (self.round_dir(rnd), self.export_dir(rnd)):
            if os.path.isdir(path):
                shutil.rmtree(path)

    def delete_export(self, rnd):
        path = self.export_dir(rnd)
        if os.path.isdir(path):
            shutil.rmtree(path)

    # --- export
    def export(self, rnd, progress=None, cancel=None, keep_notes=False, low_priority=True, recipients=None):
        """Write exports/<round>/ (the module's docstring). progress(fraction 0..1, text) is
        called as it goes; cancel() (or cancel.is_set()) returning true stops it, raising
        Cancelled. Nothing is left behind on failure or cancel. Returns the export's path.
        recipients: "age1..." keys, default recipients.txt's. low_priority: run this thread at
        nice 19 (Linux nice is per thread)."""
        recipients = list(recipients) if recipients else age.read_recipients(RECIPIENTS)
        age._crypto()   # fails now, not half way, without the cryptography library
        cancelled = getattr(cancel, "is_set", cancel) or (lambda: False)
        report = progress or (lambda fraction, text: None)
        src = self.round_dir(rnd)
        if not os.path.isdir(src):
            raise RuntimeError(f"no round {rnd}")
        if low_priority:
            try:
                os.setpriority(os.PRIO_PROCESS, threading.get_native_id(), 19)
            except OSError:
                pass
        final = self.export_dir(rnd)
        work = final + ".partial"
        if os.path.isdir(work):
            shutil.rmtree(work)
        os.makedirs(work)
        try:
            self._export(rnd, src, work, recipients, report, cancelled, keep_notes)
            if os.path.isdir(final):
                shutil.rmtree(final)
            os.replace(work, final)
        except BaseException:
            shutil.rmtree(work, ignore_errors=True)
            raise
        report(1.0, "Done")
        return final

    def _export(self, rnd, src, work, recipients, report, cancelled, keep_notes):
        profile = self.profile()
        meta = read_json(os.path.join(src, "session.json"))
        meta.pop("uploads", None)   # earlier uploads' records (hub.py) aren't part of the contribution
        takes = self.takes(rnd)
        total = sum(t["bytes"] for t in takes) or 1
        done = 0
        entries = []
        for t in takes:
            if cancelled():
                raise Cancelled()
            tdir = self.take_dir(rnd, t["id"])
            out = os.path.join(work, "takes", t["id"])
            os.makedirs(out)
            take_meta = read_json(os.path.join(tdir, "take.json"))
            raw = raw_ranges(t["ranges"], take_meta.get("clock"))
            logs = {}
            for name in WHOLE_LOGS:
                if os.path.isfile(os.path.join(tdir, name)):
                    shutil.copyfile(os.path.join(tdir, name), os.path.join(out, name))
            for name in CUT_LOGS:
                if os.path.isfile(os.path.join(tdir, name)):
                    kept, cut = cut_jsonl(os.path.join(tdir, name), os.path.join(out, name), raw)
                    logs[name] = {"rows": kept, "deleted": cut}

            def step(nbytes, title=t["id"]):
                nonlocal done
                done += nbytes
                report(min(done / total, 0.99), f"Encrypting {title}")
            chunks = self._chunks(tdir, out, raw, recipients, step, cancelled)
            if take_meta:
                take_meta["export"] = {"deleted_raw_s": raw}
                write_json(os.path.join(out, "take.json"), take_meta)
            entries.append({"id": t["id"], "group": t["group"], "status": t["status"],
                            "frames": chunks["frames"], "frames_deleted": chunks["deleted"],
                            "duration_s": round(t["duration_s"], 2), "chunks": chunks["chunks"],
                            "raw_bytes": chunks["raw_bytes"], "left_out": chunks["left_out"], "logs": logs})
        if cancelled():
            raise Cancelled()
        if os.path.isfile(os.path.join(src, "fit.json")):
            shutil.copyfile(os.path.join(src, "fit.json"), os.path.join(work, "fit.json"))
        health = os.path.join(src, "health.json")
        if os.path.isfile(health):
            age.encrypt_file(health, os.path.join(work, "health.json.age"), recipients)
        shown = json.loads(json.dumps(profile))
        shown.pop("health", None)   # only ever encrypted, in health.json.age
        if not keep_notes and isinstance(shown.get("optional"), dict):
            shown["optional"].pop("notes", None)
        manifest = {"schema": EXPORT_SCHEMA, "tool": tool_version(),
                    "exported": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
                    "round_id": rnd, "bounty": meta.get("bounty", ""),
                    "contributor": profile.get("contributor", ""),
                    "consent_version": (profile.get("consent") or {}).get("version", ""),
                    "encryption": {"format": "age-encryption.org/v1", "recipients": recipients,
                                   "files": ["takes/*/cam*/*.zst.age"] + (["health.json.age"] if os.path.isfile(health) else [])},
                    "frame": {"width": eyerec.W, "height": eyerec.H, "format": "u8 grey",
                              "cameras": {"0": "right eye", "1": "left eye"}},
                    "profile": shown, "session": meta, "takes": entries}
        write_json(os.path.join(work, "manifest.json"), manifest)
        report(0.995, "Writing checksums")
        write_sums(work, cancelled)

    @staticmethod
    def _chunks(tdir, out, ranges, recipients, step, cancelled):
        """Each camera's chunks into out/camN/*.zst.age and the kept frames into out/index.tsv."""
        rows = {}
        for c, chunk, i, slot, t in eyerec.read_index(tdir):
            rows.setdefault((c, chunk), []).append((i, slot, t))
        on_disk = set()
        for c in eyerec.CAMERAS:
            try:
                names = os.listdir(os.path.join(tdir, "cam%d" % c))
            except OSError:
                continue
            on_disk.update((c, int(m.group(1))) for m in map(CHUNK_RE.match, names) if m)
        res = {"frames": [0] * len(eyerec.CAMERAS), "deleted": [0] * len(eyerec.CAMERAS), "chunks": 0,
               "raw_bytes": 0, "left_out": {"not_indexed": 0, "missing": 0, "bad": 0}}
        res["left_out"]["not_indexed"] = len(on_disk - set(rows))
        lines = [eyerec.INDEX_HEADER]
        for c, chunk in sorted(rows):
            if cancelled():
                raise Cancelled()
            frames = sorted(rows[(c, chunk)])
            if c not in eyerec.CAMERAS:
                continue
            if (c, chunk) not in on_disk:
                res["left_out"]["missing"] += 1
                continue
            path = os.path.join(tdir, "cam%d" % c, "%06d.zst" % chunk)
            with open(path, "rb") as f:
                packed = f.read()
            step(len(packed))
            try:
                data = eyerec.decompress(packed)
            except Exception:
                data = b""
            if len(data) != len(frames) * eyerec.FRAME or [i for i, _, _ in frames] != list(range(len(frames))):
                res["left_out"]["bad"] += 1
                continue
            kept = [(i, slot, t) for i, slot, t in frames if not in_ranges(t, ranges)]
            res["deleted"][c] += len(frames) - len(kept)
            if not kept:
                continue
            if len(kept) < len(frames):
                packed = eyerec.compress(b"".join(data[i * eyerec.FRAME:(i + 1) * eyerec.FRAME] for i, _, _ in kept))
            os.makedirs(os.path.join(out, "cam%d" % c), exist_ok=True)
            with open(os.path.join(out, "cam%d" % c, "%06d.zst.age" % chunk), "wb") as f:
                f.write(age.encrypt_bytes(packed, recipients))
            lines.extend("%d\t%d\t%d\t%d\t%.6f\n" % (c, chunk, k, slot, t) for k, (_, slot, t) in enumerate(kept))
            res["frames"][c] += len(kept)
            res["chunks"] += 1
            res["raw_bytes"] += len(kept) * eyerec.FRAME
        with open(os.path.join(out, "index.tsv"), "w") as f:
            f.writelines(lines)
        return res


# ---------------------------------------------------------------- the dataset's side

def read_identity(path):
    with open(path) as f:
        for line in f:
            if line.strip().startswith("AGE-SECRET-KEY-1"):
                return line.strip()
    raise ValueError(f"{path} has no age identity")


def unpack(export, identity_path, dest, progress=None):
    """Check an export's SHA256SUMS, then copy it to dest with every .age file decrypted (its
    name without .age). The output is a take folder eyerec.py can read. Returns dest."""
    identity = read_identity(identity_path)
    report = progress or (lambda fraction, text: None)
    sums = {}
    with open(os.path.join(export, "SHA256SUMS")) as f:
        for line in f:
            digest, rel = line.rstrip("\n").split("  ", 1)
            if rel.startswith("/") or ".." in rel.split("/"):
                raise ValueError(f"SHA256SUMS names {rel!r}")
            sums[rel] = digest
    for k, (rel, digest) in enumerate(sorted(sums.items())):
        src = os.path.join(export, rel)
        if sha256_file(src) != digest:
            raise ValueError(f"{rel}: checksum doesn't match")
        out = os.path.join(dest, rel[:-4] if rel.endswith(".age") else rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        if rel.endswith(".age"):
            age.decrypt_file(src, out, identity)
        else:
            shutil.copyfile(src, out)
        report((k + 1) / len(sums), rel)
    return dest


def main():
    ap = argparse.ArgumentParser(description="List, export and unpack eye recorder rounds.")
    ap.add_argument("--base", default=DEFAULT_BASE)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    sub.add_parser("takes").add_argument("round")
    sub.add_parser("export").add_argument("round")
    up = sub.add_parser("unpack", help="check and decrypt an export (needs one of the dataset's keys)")
    up.add_argument("export")
    up.add_argument("--identity", required=True, help="an age identity file")
    up.add_argument("--out", required=True)
    a = ap.parse_args()
    store = Store(a.base)
    if a.cmd == "list":
        for r in store.rounds():
            print(f"{r['id']}  {r['status'] or '-'}  {r['takes']} takes  {human_bytes(r['bytes'])}"
                  + ("  dry run" if r["dry_run"] else "") + ("  exported" if r["exported"] else ""))
    elif a.cmd == "takes":
        for t in store.takes(a.round):
            print(f"{t['id']}  {t['status']}  frames {t['frames']} ({t['deleted_frames']} deleted)  "
                  f"{t['duration_s']:.1f} s  {human_bytes(t['bytes'])}")
    elif a.cmd == "unpack":
        print(unpack(a.export, a.identity, a.out))
    else:
        path = store.export(a.round, progress=lambda f, text: print(f"\r{f * 100:5.1f}% {text:40.40}", end="",
                                                                     flush=True))
        print(f"\n{path}  {human_bytes(tree_bytes(path))}")


if __name__ == "__main__":
    main()
