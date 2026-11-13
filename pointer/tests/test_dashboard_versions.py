"""The dashboard patch on both of SteamVR's dashboards: 2.18 (SteamOS 0.4) closes with
hideDashboard, 2.19 (SteamOS 0.5) takes a request. The gamepad pill's click closes either, and
the update check passes either.

  python3 pointer/tests/test_dashboard_versions.py

The pill's click is JavaScript: its tests run where there is node, and are skipped elsewhere.
"""
import importlib.util
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


dashpatch = load("dashpatch", "pointer/dashpatch/ft-dashpatch.py")
update_check = load("update_check", "scripts/update-check.py")
NODE = shutil.which("node")

# A dashboard as the click sees it: frametop.gamepad's page, and a Dashboard with the given ways
# to close. What was called comes back with the click's reply.
PAGE = """
const calls = [];
const window = {
  FrameStore: {frames: [{pages: [{summonOverlayKey: 'other'}]},
                        {pages: %(pages)s.map(key => ({summonOverlayKey: key, mainPanelSGID: 7,
                                                       inputFocus: {PushInputFocus: () => calls.push(['focus'])}}))}]},
  Dashboard: %(dashboard)s,
};
const reply = %(click)s;
console.log(JSON.stringify({reply, calls}));
"""
OLD = "{hideDashboard: reason => calls.push(['hide', reason])}"
NEW = "{showOrHideDashboardOrFrameInternal: (request, raw) => calls.push(['request', request, raw === undefined])}"


@unittest.skipUnless(NODE, "the gamepad pill's click is JavaScript: no node here")
class GamepadPill(unittest.TestCase):
    def click(self, dashboard, pages="['frametop.gamepad']"):
        script = PAGE % {"pages": pages, "dashboard": dashboard, "click": dashpatch.GAMEPAD_PUSH}
        p = subprocess.run([NODE, "-e", script], capture_output=True, text=True, timeout=30)
        self.assertEqual(p.returncode, 0, p.stderr[-2000:])
        out = json.loads(p.stdout)
        return out["reply"], out["calls"]

    def test_steamvr_2_18(self):
        reply, calls = self.click(OLD)
        self.assertEqual(calls, [["focus"], ["hide", "Frametop gamepad mode"]])
        self.assertEqual(reply, "focus on frametop.gamepad (panel 7), dashboard closed")

    def test_steamvr_2_19(self):
        reply, calls = self.click(NEW)
        # the request SteamVR's own pill makes: hide, for this side, no relatch, and nothing else
        request = {"dashboard_visibility_action": 3, "vrlink_request_split": 0,
                   "relatch_dashboard": {"condition": 0, "deadzone": 8}, "reason": "Frametop gamepad mode"}
        self.assertEqual(calls, [["focus"], ["request", request, True]])
        self.assertEqual(reply, "focus on frametop.gamepad (panel 7), dashboard closed")

    def test_the_old_way_first_when_a_dashboard_has_both(self):
        both = "Object.assign(%s, %s)" % (OLD, NEW)
        self.assertEqual(self.click(both)[1], [["focus"], ["hide", "Frametop gamepad mode"]])

    def test_neither_way_says_so(self):
        for dashboard in ("{}", "undefined"):
            reply, calls = self.click(dashboard)
            self.assertEqual(calls, [["focus"]])
            self.assertIn("the dashboard stays open", reply)

    def test_no_page_does_nothing(self):
        reply, calls = self.click(OLD, pages="[]")
        self.assertEqual(calls, [])
        self.assertTrue(reply.startswith("no frametop.gamepad page"))


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
        rest = " ".join(sorted(update_check.DASHBOARD_MARKERS - {"PushInputFocus"}))
        self.assertEqual(self.missing(rest, "hideDashboard"), {"PushInputFocus"})

    def test_the_click_uses_the_names_the_check_looks_for(self):
        for name in update_check.DASHBOARD_CLOSERS | {"PushInputFocus", "summonOverlayKey"}:
            self.assertIn(name, dashpatch.GAMEPAD_PUSH)


if __name__ == "__main__":
    unittest.main()
