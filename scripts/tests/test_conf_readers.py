"""The report scripts read frametop.conf as Frametop does (conf/ft_conf.py): update-check.py's
dashboard apps check and gaze-report.py's gaze lines. Their own parsers missed a quoted value
(DASHBOARD_APPS="frametop") and cut a value at a "#" inside quotes. HOME is a temp folder, and
the scripts are loaded, not run, so nothing is checked on the machine and no service is asked."""
import os
import pathlib
import runpy
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[2]
CONF = """# mine
DASHBOARD_APPS="frametop"   # floats the dashboard's apps
GAZE_TRACKER='own'
POINTER_GAZE_LABEL="a # inside quotes"
"""


class ConfReaders(unittest.TestCase):
    def setUp(self):
        self.home = tempfile.TemporaryDirectory(prefix="ft-conf-readers.")
        self.addCleanup(self.home.cleanup)
        env = mock.patch.dict(os.environ, HOME=self.home.name)
        env.start()
        self.addCleanup(env.stop)
        self.conf = pathlib.Path(self.home.name) / ".config" / "frametop.conf"
        self.conf.parent.mkdir()

    def load(self, name):
        return runpy.run_path(str(ROOT / "scripts" / name), run_name="conf_readers_test")

    def test_update_check_reads_a_quoted_value(self):
        uc = self.load("update-check.py")
        self.assertEqual(uc["frametop_repos"]()[0], str(ROOT))
        self.assertEqual(uc["frametop_conf"](), {})   # no file yet
        self.conf.write_text(CONF)
        conf = uc["frametop_conf"]()
        self.assertEqual((conf["DASHBOARD_APPS"], conf["POINTER_GAZE_LABEL"]), ("frametop", "a # inside quotes"))

    def test_update_check_without_a_checkout_skips(self):
        uc = self.load("update-check.py")
        self.conf.write_text(CONF)
        said = []
        with mock.patch.dict(uc["frametop_conf"].__globals__, frametop_repos=lambda: [self.home.name],
                             report=lambda *a: said.append(a)):
            self.assertIsNone(uc["frametop_conf"]())
            uc["check_dashboard_apps"]()
        self.assertEqual([a[:2] for a in said], [("skip", "dashboard apps")])   # said, not silent

    def test_update_check_skips_a_copy_that_does_not_load(self):
        uc = self.load("update-check.py")
        self.conf.write_text(CONF)
        broken = pathlib.Path(self.home.name) / "old" / "conf"
        broken.mkdir(parents=True)
        (broken / "ft_conf.py").write_text("raise ImportError('an old copy')\n")
        with mock.patch.dict(uc["frametop_conf"].__globals__,
                             frametop_repos=lambda: [str(broken.parent), str(ROOT)]):
            self.assertEqual(uc["frametop_conf"]()["DASHBOARD_APPS"], "frametop")

    def test_gaze_report_reads_the_gaze_lines(self):
        self.conf.write_text(CONF)
        gr = self.load("gaze-report.py")
        conf, setting, _, tracker = gr["gaze_settings"](None)
        self.assertEqual((setting, tracker), ("own", "own"))
        self.assertEqual(conf["POINTER_GAZE_LABEL"], "a # inside quotes")
        self.conf.unlink()
        self.assertEqual(gr["gaze_settings"](None)[:2], ({}, "auto"))


if __name__ == "__main__":
    unittest.main()
