#!/usr/bin/env python3
"""Tests for freeing space (2026-10-17): the contributions log (recorder/contributions.py) that
outlives deleted sessions, Store.free_session and its refusals, the export's tighter space check
and its stop before the disk fills, and hub.py adding each finished upload to the log.

  python3 hands/rec/tests/test_free_space.py     (anywhere with zstd)
"""
import datetime
import os
import shutil
import sys
import tempfile
import time
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import takes  # noqa: E402
import hub  # noqa: E402
from recorder import contributions  # noqa: E402
from test_validate import make_session, SESSION  # noqa: E402

PR = "https://huggingface.co/datasets/DeeJanuz/frametop-hands/discussions/12"


def iso(t):
    return datetime.datetime.fromtimestamp(t).astimezone().isoformat(timespec="seconds")


class LogTest(unittest.TestCase):
    def setUp(self):
        self.base = tempfile.mkdtemp(prefix="handrec-log-test-")

    def tearDown(self):
        shutil.rmtree(self.base, ignore_errors=True)

    def entry(self, **kw):
        c = {"session": SESSION, "repo": "a/b", "pr_url": PR, "pr_num": 12, "uploaded": "2026-10-15T13:04:38+00:00",
             "export_sha": "s", "export_bytes": 0, "bounty": {}, "kind": "public", "debug": False, "started": "",
             "minutes": None, "local_deleted": ""}
        c.update(kw)
        return c

    def test_add_once(self):
        self.assertEqual(contributions.add(self.base, [self.entry()]), 1)
        self.assertEqual(contributions.add(self.base, [self.entry()]), 0)
        self.assertEqual(len(contributions.load(self.base)), 1)

    def test_add_fills_gaps_only(self):
        contributions.add(self.base, [self.entry()])
        contributions.add(self.base, [self.entry(export_bytes=99, pr_url="other")])
        c, = contributions.load(self.base)
        self.assertEqual(c["export_bytes"], 99)
        self.assertEqual(c["pr_url"], PR)

    def test_two_pull_requests_two_entries(self):
        contributions.add(self.base, [self.entry(), self.entry(pr_num=13, pr_url=PR[:-2] + "13")])
        self.assertEqual(len(contributions.load(self.base)), 2)

    def test_mark_deleted(self):
        contributions.add(self.base, [self.entry()])
        self.assertTrue(contributions.mark_deleted(self.base, SESSION, "then"))
        self.assertEqual(contributions.load(self.base)[0]["local_deleted"], "then")
        self.assertFalse(contributions.mark_deleted(self.base, "20990101-000000"))

    def test_unreadable_log(self):
        with open(contributions.path(self.base), "w") as f:
            f.write("{not json")
        self.assertEqual(contributions.load(self.base), [])

    def test_done_uploads(self):
        meta = {"uploads": [{"status": "started", "pr_url": PR}, {"status": "done", "pr_url": PR},
                            {"pr_url": PR, "uploaded": "old"}, {"status": "done", "pr_url": ""}, "junk"]}
        self.assertEqual(len(contributions.done_uploads(meta)), 2)


class FreeTest(unittest.TestCase):
    def setUp(self):
        self.base = tempfile.mkdtemp(prefix="handrec-free-test-")
        make_session(self.base)
        self.store = takes.Store(self.base)
        self.sdir = self.store.session_dir(SESSION)
        self.meta_path = os.path.join(self.sdir, "session.json")

    def tearDown(self):
        shutil.rmtree(self.base, ignore_errors=True)

    def upload(self, **kw):
        record = {"repo": "DeeJanuz/frametop-hands", "pr_url": PR, "pr_num": 12, "uploaded": iso(time.time() + 5),
                  "export_sha": "x", "status": "done"}
        record.update(kw)
        meta = takes.read_json(self.meta_path)
        meta.setdefault("uploads", []).append(record)
        meta["bounty"] = {"id": "index-taps", "title": "Index taps"}
        st = os.stat(self.meta_path)
        takes.write_json(self.meta_path, meta)
        os.utime(self.meta_path, ns=(st.st_atime_ns, st.st_mtime_ns))   # as record_upload keeps it

    def row(self):
        return next(s for s in self.store.sessions() if s["id"] == SESSION)

    def test_not_uploaded(self):
        self.assertFalse(self.row()["freeable"])
        with self.assertRaises(ValueError):
            self.store.free_session(SESSION)
        self.assertTrue(os.path.isdir(self.sdir))

    def test_started_upload_isnt_enough(self):
        self.upload(status="started")
        self.assertFalse(self.row()["uploaded"])
        self.assertFalse(self.row()["freeable"])

    def test_free_uploaded(self):
        self.upload()
        os.makedirs(self.store.export_dir(SESSION))
        row = self.row()
        self.assertTrue(row["uploaded"] and row["freeable"])
        self.assertEqual(row["pr_url"], PR)
        self.assertEqual([f["id"] for f in self.store.freeable()], [SESSION])
        self.assertGreater(self.store.free_session(SESSION), 0)
        self.assertFalse(os.path.exists(self.sdir))
        self.assertFalse(os.path.exists(self.store.export_dir(SESSION)))
        c, = self.store.contributions()
        self.assertEqual((c["session"], c["pr_url"], c["bounty"]["id"], c["local"]), (SESSION, PR, "index-taps", False))
        self.assertTrue(c["local_deleted"])
        self.assertEqual(self.store.freeable(), [])

    def test_changed_since_upload(self):
        self.upload(uploaded=iso(time.time() - 3600))
        self.assertTrue(self.row()["uploaded"])
        self.assertFalse(self.row()["freeable"])
        with self.assertRaises(ValueError):
            self.store.free_session(SESSION)
        self.assertTrue(os.path.isdir(self.sdir))

    def test_no_upload_time_uses_the_export(self):
        path = self.store.export(SESSION, low_priority=False)
        sha = takes.export_sha(path)
        self.upload(uploaded="", export_sha="other")
        self.assertFalse(self.row()["freeable"])
        self.upload(uploaded="", export_sha=sha, pr_num=13)
        self.assertTrue(self.row()["freeable"])

    def test_record_first(self):
        """The log can't be written: nothing is deleted."""
        self.upload()
        with mock.patch.object(contributions, "add", side_effect=OSError(28, "No space left on device")):
            with self.assertRaises(OSError):
                self.store.free_session(SESSION)
        self.assertTrue(os.path.isdir(self.sdir))

    def test_backfill(self):
        self.upload()
        self.assertEqual(self.store.log_uploads(), 1)
        self.assertEqual(self.store.log_uploads(), 0)
        self.assertTrue(self.store.contributions()[0]["local"])

    def test_bad_ids(self):
        for sid in ("../x", "", "20261002-101500/.."):
            with self.assertRaises(ValueError):
                self.store.free_session(sid)


class ExportRoomTest(unittest.TestCase):
    def setUp(self):
        if not takes.find_zstd():
            self.skipTest("no zstd")
        self.base = tempfile.mkdtemp(prefix="handrec-room-test-")
        make_session(self.base)
        self.store = takes.Store(self.base)
        self.partial = self.store.export_dir(SESSION) + ".partial"

    def tearDown(self):
        shutil.rmtree(self.base, ignore_errors=True)
        takes._room_checked[0] = 0.0

    def test_needs_the_export_and_a_reserve(self):
        kept = self.store.kept_bytes(SESSION)
        self.assertEqual(takes.export_need(kept), int(kept * takes.EXPORT_RATIO_NEED + takes.EXPORT_RESERVE))
        with mock.patch.object(takes, "free_bytes", return_value=takes.export_need(kept) - 1):
            with self.assertRaises(takes.DiskFull) as cm:
                self.store.export(SESSION, low_priority=False)
        self.assertIn("already uploaded can be deleted", str(cm.exception))
        self.assertFalse(os.path.exists(self.partial))

    def test_stops_before_full(self):
        """The disk gets down to EXPORT_MIN_FREE as it goes: the export stops and cleans up."""
        real = takes.free_bytes
        takes._room_checked[0] = 0.0

        def free(path):
            return 10 ** 12 if path == self.store.exports_dir else int(takes.EXPORT_MIN_FREE) - 1
        with mock.patch.object(takes, "free_bytes", side_effect=free):
            with self.assertRaises(takes.DiskFull) as cm:
                self.store.export(SESSION, low_priority=False)
        self.assertTrue(str(cm.exception).startswith("The disk got nearly full, so the export stopped"))
        self.assertFalse(os.path.exists(self.partial))
        self.assertIs(takes.free_bytes, real)


class HubLogTest(unittest.TestCase):
    """hub.upload adds the finished upload to the contributions log."""

    def setUp(self):
        if not takes.find_zstd():
            self.skipTest("no zstd")
        self.base = tempfile.mkdtemp(prefix="handrec-hublog-test-")
        make_session(self.base)
        self.store = takes.Store(self.base)
        self.store.export(SESSION, low_priority=False)

    def tearDown(self):
        shutil.rmtree(self.base, ignore_errors=True)

    def fake_push(self, hf, repo, export, path_in_repo, message, description, meta_path, sha, nfiles, nbytes, tell,
                  write_json, human_bytes):
        record = {"repo": repo, "pr_url": PR, "pr_num": 12, "started": hub.now_iso(), "export_sha": sha,
                  "uploaded": hub.now_iso(), "status": "done"}
        hub.rhub.record_upload(meta_path, record, write_json)
        return mock.Mock(num=12), {"name": "someone"}, record

    def test_logged(self):
        with mock.patch.object(hub, "upload_allowed", return_value=True), \
                mock.patch.object(hub, "_hf", return_value=None), \
                mock.patch.object(hub.rhub, "push", side_effect=self.fake_push):
            result = hub.upload(self.store, SESSION)
        self.assertEqual(result["pr_url"], PR)
        c, = contributions.load(self.base)
        self.assertEqual((c["session"], c["pr_num"], c["kind"]), (SESSION, 12, "public"))
        self.assertGreater(c["export_bytes"], 0)

    def test_dry_run_not_logged(self):
        hub.upload(self.store, SESSION, dry_run=True)
        self.assertEqual(contributions.load(self.base), [])


class BackendTest(unittest.TestCase):
    """The window's side: what can be freed, the free slots, the testing session's count from the
    log, and the backfill when the window opens. Needs PySide6 (the dev container)."""

    def setUp(self):
        try:
            import ft_handrec
        except ImportError as e:
            self.skipTest(f"no PySide6: {e}")
        self.ft = ft_handrec
        self.base = tempfile.mkdtemp(prefix="handrec-free-backend-test-")
        make_session(self.base)
        self.store = takes.Store(self.base)
        meta_path = os.path.join(self.store.session_dir(SESSION), "session.json")
        meta = takes.read_json(meta_path)
        meta["debug"] = {"use": "testing"}
        meta["status"] = "done"
        meta["uploads"] = [{"repo": "DeeJanuz/frametop-hands-test", "pr_url": PR, "pr_num": 3, "status": "done",
                            "uploaded": iso(time.time() + 5), "export_sha": "x"}]
        st = os.stat(meta_path)
        takes.write_json(meta_path, meta)
        os.utime(meta_path, ns=(st.st_atime_ns, st.st_mtime_ns))
        os.environ["FT_HANDREC_FRAMETOP_HANDS"] = "1"
        self.addCleanup(os.environ.pop, "FT_HANDREC_FRAMETOP_HANDS", None)
        self.backend = ft_handrec.Backend(self.store, {"dry_run": True}, ignore_cameras=True)
        self.messages = []
        self.backend.message.connect(lambda text, error: self.messages.append((text, error)))

    def tearDown(self):
        shutil.rmtree(self.base, ignore_errors=True)

    def test_backfilled_on_open(self):
        self.assertEqual([c["session"] for c in contributions.load(self.base)], [SESSION])

    def test_free_all(self):
        self.assertEqual(self.backend.freeableCount, 1)
        self.assertTrue(self.backend.freeableText)
        row, = self.backend.sessions
        self.assertTrue(row["freeable"])
        self.assertEqual(row["uploadText"], "uploaded")
        self.backend.freeAllUploaded()
        self.assertFalse(os.path.exists(self.store.session_dir(SESSION)))
        self.assertEqual(self.backend.freeableCount, 0)
        self.assertEqual(self.backend.freeableText, "")
        self.assertFalse(self.messages[-1][1], self.messages)
        c, = self.backend.contributions
        self.assertEqual((c["title"], c["local"], c["testing"]), ("Testing session", False, True))
        self.assertTrue(c["deletedText"])
        debug = next(b for b in self.backend.bounties if b["id"] == self.ft.DEBUG_ID)
        self.assertEqual(debug["done"], 1)   # counted from the log once the session is gone

    def test_free_one_refuses_unuploaded(self):
        make_session(self.base, sid="20261003-184200")
        self.backend.freeSession("20261003-184200")
        self.assertTrue(self.messages[-1][1])
        self.assertTrue(os.path.isdir(self.store.session_dir("20261003-184200")))

    def test_busy_session_not_offered(self):
        self.backend._upload_proc = object()
        self.backend._upload = {"session": SESSION}
        self.assertEqual(self.backend.freeableCount, 0)
        self.backend.freeSession(SESSION)
        self.assertTrue(os.path.isdir(self.store.session_dir(SESSION)))
        self.backend._upload_proc = None

    def test_round_needs_room_for_its_export(self):
        self.assertGreaterEqual(self.ft.ROUND_BYTES, self.ft.RECORD_BYTES + takes.export_need(self.ft.RECORD_BYTES))


if __name__ == "__main__":
    unittest.main()
