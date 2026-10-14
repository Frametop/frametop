#!/usr/bin/env python3
"""The eye recorder's upload (gaze/rec/hub.py) without the network: the dry run, the gate that
keeps the real dataset closed until the texts are reviewed, upload records, and the pull request
flow against a fake Hub. The round is test_takes.make_round's random frames.

  python3 gaze/rec/tests/test_hub.py
"""
import json
import os
import shutil
import sys
import tempfile
import unittest
import uuid
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, ".."))
import hub  # noqa: E402
import takes  # noqa: E402
from test_takes import HAVE, RID, make_round  # noqa: E402


class Pr:
    def __init__(self, num, status="draft"):
        self.num, self.status, self.is_pull_request = num, status, True
        self.url = f"https://huggingface.co/datasets/a/b/discussions/{num}"


@unittest.skipUnless(HAVE, "needs zstd and the cryptography library (the dev container)")
class HubTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="eyerec-hub-")
        base = os.path.join(self.tmp, "contrib")
        make_round(base)
        self.contributor = str(uuid.uuid4())
        with open(os.path.join(base, "profile.json"), "w") as f:
            json.dump({"contributor": self.contributor,
                       "consent": {"version": "test-1", "adult": True, "region_ok": True, "accepted": "2026-10-14"}}, f)
        self.store = takes.Store(base)
        meta = hub.meta_path(self.store, RID)
        with open(meta) as f:
            m = json.load(f)
        m.pop("uploads")   # the fixture's, for the export's test
        with open(meta, "w") as f:
            json.dump(m, f)
        self.store.export(RID, low_priority=False)   # to recipients.txt's keys, as validate wants

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_dry_run(self):
        logged = []
        with mock.patch.dict(os.environ, {hub.DATASET_ENV: "someone/test-eyes"}):
            result = hub.upload(self.store, RID, dry_run=True, log=logged.append)
        self.assertTrue(result["dry_run"])
        self.assertEqual(result["repo"], "someone/test-eyes")
        self.assertEqual(result["path_in_repo"], f"contributions/{self.contributor}/{RID}")
        self.assertIn("- Bounty: edges", result["commit_description"])
        self.assertIn("- Health answers: included, encrypted", result["commit_description"])
        self.assertTrue(any("health.json.age" in line for line in logged))
        self.assertEqual(hub.rhub.uploads(hub.meta_path(self.store, RID)), [])

    def test_gate(self):
        self.assertFalse(hub.texts_ready(), "the texts aren't written yet")
        with mock.patch.dict(os.environ, {hub.ALLOW_ENV: "1", hub.DATASET_ENV: ""}):
            self.assertFalse(hub.upload_allowed(hub.dataset_id()), "never the real dataset before the review")
            with self.assertRaises(hub.HubError) as cm:
                hub.upload(self.store, RID)
            self.assertEqual(cm.exception.kind, "closed")
        with mock.patch.dict(os.environ, {hub.ALLOW_ENV: "1", hub.DATASET_ENV: "a/test"}):
            self.assertTrue(hub.upload_allowed(hub.dataset_id()))
        with mock.patch.dict(os.environ, {hub.ALLOW_ENV: "", hub.DATASET_ENV: "a/test"}):
            self.assertFalse(hub.upload_allowed(hub.dataset_id()))

    def test_invalid_blocks(self):
        with open(os.path.join(self.store.export_dir(RID), "health.json"), "w") as f:
            f.write("{}")
        with self.assertRaises(hub.HubError) as cm:
            hub.upload(self.store, RID, dry_run=True)
        self.assertEqual(cm.exception.kind, "invalid")

    def test_pull_request_first(self):
        calls = []

        class Api:
            fail = True

            def whoami(self):
                return {"name": "someone", "auth": {"accessToken": {"role": "write"}}}

            def auth_check(self, repo, repo_type=None):
                pass

            def create_pull_request(self, repo, title, description=None, repo_type=None):
                calls.append("create")
                return Pr(3)

            def get_discussion_details(self, repo, num, repo_type=None):
                return Pr(num)

            def upload_folder(self, **kw):
                calls.append(("upload", kw["revision"], kw["path_in_repo"]))
                if Api.fail:
                    raise ConnectionResetError("reset")

            def change_discussion_status(self, repo, num, status, repo_type=None):
                calls.append(("status", num, status))

        env = {hub.DATASET_ENV: "a/b", hub.ALLOW_ENV: "1"}
        meta = hub.meta_path(self.store, RID)
        before = os.stat(meta).st_mtime_ns
        with mock.patch.dict(os.environ, env), mock.patch.object(hub, "_hf", return_value=mock.Mock(HfApi=Api)):
            with self.assertRaises(hub.HubError) as cm:
                hub.upload(self.store, RID)
            self.assertEqual(cm.exception.kind, "network")
            [rec] = hub.rhub.uploads(meta)
            self.assertEqual((rec["status"], rec["pr_num"]), ("started", 3))
            Api.fail = False
            result = hub.upload(self.store, RID)
            with self.assertRaises(hub.HubError) as cm:
                hub.upload(self.store, RID, dry_run=True)
            self.assertEqual(cm.exception.kind, "duplicate")
        where = f"contributions/{self.contributor}/{RID}"
        self.assertEqual(calls, ["create", ("upload", "refs/pr/3", where), ("upload", "refs/pr/3", where),
                                 ("status", 3, "open")])
        self.assertEqual(result["pr_num"], 3)
        self.assertEqual(os.stat(meta).st_mtime_ns, before, "the record doesn't make the export look stale")
        self.assertFalse(self.store.rounds()[0]["export_stale"])


if __name__ == "__main__":
    unittest.main()
