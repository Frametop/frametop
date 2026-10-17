#!/usr/bin/env python3
"""Exports queue up and run one at a time; one set to upload uploads by itself once it's
exported, after any upload before it (recorder/window.py queueExport, 2026-10-17: "users can
queue exports, and trigger an auto upload when the export is finished"). Needs PySide6 (the dev
container); skipped without it.

  python3 hands/rec/tests/test_export_queue.py
"""
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import takes  # noqa: E402
from test_validate import make_session  # noqa: E402

A, B, C = "20261003-184200", "20261003-190000", "20261003-193000"


class ExportQueueTest(unittest.TestCase):
    def setUp(self):
        try:
            import ft_handrec
            from recorder import window
        except ImportError as e:
            self.skipTest(f"no PySide6: {e}")
        self.base = tempfile.mkdtemp(prefix="handrec-export-queue-test-")
        for sid in (A, B, C):
            make_session(self.base, sid=sid)
        self.store = takes.Store(self.base)
        self.backend = ft_handrec.Backend(self.store, {"dry_run": True}, hub_dry_run=True, ignore_cameras=True)
        self.backend._stay_awake = lambda why, on: None
        # Nothing runs: exports start and end by hand (finish), uploads start a fake hub.py.
        self.started = []    # sessions whose export started, in order
        self.sent = []       # sessions whose upload started, in order
        real_start = self.backend._start_export

        def start_export(session, keep_notes):
            real_start(session, keep_notes)
            self.started.append(session)
        self.backend._start_export = start_export
        popen = mock.patch.object(window.subprocess, "Popen", side_effect=self.popen)
        popen.start()
        self.addCleanup(popen.stop)
        thread = mock.patch.object(self.backend, "_thread")
        thread.start()
        self.addCleanup(thread.stop)

    def tearDown(self):
        shutil.rmtree(self.base, ignore_errors=True)

    def popen(self, argv, **kw):
        self.sent.append(argv[argv.index("upload") + 1])
        return mock.Mock()

    def finish_export(self, error=""):
        self.backend._on_export_finished("" if error else self.base, error)

    def finish_upload(self, ok=True):
        self.backend._on_upload_finished({"done": {"dry_run": True}} if ok else {"error": {"kind": "hub", "text": "no"}})

    def test_exports_run_in_turn(self):
        for sid in (A, B, C):
            self.backend.queueExport(sid, False, False)
        self.assertEqual(self.started, [A])
        self.assertEqual([q["session"] for q in self.backend.exportQueue], [B, C])
        self.finish_export()
        self.assertEqual(self.started, [A, B])
        self.finish_export("disk full")   # a failed one doesn't hold up the rest
        self.assertEqual(self.started, [A, B, C])
        self.finish_export()
        self.assertFalse(self.backend.exporting)
        self.assertEqual(self.sent, [])   # none was set to upload

    def test_queued_once(self):
        self.backend.queueExport(A, False, False)
        self.backend.queueExport(B, False, False)
        self.backend.queueExport(B, False, True)   # again, now to upload: the same place in the queue
        self.backend.queueExport(A, False, False)
        self.assertEqual(self.backend.exportQueue, [{"session": B, "upload": True}])
        self.assertEqual(self.started, [A])

    def test_upload_after_export(self):
        self.backend.queueExport(A, False, True)
        self.assertEqual(self.sent, [])
        self.finish_export()
        self.assertEqual(self.sent, [A])
        self.assertEqual(self.backend.upload["session"], A)

    def test_no_upload_after_a_failed_or_cancelled_export(self):
        self.backend.queueExport(A, False, True)
        self.finish_export("disk full")
        self.backend.queueExport(B, False, True)
        self.backend.cancelExport()
        self.finish_export("cancelled")
        self.assertEqual(self.sent, [])
        self.assertTrue(self.backend.doneLog[0]["error"])

    def test_uploads_wait_their_turn(self):
        for sid in (A, B, C):
            self.backend.queueExport(sid, False, sid != B)
        self.finish_export()              # A exported: it uploads
        self.finish_export()              # B exported: not set to upload
        self.finish_export()              # C exported: waits for A's upload
        self.assertEqual(self.sent, [A])
        self.assertEqual(self.backend.uploadQueue, [C])
        self.finish_upload(ok=False)      # a failed upload doesn't hold up the next
        self.assertEqual(self.sent, [A, C])
        self.finish_upload()
        self.assertFalse(self.backend.uploading)
        self.assertEqual(self.backend.uploadQueue, [])

    def test_upload_after_can_change(self):
        self.backend.queueExport(A, False, False)
        self.backend.queueExport(B, False, True)
        self.backend.setUploadAfter(A, True)
        self.backend.setUploadAfter(B, False)
        self.finish_export()
        self.finish_upload()
        self.finish_export()
        self.assertEqual(self.sent, [A])

    def test_unqueue(self):
        self.backend.queueExport(A, False, False)
        self.backend.queueExport(B, False, True)
        self.backend.unqueueExport(B)
        self.finish_export()
        self.assertEqual(self.started, [A])
        self.assertEqual(self.sent, [])

    def test_queued_session_isnt_deleted(self):
        self.backend.queueExport(A, False, False)
        self.backend.queueExport(B, False, False)
        self.backend.deleteSession(B)
        self.assertTrue(os.path.isdir(os.path.join(self.base, "sessions", B)))

    def test_no_second_upload_of_the_same_export(self):
        self.backend.queueExport(A, False, True)
        with mock.patch.object(self.backend, "uploadInfo", return_value={"previous": {"export_sha": "x"}, "uploads": 1}):
            self.finish_export()
        self.assertEqual(self.sent, [])
        self.assertIn("uploaded before", self.backend.doneLog[-1]["text"])


if __name__ == "__main__":
    unittest.main()
