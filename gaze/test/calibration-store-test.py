#!/usr/bin/env python3
"""calibration.json's one writer (gazecal.save_calibration, used by ft-gazeprobe and the panel's
calibration in gaze/gazecheck.py) and ft-eyes' pruning of its earlier calibrations
(eyes_model.prune_backups). HOME is a temp folder: no real calibration is read,
written or removed.

  gaze/test/calibration-store-test.py
"""
import os
import tempfile

HOME = tempfile.mkdtemp(prefix="ft-gaze-calibration-store-test-")
os.environ["HOME"] = HOME  # before gazecal: its STATE follows HOME

import contextlib  # noqa: E402
import io  # noqa: E402
import json  # noqa: E402
import sys  # noqa: E402
import unittest  # noqa: E402
from pathlib import Path  # noqa: E402

GAZE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GAZE))
sys.path.insert(0, str(GAZE / "tracker"))
import eyes_model  # noqa: E402
import ft_store  # noqa: E402  (gazecal and eyes_model put conf/ on the path)
import gazecal  # noqa: E402


class Model:
    def __init__(self, n):
        self.n = n

    def to_json(self):
        return {"samples": self.n}


class SaveCalibration(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(dir=HOME))
        self.path = self.dir / "calibration.json"

    def test_home_is_the_temp_folder(self):
        self.assertTrue(str(gazecal.CALIBRATION).startswith(HOME))

    def test_both_writers_one_schema(self):
        models = {"steamvr": Model(9), "left": Model(4)}
        gazecal.save_calibration(models, "quadratic", "panel", path=self.path)
        panel = json.loads(self.path.read_text())
        self.assertEqual(panel["_meta"]["how"], "panel")
        self.assertEqual(panel["_live"], {})
        gazecal.save_calibration(models, "affine", "probe", {"steamvr": [[1, 2]]}, 1234.5, self.path)
        probe = json.loads(self.path.read_text())
        self.assertEqual(probe["_meta"], {"calibrated_at": 1234.5, "model": "affine", "how": "probe"})
        self.assertEqual(probe["_live"], {"steamvr": [[1, 2]]})
        self.assertEqual(set(probe), set(panel))  # the same keys from both
        self.assertEqual(probe["steamvr"], {"samples": 9})
        self.assertEqual(sorted(os.listdir(self.dir)), [".calibration.json.lock", "calibration.json"])

    def test_a_file_that_doesnt_read_is_copied_then_replaced(self):
        for text in ('{"steamvr": ', "[]"):
            with self.subTest(text=text):
                self.path.write_text(text)
                err = io.StringIO()
                with contextlib.redirect_stderr(err):
                    gazecal.save_calibration({"steamvr": Model(1)}, "quadratic", "panel", path=self.path)
                self.assertIn("the new calibration replaces it", err.getvalue())
                self.assertEqual(json.loads(self.path.read_text())["steamvr"], {"samples": 1})
                self.assertEqual(sum((self.dir / n).read_text() == text
                                     for n in os.listdir(self.dir) if n.startswith("calibration.json.bad-")), 1)

    def test_no_copy_no_replace(self):
        self.path.write_text("[]")
        real = ft_store.backup
        ft_store.backup = lambda path: None   # (a full disk: no copy can be made)
        try:
            with self.assertRaises(ft_store.StoreError):
                gazecal.save_calibration({"steamvr": Model(1)}, "quadratic", "panel", path=self.path)
        finally:
            ft_store.backup = real
        self.assertEqual(self.path.read_text(), "[]")


class PruneBackups(unittest.TestCase):
    def test_keeps_the_newest_ten_and_nothing_else_goes(self):
        d = Path(tempfile.mkdtemp(dir=HOME))
        backups = [f"calibration-202610{day:02d}-120000.json" for day in range(1, 16)]
        others = ["calibration.json", "calibration-dots.jsonl", "state.json", "clicks.jsonl",
                  "calibration-mine.json", "calibration-20261001-120000.json.bak"]
        for n in backups + others:
            (d / n).write_text("{}")
        gone = eyes_model.prune_backups(d)
        self.assertEqual(gone, backups[:5])
        self.assertEqual(sorted(os.listdir(d)), sorted(backups[5:] + others))
        self.assertEqual(eyes_model.prune_backups(d), [])  # ten left: nothing more


if __name__ == "__main__":
    unittest.main()
