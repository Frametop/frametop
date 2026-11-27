"""The update check passes both of SteamVR's dashboards: 2.18 (SteamOS 0.4) closes with
hideDashboard, 2.19 (SteamOS 0.5) takes a request. Gamepad mode's asks on each are
pointer/tests/test_gamepadmode.py.

  python3 pointer/tests/test_dashboard_versions.py
"""
import importlib.util
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


gamepadmode = load("gamepadmode", "pointer/dashpatch/gamepadmode.py")
update_check = load("update_check", "scripts/update-check.py")

class UpdateCheck(unittest.TestCase):
    def missing(self, *texts):
        with tempfile.TemporaryDirectory(prefix="ft-dashboard-test-") as folder:
            for i, text in enumerate(texts):
                sub = Path(folder, "libraries" if i else ".")
                sub.mkdir(exist_ok=True)
                (sub / f"chunk~{i}.js").write_text(text)
            (Path(folder) / "notes.txt").write_text(" ".join(update_check.DASHBOARD_CLOSERS))  # not .js: not read
            return update_check.dashboard_missing(folder)

    def test_either_way_to_close_is_enough(self):
        rest = " ".join(sorted(update_check.DASHBOARD_MARKERS))
        self.assertEqual(self.missing(rest, "x.hideDashboard('y')"), set())
        self.assertEqual(self.missing(rest + ";this.showOrHideDashboardOrFrameInternal(e,t){"), set())

    def test_no_way_to_close_names_both(self):
        # 2.19's HideDashboard is the request's maker, not a way in for the patch
        rest = " ".join(sorted(update_check.DASHBOARD_MARKERS)) + " HideDashboard("
        self.assertEqual(self.missing(rest), update_check.DASHBOARD_CLOSERS)

    def test_another_missing_name_is_reported_alone(self):
        rest = " ".join(sorted(update_check.DASHBOARD_MARKERS - {"m_entryAtTopOfStack"}))
        self.assertEqual(self.missing(rest, "hideDashboard"), {"m_entryAtTopOfStack"})

    def test_gamepad_mode_uses_the_names_the_check_looks_for(self):
        expressions = gamepadmode.CLOSE + gamepadmode.LASER
        for name in update_check.DASHBOARD_CLOSERS | update_check.GAMEPAD_MARKERS:
            self.assertIn(name, expressions)


if __name__ == "__main__":
    unittest.main()
