"""Steam's page in SteamVR's dashboard: the strip's lease and the watch's expression."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("steampage", ROOT / "pointer/dashpatch/steampage.py")
steampage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(steampage)


class StripLeaseTests(unittest.TestCase):
    def test_held_until_it_lapses(self):
        lease = steampage.StripLease()
        self.assertFalse(lease.held(10.0))
        self.assertTrue(lease.take(b"steamstrip 1", 10.0))
        self.assertTrue(lease.held(10.0 + steampage.LAPSE_S - 0.1))
        self.assertFalse(lease.held(10.0 + steampage.LAPSE_S))

    def test_renewal_extends_it(self):
        lease = steampage.StripLease()
        lease.take(b"steamstrip 1", 10.0)
        lease.take(b"steamstrip 1", 11.0)
        self.assertTrue(lease.held(11.0 + steampage.LAPSE_S - 0.1))

    def test_zero_ends_it_at_once(self):
        lease = steampage.StripLease()
        lease.take(b"steamstrip 1", 10.0)
        self.assertTrue(lease.take(b"steamstrip 0", 10.5))
        self.assertFalse(lease.held(10.5))

    def test_other_messages_are_not_its(self):
        lease = steampage.StripLease()
        for msg in (b"gamepad", b"steamstrip", b"steamstrip 2", b"steamstrip 1 extra"):
            self.assertFalse(lease.take(msg, 10.0))
        self.assertFalse(lease.held(10.0))


class ExpressionTests(unittest.TestCase):
    def test_hide_flag_is_passed(self):
        self.assertTrue(steampage.expression(True).endswith("(true)"))
        self.assertTrue(steampage.expression(False).endswith("(false)"))

    def test_it_names_steams_page_and_scopes_the_rule_to_its_frame(self):
        e = steampage.expression(True)
        self.assertIn("'valve.steam.gamepadui.main'", e)
        self.assertIn("legacy-frame-controls-", e)
        self.assertIn("m_rgTheaterFrameStack", e)
        self.assertIn("valve.steam.gamepadui.main_Panel.ResizeHandle", e)

    def test_only_while_steams_page_is_the_one_shown(self):
        e = steampage.expression(True)
        self.assertIn("frame.activePage.summonOverlayKey == 'valve.steam.gamepadui.main'", e)
        self.assertIn("hide && id && steams", e)


class TheaterTests(unittest.TestCase):
    def test_it_asks_steamvr_as_its_own_button_does(self):
        self.assertIn("'valve.steam.gamepadui.main'", steampage.TO_THEATER)
        self.assertIn("docking.SetInTheater(false)", steampage.TO_THEATER)

    def test_the_ask_is_not_a_strip_message(self):
        self.assertFalse(steampage.StripLease().take(steampage.THEATER_ASK, 10.0))


class SteamTabTests(unittest.TestCase):
    def test_only_from_a_games_tab_to_steams(self):
        e = steampage.TO_STEAM
        self.assertIn("key.startsWith('valve.steam.desktopgame.')", e)
        self.assertIn("overlay_key: 'valve.steam.gamepadui.main'", e)
        self.assertIn("dashboard_visibility_action: 0", e)
        self.assertIn("isFullyVisible", e)

    def test_asks_are_routed(self):
        self.assertEqual(steampage.ASKS[b"steamtab"][1], steampage.TO_STEAM)
        self.assertEqual(steampage.ASKS[b"steamtheater"][1], steampage.TO_THEATER)
        self.assertNotIn(b"steamstrip 1", steampage.ASKS)


# TO_STEAM against a pretend dashboard: (what it said, the tab after).
FAKE = r"""
const o = %s;
let active = o.active;
globalThis.window = {
  DashboardStore: {isFullyVisible: o.open, get activeFrame() { return {activePage: {summonOverlayKey: active}}; }},
  Dashboard: {showOrHideDashboardOrFrameInternal: r => {
    if (r.dashboard_visibility_action == 0) active = r.target.overlay_key; }},
};
const said = eval(%s);
process.stdout.write(JSON.stringify([said, active]));
"""


@unittest.skipUnless(shutil.which("node"), "the expression is JavaScript: no node here")
class SteamTabBehaviour(unittest.TestCase):
    def run_it(self, **o):
        out = subprocess.run([shutil.which("node"), "-e", FAKE % (json.dumps(o), json.dumps(steampage.TO_STEAM))],
                             capture_output=True, text=True, check=True).stdout
        return json.loads(out)

    def test_a_games_tab_goes_to_steams(self):
        self.assertEqual(self.run_it(open=True, active="valve.steam.desktopgame.2868840"),
                         ["from valve.steam.desktopgame.2868840", "valve.steam.gamepadui.main"])

    def test_another_tab_is_left(self):
        self.assertEqual(self.run_it(open=True, active="frametop.gamepad"),
                         ["left on frametop.gamepad", "frametop.gamepad"])

    def test_a_closed_dashboard_is_left(self):
        self.assertEqual(self.run_it(open=False, active="valve.steam.desktopgame.1"),
                         ["no dashboard open", "valve.steam.desktopgame.1"])


if __name__ == "__main__":
    unittest.main()
