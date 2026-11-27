"""The gamepad pill's Gamepad mode (pointer/dashpatch/gamepadmode.py) against a pretend SteamVR
dashboard: 2.18 (push the focus, then close; armed when the push isn't taken) and 2.19 (our tab,
then the diminished dashboard, left as it was when SteamVR doesn't give us the focus).

  python3 pointer/tests/test_gamepadmode.py

The expressions are JavaScript: these run where there is node, and are skipped elsewhere.
"""
import importlib.util
import json
import shutil
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("gamepadmode", ROOT / "pointer/dashpatch/gamepadmode.py")
gamepadmode = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gamepadmode)
NODE = shutil.which("node")

# One expression per line in (JSON), its value per line out. `opts`: version 18 or 19, whether
# SteamVR gives the page the focus (takes), and lets our tab dim (dims).
DASHBOARD = r"""
const opts = %s;
const calls = [];
const state = {active: opts.active || 'valve.steam.gamepadui.main', vis: opts.open === false ? 0 : 1, top: 3};
const padPage = {summonOverlayKey: 'frametop.gamepad', mainPanelSGID: 7,
                 inputFocus: {PushInputFocus: () => { calls.push('push'); if (opts.takes) state.top = 7; }}};
const window = {
  FrameStore: {frames: [{pages: [{summonOverlayKey: 'valve.steam.gamepadui.main'}]},
                        {pages: [{summonOverlayKey: 'valve.steam.desktop'}]},
                        {pages: opts.page === false ? [] : [padPage]}]},
  inputFocusStore: {get computedInputFocus() { return {m_entryAtTopOfStack: {m_unPanelSGID: state.top}}; }},
  DashboardStore: {
    get visibilityState() { return state.vis; },
    get isFullyVisible() { return state.vis == 1; },
    get activeFrame() { return {activePage: {summonOverlayKey: state.active}}; },
    get canBeInDiminishedModeForActiveFrame() { return opts.dims && state.active == 'frametop.gamepad'; },
    EnterDiminishedState() {
      calls.push('dim');
      if (this.canBeInDiminishedModeForActiveFrame) { state.vis = 2; padPage.inputFocus.PushInputFocus(); }
    },
  },
  Dashboard: opts.version == 18 ? {hideDashboard: reason => { calls.push('hide'); state.vis = 0; }} : {
    showOrHideDashboardOrFrameInternal: req => {
      calls.push('switch ' + req.target.overlay_key);
      if (req.dashboard_visibility_action == 0) state.active = req.target.overlay_key;
    },
  },
};
globalThis.window = window;
globalThis.__ft = {state, calls};
require('readline').createInterface({input: process.stdin}).on('line', line => {
  const expr = JSON.parse(line);
  let value;
  try { value = expr == '__state' ? {state, calls} : eval(expr); } catch (e) { value = 'error: ' + e; }
  process.stdout.write(JSON.stringify(value === undefined ? null : value) + '\n');
});
"""


class Dashboard:
    def __init__(self, **opts):
        self.p = subprocess.Popen([NODE, "-e", DASHBOARD % json.dumps(opts)], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True)

    def evaluate(self, expr):
        self.p.stdin.write(json.dumps(expr) + "\n")
        self.p.stdin.flush()
        return json.loads(self.p.stdout.readline())

    def state(self):
        return self.evaluate("__state")

    def close(self):
        self.p.stdin.close()
        self.p.wait(timeout=10)
        self.p.stdout.close()


@unittest.skipUnless(NODE, "the expressions are JavaScript: no node here")
class Enter(unittest.TestCase):
    def enter(self, **opts):
        d = Dashboard(**opts)
        self.addCleanup(d.close)
        self.slept = []
        said, prev = gamepadmode.enter(d.evaluate, self.slept.append)
        return d, said, prev

    def test_2_19_dims_our_tab_and_remembers_the_one_before(self):
        d, said, prev = self.enter(version=19, takes=True, dims=True, active="valve.steam.desktop")
        self.assertIn("Gamepad mode", said)
        self.assertEqual(prev, "valve.steam.desktop")
        s = d.state()
        self.assertEqual(s["state"], {"active": "frametop.gamepad", "vis": 2, "top": 7})
        self.assertEqual(s["calls"], ["switch frametop.gamepad", "dim", "push"])

    def test_2_19_already_on_our_tab_goes_back_to_steam_after(self):
        _, _, prev = self.enter(version=19, takes=True, dims=True, active="frametop.gamepad")
        self.assertEqual(prev, gamepadmode.HOME_KEY)

    def test_2_19_no_focus_switches_back_and_keeps_the_dashboard(self):
        d, said, prev = self.enter(version=19, takes=False, dims=True)
        self.assertIsNone(prev)
        self.assertIn("didn't give", said)
        s = d.state()
        self.assertEqual(s["state"]["active"], "valve.steam.gamepadui.main")
        self.assertNotIn("hide", s["calls"])
        self.assertLessEqual(sum(self.slept), 2 * gamepadmode.WAIT_S + 1e-9)

    def test_2_19_no_dim_switches_back(self):
        d, said, prev = self.enter(version=19, takes=True, dims=False, active="valve.steam.desktop")
        self.assertIsNone(prev)
        self.assertIn("didn't let", said)
        s = d.state()
        self.assertEqual(s["state"]["active"], "valve.steam.desktop")
        self.assertEqual(s["state"]["vis"], 1)
        self.assertNotIn("dim", s["calls"])

    def test_2_19_closed_dashboard_is_left_alone(self):
        d, said, prev = self.enter(version=19, takes=True, dims=True, open=False)
        self.assertEqual((said, prev), ("no dashboard open", None))
        self.assertEqual(d.state()["calls"], [])

    def test_2_18_pushes_then_closes(self):
        d, said, prev = self.enter(version=18, takes=True)
        self.assertEqual(said, "focus on frametop.gamepad, dashboard closed")
        self.assertIsNone(prev)
        self.assertEqual(d.state()["calls"], ["push", "hide"])

    def test_2_18_no_focus_arms_the_next_gamepad_mode_then_closes(self):
        # Issue 105: SteamOS 0.4.5's SteamVR keeps the focus on the panel clicked last.
        d, said, prev = self.enter(version=18, takes=False)
        self.assertIn("next Gamepad mode is Frametop's", said)
        self.assertIsNone(prev)
        self.assertEqual(d.state()["calls"], ["push", "hide"])
        self.assertTrue(d.evaluate("!!(window.__ftPadArmed && !window.__ftPadArmed.active)"))
        self.assertLessEqual(sum(self.slept), gamepadmode.OLD_WAIT_S + 1e-9)

    def test_no_page(self):
        d, said, prev = self.enter(version=19, takes=True, dims=True, page=False)
        self.assertTrue(said.startswith("no frametop.gamepad page"), said)
        self.assertIsNone(prev)
        self.assertEqual(d.state()["calls"], [])


@unittest.skipUnless(NODE, "the expressions are JavaScript: no node here")
class Leave(unittest.TestCase):
    def test_back_to_the_tab_once_the_diminished_mode_ends(self):
        d = Dashboard(version=19, takes=True, dims=True, active="valve.steam.desktop")
        self.addCleanup(d.close)
        _, prev = gamepadmode.enter(d.evaluate, lambda s: None)
        session = gamepadmode.Session()
        session.start(prev)
        self.assertFalse(session.leave(d.evaluate))  # still diminished: nothing
        d.evaluate("__ft.state.vis = 1")  # SteamVR showed the dashboard again (a grip)
        self.assertTrue(session.leave(d.evaluate))
        self.assertEqual(d.state()["state"]["active"], "valve.steam.desktop")
        self.assertIsNone(session.prev)
        self.assertFalse(session.leave(d.evaluate))  # once

    def test_a_tab_the_user_picked_is_kept(self):
        d = Dashboard(version=19, takes=True, dims=True)
        self.addCleanup(d.close)
        session = gamepadmode.Session()
        session.start("valve.steam.desktop")
        d.evaluate("__ft.state.active = 'valve.steam.gamepadui.main'")
        self.assertTrue(session.leave(d.evaluate))
        self.assertEqual(d.state()["state"]["active"], "valve.steam.gamepadui.main")

    def test_no_session_reads_nothing(self):
        asked = []
        self.assertFalse(gamepadmode.Session().leave(asked.append))
        self.assertEqual(asked, [])


# ft-dashpatch's STEAMVR_STATE on a 2.18 dashboard in Gamepad mode with a Frametop screen still on
# top of the focus stack (issue 105): laser mode unless the pill armed it.
STATE = r"""
const st = {mode: 2};
globalThis.window = {
  FrameStore: {frames: [{pages: [{summonOverlayKey: 'frametop.gamepad', mainPanelSGID: 7}]}]},
  OverlayStore: {m_mapAllOverlays: new Map([[1, {sOverlayKey: 'frametop.screen.1', unStandalonePanelSGID: 3}]])},
  get inputFocusStore() { return {computedInputFocus: {m_eSystemPanelInteractionMode: st.mode,
    m_unPIDForVRInput: 0, m_unSteamInputAppID: 769,
    m_entryAtTopOfStack: {m_unPanelSGID: 3, m_bPanelVisible: true, m_bCanTakeKeyboardFocus: true}}}; },
};
const state = () => eval(%s).split(' ').slice(0, 3).join(' ');
const out = [state()];
eval(%s);
out.push(state());
st.mode = 0; out.push(state(), String(window.__ftPadArmed));
st.mode = 2; out.push(state());
process.stdout.write(JSON.stringify(out));
"""


@unittest.skipUnless(NODE, "the expressions are JavaScript: no node here")
class Armed(unittest.TestCase):
    def test_armed_gamepad_mode_is_ours_until_it_ends(self):
        dspec = importlib.util.spec_from_file_location("dashpatch", ROOT / "pointer/dashpatch/ft-dashpatch.py")
        dashpatch = importlib.util.module_from_spec(dspec)
        dspec.loader.exec_module(dashpatch)
        js = STATE % (json.dumps(dashpatch.STEAMVR_STATE), json.dumps(gamepadmode.ARM))
        out = json.loads(subprocess.run([NODE, "-e", js], capture_output=True, text=True, check=True).stdout)
        # before: laser mode; armed: Frametop's; it ended: disarmed; the next one is laser mode again
        self.assertEqual(out, ["0 0 0", "1 0 1", "0 0 0", "null", "0 0 0"])


# STEAMVR_STATE on a 2.19 dashboard after the pill: SteamVR puts the panel clicked last (here a
# game's Frametop panel, SGID 3) back on top of the focus stack as its Gamepad mode starts, with the
# dashboard still diminished on our tab (headset, 2026-11-27).
DIMMED = r"""
const st = {mode: 2, vis: 2, active: 'frametop.gamepad', top: 3};
globalThis.window = {
  FrameStore: {frames: [{pages: [{summonOverlayKey: 'frametop.gamepad', mainPanelSGID: 7}]}]},
  OverlayStore: {m_mapAllOverlays: new Map([[1, {sOverlayKey: 'frametop.app.200', unStandalonePanelSGID: 3}],
                                            [2, {sOverlayKey: 'valve.steam.gamepadui.main', unStandalonePanelSGID: 5}]])},
  DashboardStore: {get visibilityState() { return st.vis; },
                   get activeFrame() { return {activePage: {summonOverlayKey: st.active}}; }},
  get inputFocusStore() { return {computedInputFocus: {m_eSystemPanelInteractionMode: st.mode,
    m_unPIDForVRInput: 0, m_unSteamInputAppID: 769,
    m_entryAtTopOfStack: {m_unPanelSGID: st.top, m_bPanelVisible: true, m_bCanTakeKeyboardFocus: true}}}; },
};
const state = change => { Object.assign(st, {mode: 2, vis: 2, active: 'frametop.gamepad', top: 3}, change);
                          return eval(%s).split(' ').slice(0, 3).join(' ') + ' ' + eval(%s); };
process.stdout.write(JSON.stringify([state({}), state({mode: 1}), state({vis: 1}),
                                     state({active: 'valve.steam.gamepadui.main'}), state({top: 5}), state({vis: 0})]));
"""


@unittest.skipUnless(NODE, "the expressions are JavaScript: no node here")
class Dimmed(unittest.TestCase):
    def test_gamepad_mode_in_our_dimmed_tab_is_ours_with_a_frametop_panel_on_top(self):
        dspec = importlib.util.spec_from_file_location("dashpatch", ROOT / "pointer/dashpatch/ft-dashpatch.py")
        dashpatch = importlib.util.module_from_spec(dspec)
        dspec.loader.exec_module(dashpatch)
        js = DIMMED % (json.dumps(dashpatch.STEAMVR_STATE), json.dumps(gamepadmode.STILL))
        out = json.loads(subprocess.run([NODE, "-e", js], capture_output=True, text=True, check=True).stdout)
        self.assertEqual(out, [
            "1 0 1 on",    # Gamepad mode, dimmed on our tab, the game's panel on top: Frametop's
            "0 0 0 on",    # before SteamVR's Gamepad mode starts: laser mode
            "0 0 0 off",   # the dashboard fully open: a Frametop panel on top is laser mode (2026-10-25)
            "0 0 0 off",   # dimmed on Steam's tab (SteamVR's own pill): the same
            "1 0 0 on",    # Steam's panel on top: Gamepad mode, not Frametop's
            "0 0 0 off",   # the dashboard closed: laser mode again
        ])


if __name__ == "__main__":
    unittest.main()
