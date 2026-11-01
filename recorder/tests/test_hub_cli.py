"""The recorders' hub.py command line (recorder/hub.py's cli, used by hands/rec/hub.py and
gaze/rec/hub.py): each recorder's help text, its upload argument (a hand session or an eye round)
and the JSON lines the window reads. The hub functions are stubs: nothing reaches Hugging Face,
and the base folder is a temp one."""
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

from gaze.rec import hub as gaze_hub
from hands.rec import hub as hands_hub
from recorder import hub as rhub


def run(module, *argv):
    """module.main() with argv: (status, stdout, stderr)."""
    out, err = io.StringIO(), io.StringIO()
    with mock.patch.object(sys, "argv", ["hub.py", *argv]), contextlib.redirect_stdout(out), \
            contextlib.redirect_stderr(err), mock.patch.object(os, "setpriority", lambda *a: None):
        try:
            status = module.main()
        except SystemExit as e:
            status = e.code
    return status, out.getvalue(), err.getvalue()


class HubCli(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def stub(self, module, **fns):
        for name, fn in fns.items():
            p = mock.patch.object(module, name, fn)
            p.start()
            self.addCleanup(p.stop)

    def test_help_names_each_recorder_and_its_item(self):
        for module, desc, item in ((hands_hub, "Upload a hand recorder export to the hand dataset on Hugging Face.", "session"),
                                   (gaze_hub, "Upload an eye recorder export to the eye dataset on Hugging Face.", "round")):
            with self.subTest(item):
                status, out, _ = run(module, "--help")
                self.assertEqual(status, 0)
                self.assertIn(desc, " ".join(out.split()))
                self.assertIn(f"upload exports/{item.upper()} as a pull request", out)
                status, out, _ = run(module, "upload", "--help")
                self.assertEqual(status, 0)
                self.assertRegex(out.split("positional arguments")[0], rf"\s{item}\s")

    def test_upload_gets_the_item_and_answers_in_json(self):
        for module, item in ((hands_hub, "20261031-120000-a1b2"), (gaze_hub, "20261031-120000-edges")):
            with self.subTest(module.__name__):
                got = {}

                def upload(store, name, dry_run=False, again=False, progress=None, log=None):
                    got.update(base=store.base, name=name, dry_run=dry_run, again=again)
                    progress("check", "Checking", 0.5)
                    log("a line")
                    return {"dry_run": True, "files": 3, "repo": "x/y", "path_in_repo": "p"}

                self.stub(module, upload=upload)
                status, out, _ = run(module, "--base", self.tmp.name, "upload", item, "--dry-run", "--json")
                self.assertEqual(status, 0)
                self.assertEqual(got, {"base": self.tmp.name, "name": item, "dry_run": True, "again": False})
                lines = [json.loads(x) for x in out.splitlines()]
                self.assertEqual(lines, [{"phase": "check", "text": "Checking", "fraction": 0.5}, {"log": "a line"},
                                         {"done": {"dry_run": True, "files": 3, "repo": "x/y", "path_in_repo": "p"}}])

    def test_a_hub_error_is_status_1(self):
        def whoami():
            raise rhub.HubError("login", "Not logged in", errors=["run hub.py login"])

        for module in (hands_hub, gaze_hub):
            with self.subTest(module.__name__):
                self.stub(module, whoami=whoami)
                status, out, _ = run(module, "whoami", "--json")
                self.assertEqual(status, 1)
                self.assertEqual(json.loads(out)["error"]["kind"], "login")
                status, _, err = run(module, "whoami")
                self.assertEqual((status, err.splitlines()), (1, ["error (login): Not logged in", "  run hub.py login"]))

    def test_board_and_whoami(self):
        for module in (hands_hub, gaze_hub):
            with self.subTest(module.__name__):
                bases = []
                self.stub(module, whoami=lambda: {"name": "someone", "role": "write"},
                          fetch_board=lambda base: bases.append(base) or {"updated": "2026-10-31", "bounties": 2})
                self.assertEqual(run(module, "whoami")[:2], (0, "logged in as someone (token role: write)\n"))
                self.assertEqual(run(module, "--base", self.tmp.name, "board", "--json")[:2],
                                 (0, json.dumps({"done": {"updated": "2026-10-31", "bounties": 2}}) + "\n"))
                self.assertEqual(bases, [self.tmp.name])


if __name__ == "__main__":
    unittest.main()
