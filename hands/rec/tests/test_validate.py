#!/usr/bin/env python3
"""Tests for validate.py and hub.py (no network): a good export, and broken ones.

  python3 hands/rec/tests/test_validate.py     (in the dev container, or anywhere with zstd)
"""
import hashlib
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
import uuid
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import hub  # noqa: E402
import session  # noqa: E402
import takes  # noqa: E402
import validate  # noqa: E402

SESSION = "20261002-101500"
DEVICE = {"cv": {"cad_from_cal": {"method": "FrontAndUpperCamPositions", "plus_x": [0.63, -0.77, 0.02],
                                  "plus_z": [-0.45, -0.35, 0.82], "position": [-0.05, -0.02, 0.04]}},
          "head": {"plus_x": [-1, 0, 0], "plus_z": [0, 0, -1], "position": [0, 0, 0]}}
CAMS = [("slam_left", 32, 24), ("slam_right", 32, 24)]


def fhset(t_ns, seed):
    """One FHSET01 record: header, camera headers, pixels."""
    pixels = b"".join(bytes((seed + i) % 256 for i in range(w * h)) for _, w, h in CAMS)
    size = validate.HDR.size + validate.CAM.size * len(CAMS) + len(pixels)
    out = validate.HDR.pack(b"FHSET01\0", len(CAMS), size)
    for name, w, h in CAMS:
        out += validate.CAM.pack(name.encode(), w, h, t_ns - 1000, t_ns)
    return out + pixels


def write_sums(export):
    sums = []
    for root, _, files in os.walk(export):
        for name in files:
            rel = os.path.relpath(os.path.join(root, name), export)
            if rel != "SHA256SUMS":
                with open(os.path.join(root, name), "rb") as f:
                    sums.append((rel, hashlib.sha256(f.read()).hexdigest()))
    with open(os.path.join(export, "SHA256SUMS"), "w") as f:
        for rel, digest in sorted(sums):
            f.write(f"{digest}  {rel}\n")


def make_session(base, contributor=None, sid=None, device=True):
    """A small recorded session in base, as session.py leaves it."""
    contributor = contributor or str(uuid.uuid4())
    os.makedirs(base, exist_ok=True)
    takes.write_json(os.path.join(base, "profile.json"), {
        "schema": 1, "contributor": contributor,
        "consent": {"version": "2026-10-02", "accepted": "2026-10-02T10:00:00+00:00", "adult": True},
        "optional": {"handedness": "right", "notes": ""}})
    sdir = os.path.join(base, "sessions", sid or SESSION)
    os.makedirs(os.path.join(sdir, "takes"))
    takes.write_json(os.path.join(sdir, "session.json"), {
        "schema": 1, "tool": "ft-handrec test", "started": "2026-10-02T10:15:00+0000", "contributor": contributor,
        "lighting": {"chosen": "room", "ring": {}},
        "checklist": {"objects": ["pencil"], "own_objects": ["stapler"], "controllers": "none", "sleeves": "",
                      "rings": False, "watch": False, "notes": ""},
        "device": {"steamos": "3.8", "steamvr": "r25358740+28b72a4f-1", "cameras": [{"name": n, "width": w, "height": h}
                                                                 for n, w, h in CAMS]},
        "calibration_removed": ["serial"], "takes": ["01-hand-size", "02-no-hands"], "status": "done",
        "sides": {"swapped": False, "decided_by": "auto", "state": "confirmed"}})
    takes.write_json(os.path.join(sdir, "calibration.json"),
                     {"cameras": [{"name": "slam_left", "intrinsics": [1.0, 2.0, 3.0]}]})
    if device:
        takes.write_json(os.path.join(sdir, "device.json"), DEVICE)
    for k, tid in enumerate(("01-hand-size", "02-no-hands")):
        tdir = os.path.join(sdir, "takes", tid)
        os.makedirs(tdir)
        t0 = 10 ** 12 * (k + 1)
        with open(os.path.join(tdir, "sets.bin"), "wb") as f:
            for i in range(30):
                f.write(fhset(t0 + i * 100_000_000, i))
        with open(os.path.join(tdir, "prompts.jsonl"), "w") as f:
            f.write(json.dumps({"t": t0, "event": "take", "section": tid[3:], "take": tid}) + "\n")
            f.write(json.dumps({"t": t0 + 5, "event": "end", "status": "complete"}) + "\n")
        with open(os.path.join(tdir, "poses.jsonl"), "w") as f:
            for i in range(10):
                f.write(json.dumps({"t": t0 + i, "hmd": {"m": [0.0] * 12, "r": 200, "ok": True},
                                    "left": None, "right": None}) + "\n")
        takes.write_json(os.path.join(tdir, "take.json"), {"section": tid[3:], "title": tid, "started_ns": t0,
                                                            "ended_ns": t0 + 3 * 10 ** 9, "status": "complete",
                                                            "deleted": [], "notes": ""})
    return contributor


class ValidateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not takes.find_zstd():
            raise unittest.SkipTest("no zstd")
        cls.tmp = tempfile.mkdtemp(prefix="handrec-validate-test-")
        cls.base = os.path.join(cls.tmp, "base")
        cls.contributor = make_session(cls.base)
        cls.store = takes.Store(cls.base)
        # One deleted range, so the export leaves sets out.
        index = takes.take_index(cls.store.take_dir(SESSION, "01-hand-size"))
        cls.store.delete_range(SESSION, "01-hand-size", index.time_ns(3), index.time_ns(5))
        cls.export = cls.store.export(SESSION, low_priority=False)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def copy(self):
        d = tempfile.mkdtemp(dir=self.tmp)
        dest = os.path.join(d, SESSION)
        shutil.copytree(self.export, dest)
        return dest

    def assertError(self, report, text):
        self.assertTrue(any(text in e for e in report.errors), f"no error with {text!r}: {report.errors}")

    def test_good(self):
        r = validate.validate(self.export)
        self.assertEqual(r.errors, [])
        self.assertEqual(r.warnings, [])
        self.assertTrue(os.path.isfile(os.path.join(self.export, "device.json")))
        self.assertEqual(r.summary["takes"], 2)
        self.assertEqual(r.summary["sets"], 57)
        self.assertEqual(r.summary["consent_version"], "2026-10-02")
        self.assertEqual(r.summary["contributor"], self.contributor)

    def test_extra_file(self):
        d = self.copy()
        with open(os.path.join(d, "notes.txt"), "w") as f:
            f.write("hello\n")
        os.makedirs(os.path.join(d, "takes", "01-hand-size", "extra"))
        r = validate.validate(d)
        self.assertError(r, "notes.txt: not an allowed file")
        self.assertError(r, "not in SHA256SUMS: notes.txt")
        self.assertError(r, "extra/: not an allowed folder")

    def test_symlink(self):
        d = self.copy()
        os.symlink("/etc/hostname", os.path.join(d, "takes", "01-hand-size", "take.json.link"))
        self.assertError(validate.validate(d), "a symlink")

    def test_bad_checksum(self):
        d = self.copy()
        path = os.path.join(d, "takes", "02-no-hands", "poses.jsonl")
        with open(path, "r+b") as f:
            f.seek(5)
            f.write(b"9")   # "t": 2... becomes "t": 9...: still valid JSON
        r = validate.validate(d)
        self.assertError(r, "checksum mismatch: takes/02-no-hands/poses.jsonl")
        self.assertEqual(len(r.errors), 1, r.errors)

    def test_calibration_serial(self):
        d = self.copy()
        takes.write_json(os.path.join(d, "calibration.json"),
                         {"cameras": [{"name": "slam_left", "serial": "X", "label": "unit 4H2K19A7731"}]})
        write_sums(d)
        r = validate.validate(d)
        self.assertError(r, "calibration.json: identifying fields left in")
        self.assertIn("cameras[0].serial", r.errors[0])
        self.assertIn("cameras[0].label", r.errors[0])
        self.assertEqual(len(r.errors), 1, r.errors)

    def test_truncated_zst(self):
        d = self.copy()
        path = os.path.join(d, "takes", "01-hand-size", "sets.bin.zst")
        os.truncate(path, os.path.getsize(path) // 2)
        write_sums(d)
        r = validate.validate(d)
        self.assertTrue(any("sets.bin.zst" in e and ("cut short" in e or "set " in e or "decompress" in e)
                            for e in r.errors), r.errors)

    def test_truncated_zst_command(self):
        """The same with the zstd program (Python before 3.14)."""
        d = self.copy()
        path = os.path.join(d, "takes", "01-hand-size", "sets.bin.zst")
        os.truncate(path, os.path.getsize(path) // 2)
        with mock.patch.object(validate, "_zstd", None):
            r = validate.validate(d)
            self.assertTrue(any("sets.bin.zst" in e and "checksum" not in e for e in r.errors), r.errors)
            self.assertEqual(validate.validate(self.export).errors, [])

    def test_garbage_records(self):
        d = self.copy()
        rec = bytearray(fhset(10 ** 9, 0))
        rec[validate.HDR.size:validate.HDR.size + 16] = b"slam left!\0\0\0\0\0\0"
        zst = os.path.join(d, "takes", "02-no-hands", "sets.bin.zst")
        with open(zst + ".raw", "wb") as f:
            f.write(bytes(rec))
        os.system(f"zstd -q -f {zst}.raw -o {zst} && rm {zst}.raw")
        write_sums(d)
        self.assertError(validate.validate(d), "a camera name that isn't one")

    def test_manifest(self):
        d = self.copy()
        path = os.path.join(d, "manifest.json")
        with open(path) as f:
            m = json.load(f)
        m["contributor"] = m["profile"]["contributor"] = str(uuid.uuid1())
        m["consent_version"] = ""
        m["profile"]["consent"]["version"] = ""
        m["takes"][0]["sets"] += 1
        m["session"]["dry_run"] = True
        m["session"]["device"]["serial_number"] = "x"
        del m["exported"]
        takes.write_json(path, m)
        with open(os.path.join(d, "takes", "02-no-hands", "prompts.jsonl"), "a") as f:
            f.write("{not json\n")
        write_sums(d)
        r = validate.validate(d)
        self.assertError(r, "isn't a random uuid4")
        self.assertError(r, "the consent version is empty")
        self.assertError(r, "27 sets, the manifest says 28")
        self.assertError(r, "dry-run session")
        self.assertError(r, "identifying fields: session.device.serial_number")
        self.assertError(r, "exported is missing")
        self.assertError(r, "prompts.jsonl line 3: not valid JSON")

    def test_region(self):
        """From consent 2026-10-03, the residency confirmation must be there."""
        d = self.copy()
        path = os.path.join(d, "manifest.json")
        with open(path) as f:
            m = json.load(f)
        m["consent_version"] = m["profile"]["consent"]["version"] = "2026-10-03"
        takes.write_json(path, m)
        write_sums(d)
        self.assertError(validate.validate(d), "Illinois, Texas or Washington")
        m["profile"]["consent"]["region_ok"] = True
        takes.write_json(path, m)
        write_sums(d)
        self.assertEqual(validate.validate(d).errors, [])

    def test_device(self):
        d = self.copy()
        bad = json.loads(json.dumps(DEVICE))
        bad["device_serial_number"] = "X"
        bad["head"]["position"] = [0, 0]
        takes.write_json(os.path.join(d, "device.json"), bad)
        write_sums(d)
        r = validate.validate(d)
        self.assertError(r, "expected cv and head only")
        self.assertError(r, "head.position isn't 3 numbers")
        self.assertError(r, "device.json: identifying fields: device_serial_number")

    def test_no_device(self):
        """Sessions from before device.json still pass, with a warning."""
        d = self.copy()
        os.remove(os.path.join(d, "device.json"))
        write_sums(d)
        r = validate.validate(d)
        self.assertEqual(r.errors, [])
        self.assertTrue(any("no device.json" in w for w in r.warnings), r.warnings)

    def test_zstandard(self):
        """The zstandard package's path (the maintainer's Windows PC), good and truncated."""
        if validate._zstandard is None:
            self.skipTest("no zstandard")
        with mock.patch.object(validate, "_zstd", None):
            self.assertEqual(validate.validate(self.export).errors, [])
            d = self.copy()
            path = os.path.join(d, "takes", "01-hand-size", "sets.bin.zst")
            os.truncate(path, os.path.getsize(path) // 2)
            write_sums(d)
            self.assertError(validate.validate(d), "sets.bin.zst: the zstd stream is cut short")

    def test_frames(self):
        """Several zstd frames one after another (zstd -T2 can write them) read as one stream."""
        if validate._zstd is None and validate._zstandard is None:
            self.skipTest("no compression.zstd or zstandard")
        d = self.copy()
        zst = os.path.join(d, "takes", "02-no-hands", "sets.bin.zst")
        with open(zst, "rb") as f:
            one = f.read()
        with open(zst, "ab") as f:
            f.write(one)
        with open(os.path.join(d, "manifest.json")) as f:
            m = json.load(f)
        m["takes"][1]["sets"] *= 2
        m["takes"][1]["raw_bytes"] *= 2
        takes.write_json(os.path.join(d, "manifest.json"), m)
        write_sums(d)
        r = validate.validate(d)
        self.assertEqual([e for e in r.errors if "go back" not in e], [])

    def test_cli(self):
        self.assertEqual(os.system(f"{sys.executable} {validate.__file__} {self.export} --json >/dev/null"), 0)
        d = self.copy()
        os.remove(os.path.join(d, "manifest.json"))
        self.assertNotEqual(os.system(f"{sys.executable} {validate.__file__} {d} >/dev/null"), 0)


class ExportJsonlTest(unittest.TestCase):
    """takes.export_jsonl: deleted ranges and the controllers when the checklist says none."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="handrec-jsonl-test-")
        ctrl = {"m": [0.0] * 12, "r": 200, "ok": True}
        with open(os.path.join(self.tmp, "poses.jsonl"), "w") as f:
            for t in range(10):
                f.write(json.dumps({"t": t, "hmd": ctrl, "left": ctrl, "right": ctrl}) + "\n")
        with open(os.path.join(self.tmp, "prompts.jsonl"), "w") as f:
            for e in ({"t": 1, "event": "prompt", "id": "a"}, {"t": 4, "event": "feedback", "left": True},
                      {"t": 7, "event": "feedback", "left": True, "controller": {"left": "ok"}}):
                f.write(json.dumps(e) + "\n")
        os.makedirs(os.path.join(self.tmp, "out"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_export(self, name, keep):
        out = os.path.join(self.tmp, "out", name)
        takes.export_jsonl(os.path.join(self.tmp, name), out, [[3, 5]], keep)
        with open(out) as f:
            return [json.loads(line) for line in f]

    def test_no_controllers(self):
        poses = self.run_export("poses.jsonl", False)
        self.assertEqual([p["t"] for p in poses], [0, 1, 2, 6, 7, 8, 9])   # 3-5 deleted
        self.assertTrue(all(p["left"] is None and p["right"] is None and p["hmd"] for p in poses))
        prompts = self.run_export("prompts.jsonl", False)
        self.assertEqual([e["t"] for e in prompts], [1, 7])   # the feedback at 4 was in a deleted range
        self.assertNotIn("controller", prompts[1])

    def test_with_controllers(self):
        poses = self.run_export("poses.jsonl", True)
        self.assertTrue(all(p["left"] and p["right"] for p in poses))
        self.assertIn("controller", self.run_export("prompts.jsonl", True)[1])


class SessionFilesTest(unittest.TestCase):
    """session.py's device.json, and session ids with a suffix."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="handrec-session-test-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_write_device(self):
        full = dict(json.loads(json.dumps(DEVICE)), device_serial_number="ABC12345678", display_edid="00ff",
                    model_number="Frame")
        src = os.path.join(self.tmp, "device_config.json")
        takes.write_json(src, full)
        s = session.Session.__new__(session.Session)
        s.session_dir, s._log = self.tmp, lambda text: None
        with mock.patch.object(session, "host_path", lambda p: src if p.endswith("device_config.json") else None):
            self.assertEqual(s._write_device(), [])
        with open(os.path.join(self.tmp, "device.json")) as f:
            self.assertEqual(json.load(f), DEVICE)
        with mock.patch.object(session, "host_path", lambda p: None):
            self.assertEqual(s._write_device(), [])

    def test_suffix_id(self):
        if not takes.find_zstd():
            self.skipTest("no zstd")
        sid = SESSION + "-2"
        make_session(self.tmp, sid=sid)
        store = takes.Store(self.tmp)
        self.assertEqual([x["id"] for x in store.sessions()], [sid])
        path = store.export(sid, low_priority=False)
        self.assertEqual(validate.validate(path).errors, [])


class LoginTest(unittest.TestCase):
    """hub.login: the link and code go out first, the token is saved by huggingface_hub and never
    passed on, and a refused login is a "login" error. huggingface_hub's helpers are faked."""

    def setUp(self):
        try:
            import huggingface_hub._login
            import huggingface_hub.utils._oauth_device
        except ImportError as e:
            self.skipTest(f"no huggingface_hub browser login: {e}")
        self.login_mod = huggingface_hub._login
        self.device = huggingface_hub.utils._oauth_device

    def test_code_then_saved(self):
        told, saved = [], []
        info = {"verification_uri_complete": "https://hf.co/oauth/device", "user_code": "ABCD-1234", "expires_in": 300}
        with mock.patch.object(self.device, "request_device_code", return_value=info), \
                mock.patch.object(self.device, "poll_device_token", return_value={"access_token": "secret"}), \
                mock.patch.object(self.login_mod, "_save_oauth_token", side_effect=saved.append), \
                mock.patch.object(hub, "whoami", return_value={"name": "someone", "role": ""}):
            who = hub.login(lambda phase, text, **extra: told.append(dict(extra, phase=phase)))
        self.assertEqual(told, [{"phase": "code", "url": info["verification_uri_complete"], "code": "ABCD-1234",
                                 "expires_in": 300}])
        self.assertEqual(saved, [{"access_token": "secret"}])
        self.assertEqual(who["name"], "someone")
        self.assertNotIn("secret", json.dumps(told) + json.dumps(who))

    def test_refused(self):
        from huggingface_hub.errors import DeviceCodeError
        info = {"verification_uri_complete": "u", "user_code": "c", "expires_in": 300}
        with mock.patch.object(self.device, "request_device_code", return_value=info), \
                mock.patch.object(self.device, "poll_device_token", side_effect=DeviceCodeError("access_denied")):
            with self.assertRaises(hub.HubError) as cm:
                hub.login()
        self.assertEqual(cm.exception.kind, "login")


class HubTest(unittest.TestCase):
    """hub.py without the network: the dry run, the draft gate, upload records."""

    def setUp(self):
        if not takes.find_zstd():
            self.skipTest("no zstd")
        self.tmp = tempfile.mkdtemp(prefix="handrec-hub-test-")
        self.contributor = make_session(self.tmp)
        self.store = takes.Store(self.tmp)
        self.store.export(SESSION, low_priority=False)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_dry_run(self):
        logged = []
        with mock.patch.dict(os.environ, {hub.DATASET_ENV: "someone/test-hands"}):
            result = hub.upload(self.store, SESSION, dry_run=True, log=logged.append)
        self.assertTrue(result["dry_run"])
        self.assertEqual(result["repo"], "someone/test-hands")
        self.assertEqual(result["path_in_repo"], f"contributions/{self.contributor}/{SESSION}")
        self.assertIn("- Takes: 2", result["commit_description"])
        self.assertIn("- Consent version: 2026-10-02", result["commit_description"])
        self.assertIn("- Objects: pencil (+1 of their own)", result["commit_description"])
        self.assertTrue(any("manifest.json" in line for line in logged))
        self.assertEqual(hub.uploads(self.store, SESSION), [])   # a dry run records nothing

    def test_consent_line(self):
        """The description says which consent the upload goes under, and the older one it was
        recorded under when they differ."""
        self.assertEqual(hub.consent_line({"consent_version": "2026-10-14"}), "2026-10-14")
        self.assertEqual(hub.consent_line({"consent_version": "2026-10-14", "recorded_consent_version": "2026-10-14"}),
                         "2026-10-14")
        self.assertEqual(hub.consent_line({"consent_version": "2026-10-14", "recorded_consent_version": "2026-10-03"}),
                         "2026-10-14 (recorded under 2026-10-03)")

    def test_dataset_env(self):
        self.assertEqual(hub.dataset_id(), hub.HF_DATASET)
        with mock.patch.dict(os.environ, {hub.DATASET_ENV: "not a repo"}):
            self.assertRaises(hub.HubError, hub.dataset_id)

    def test_draft_gate(self):
        with mock.patch.object(hub, "texts_draft", return_value=True), \
                mock.patch.dict(os.environ, {hub.ALLOW_ENV: ""}):
            with self.assertRaises(hub.HubError) as cm:
                hub.upload(self.store, SESSION)
            self.assertEqual(cm.exception.kind, "closed")
            self.assertFalse(hub.upload_allowed())
        with mock.patch.object(hub, "texts_draft", return_value=True), \
                mock.patch.dict(os.environ, {hub.ALLOW_ENV: "1"}):
            self.assertTrue(hub.upload_allowed())

    def test_invalid_blocks(self):
        with open(os.path.join(self.store.export_dir(SESSION), "extra.bin"), "w") as f:
            f.write("x")
        with self.assertRaises(hub.HubError) as cm:
            hub.upload(self.store, SESSION, dry_run=True)
        self.assertEqual(cm.exception.kind, "invalid")
        self.assertTrue(cm.exception.errors)

    def test_record_and_duplicate(self):
        export = self.store.export_dir(SESSION)
        sha = hub.export_sha(export)
        self.assertEqual(len(sha), 64)
        hub.record_upload(self.store, SESSION, {"repo": "a/b", "pr_url": "https://huggingface.co/datasets/a/b/discussions/1",
                                                "uploaded": hub.now_iso(), "export_sha": sha})
        # The upload record doesn't make the export look out of date...
        self.assertFalse(self.store.sessions()[0]["export_stale"])
        # ...and the next upload of the same export stops, unless asked again.
        with self.assertRaises(hub.HubError) as cm:
            hub.upload(self.store, SESSION, dry_run=True)
        self.assertEqual(cm.exception.kind, "duplicate")
        self.assertTrue(hub.upload(self.store, SESSION, dry_run=True, again=True)["dry_run"])
        # A new export leaves the records out of its manifest.
        self.store.export(SESSION, low_priority=False)
        with open(os.path.join(export, "manifest.json")) as f:
            self.assertNotIn("uploads", json.load(f)["session"])

    def test_pull_request_first(self):
        """The pull request opens before the files go; a retry goes on in it."""
        calls = []

        class Pr:
            def __init__(self, num, status="draft"):
                self.num, self.status, self.is_pull_request = num, status, True
                self.url = f"https://huggingface.co/datasets/a/b/discussions/{num}"

        class Api:
            fail = True
            status = "draft"

            def whoami(self):
                return {"name": "someone", "auth": {"accessToken": {"role": "write"}}}

            def auth_check(self, repo, repo_type=None):
                pass

            def create_pull_request(self, repo, title, description=None, repo_type=None):
                calls.append("create")
                return Pr(7)

            def get_discussion_details(self, repo, num, repo_type=None):
                return Pr(num, Api.status)

            def upload_folder(self, **kw):
                calls.append(("upload", kw["revision"], kw.get("create_pr")))
                if Api.fail:
                    raise ConnectionResetError("reset")

            def change_discussion_status(self, repo, num, status, repo_type=None):
                calls.append(("status", num, status))

        fake = mock.Mock(HfApi=Api)
        seen = []
        env = {hub.DATASET_ENV: "a/b", hub.ALLOW_ENV: "1"}
        with mock.patch.dict(os.environ, env), mock.patch.object(hub, "_hf", return_value=fake):
            with self.assertRaises(hub.HubError):
                hub.upload(self.store, SESSION, progress=lambda phase, text, fraction=None, **extra: seen.append(
                    (phase, extra.get("pr_url"))))
            self.assertIn(("opened", "https://huggingface.co/datasets/a/b/discussions/7"), seen)
            [rec] = hub.uploads(self.store, SESSION)
            self.assertEqual((rec["status"], rec["pr_num"]), ("started", 7))
            self.assertIsNone(hub.previous_upload(self.store, SESSION, rec["export_sha"]))   # not a duplicate
            Api.fail = False
            result = hub.upload(self.store, SESSION)
        self.assertEqual(calls, ["create", ("upload", "refs/pr/7", None), ("upload", "refs/pr/7", None),
                                 ("status", 7, "open")])   # the retry reused #7
        self.assertEqual(result["pr_url"], "https://huggingface.co/datasets/a/b/discussions/7")
        [rec] = hub.uploads(self.store, SESSION)
        self.assertEqual(rec["status"], "done")
        with mock.patch.dict(os.environ, env), self.assertRaises(hub.HubError) as cm:
            hub.upload(self.store, SESSION, dry_run=True)
        self.assertEqual(cm.exception.kind, "duplicate")

    def test_explain(self):
        try:
            import httpx
            from huggingface_hub import errors as hf
        except ImportError:
            self.skipTest("no huggingface_hub")

        def response(code):
            return httpx.Response(code, request=httpx.Request("POST", "https://huggingface.co/api/x"))
        cases = [(hf.GatedRepoError("gated", response=response(403)), "terms"),
                 (hf.RepositoryNotFoundError("nope", response=response(404)), "not-found"),
                 (hf.HfHubHTTPError("no", response=response(403)), "permission"),
                 (hf.HfHubHTTPError("bad token", response=response(401)), "login"),
                 (hf.LocalTokenNotFoundError("none"), "login"),
                 (httpx.ConnectError("down"), "network"),
                 (ConnectionResetError("reset"), "network")]
        for exc, kind in cases:
            e = hub.explain(exc, "a/b")
            self.assertEqual(e.kind, kind, exc)
        self.assertIn("https://huggingface.co/datasets/a/b", hub.explain(cases[0][0], "a/b").link)


if __name__ == "__main__":
    unittest.main()
