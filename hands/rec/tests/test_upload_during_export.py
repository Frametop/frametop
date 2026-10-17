#!/usr/bin/env python3
"""An export and an upload of different sessions go on together; the same session waits
(recorder/window.py exportSession and startUpload, 2026-10-17: an upload was refused while
any other session exported). Needs PySide6 (the dev container); skipped without it.

  python3 hands/rec/tests/test_upload_during_export.py
"""
import os
import shutil
import sys
import tempfile
import threading
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import takes  # noqa: E402
from test_validate import make_session  # noqa: E402

A, B = "20261003-184200", "20261003-190000"


class UploadDuringExportTest(unittest.TestCase):
    def setUp(self):
        try:
            import ft_handrec
            from recorder import window
        except ImportError as e:
            self.skipTest(f"no PySide6: {e}")
        self.window = window
        self.base = tempfile.mkdtemp(prefix="handrec-upload-export-test-")
        make_session(self.base, sid=A)
        make_session(self.base, sid=B)
        self.store = takes.Store(self.base)
        self.backend = ft_handrec.Backend(self.store, {"dry_run": True}, hub_dry_run=True, ignore_cameras=True)
        self.backend._stay_awake = lambda why, on: None

    def tearDown(self):
        shutil.rmtree(self.base, ignore_errors=True)

    def exporting(self, session):
        self.backend._export_cancel = threading.Event()
        self.backend._export_session = session

    def uploading(self, session):
        self.backend._upload_proc = object()
        self.backend._upload = {"session": session}

    def upload(self, session):
        """startUpload(session): whether it started hub.py."""
        with mock.patch.object(self.window.subprocess, "Popen") as popen, \
                mock.patch.object(self.backend, "_thread"):
            self.backend.startUpload(session, False)
        return popen.called

    def export(self, session):
        """exportSession(session): whether it started the export."""
        with mock.patch.object(self.backend, "_thread") as thread:
            self.backend.exportSession(session, False)
        return thread.called

    def test_upload_while_another_exports(self):
        self.exporting(A)
        self.assertTrue(self.upload(B))
        self.assertTrue(self.backend.uploading)

    def test_upload_waits_for_its_own_export(self):
        self.exporting(A)
        self.assertFalse(self.upload(A))

    def test_export_while_another_uploads(self):
        self.uploading(B)
        self.assertTrue(self.export(A))
        self.assertEqual(self.backend.exportSessionId, A)

    def test_no_export_of_the_uploading_session(self):
        self.uploading(B)
        self.assertFalse(self.export(B))

    def test_one_export_at_a_time(self):
        self.exporting(A)
        self.assertFalse(self.export(B))


if __name__ == "__main__":
    unittest.main()
