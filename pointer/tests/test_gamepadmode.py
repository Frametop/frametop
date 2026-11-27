"""Gamepad mode's asks of SteamVR's dashboard (pointer/dashpatch/gamepadmode.py) against a pretend
one: the dashboard closes on 2.18 and on 2.19, and SteamVR's laser is asked back only out of its
Gamepad mode. And ft-dashpatch's STEAMVR_STATE: Gamepad mode with a Frametop panel on top of the
focus stack is laser mode.

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


def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


gamepadmode = load("gamepadmode", "pointer/dashpatch/gamepadmode.py")
NODE = shutil.which("node")

# `opts`: version 18, 19 or 0 (a dashboard with neither way to close), mode (SteamVR's panel
# interaction mode, 2 Gamepad), controllers (a hand controller is connected), force (it has
# ForceActivateLaserMouse). Prints [the expression's value, the calls it made].
DASHBOARD = r"""
const opts = %s;
const calls = [];
const store = {computedInputFocus: opts.focus === false ? null : {m_eSystemPanelInteractionMode: opts.mode},
               canForceActivateLaserMouse: !!opts.controllers};
if (opts.force !== false) store.ForceActivateLaserMouse = () => calls.push('laser');
globalThis.window = {
  inputFocusStore: store,
  Dashboard: opts.version == 18 ? {hideDashboard: reason => calls.push('hide ' + reason)}
    : opts.version == 19 ? {showOrHideDashboardOrFrameInternal: req => calls.push('request ' + req.dashboard_visibility_action)}
    : {},
};
const value = eval(%s);
process.stdout.write(JSON.stringify([value, calls]));
"""


def run(expression, **opts):
    js = DASHBOARD % (json.dumps(opts), json.dumps(expression))
    return json.loads(subprocess.run([NODE, "-e", js], capture_output=True, text=True, check=True).stdout)


@unittest.skipUnless(NODE, "the expressions are JavaScript: no node here")
class Close(unittest.TestCase):
    def test_2_18_hides(self):
        self.assertEqual(run(gamepadmode.CLOSE, version=18), ["the dashboard closes", ["hide Frametop gamepad mode"]])

    def test_2_19_asks_to_hide(self):
        # 3: SteamVR 2.19's own HideDashboard request
        self.assertEqual(run(gamepadmode.CLOSE, version=19), ["the dashboard closes", ["request 3"]])

    def test_neither_says_so(self):
        said, calls = run(gamepadmode.CLOSE, version=0)
        self.assertTrue(said.startswith("no way to close"), said)
        self.assertEqual(calls, [])


@unittest.skipUnless(NODE, "the expressions are JavaScript: no node here")
class Laser(unittest.TestCase):
    def test_asked_out_of_gamepad_mode(self):
        self.assertEqual(run(gamepadmode.LASER, mode=2, controllers=True), ["SteamVR asked for its laser", ["laser"]])

    def test_laser_mode_is_left_alone(self):
        said, calls = run(gamepadmode.LASER, mode=1, controllers=True)
        self.assertEqual((said, calls), ("SteamVR has its laser already", []))

    def test_no_controller(self):
        self.assertEqual(run(gamepadmode.LASER, mode=2, controllers=False)[1], [])

    def test_a_steamvr_without_it(self):
        said, calls = run(gamepadmode.LASER, mode=2, controllers=True, force=False)
        self.assertTrue(said.startswith("no way to ask"), said)
        self.assertEqual(calls, [])

    def test_no_focus_to_read(self):
        self.assertEqual(run(gamepadmode.LASER, focus=False, controllers=True)[1], [])


class Asks(unittest.TestCase):
    def test_ft_screens_words(self):
        self.assertEqual(set(gamepadmode.ASKS), {b"gamepad", b"laser"})
        self.assertEqual(gamepadmode.ASKS[b"gamepad"][1], gamepadmode.CLOSE)
        self.assertEqual(gamepadmode.ASKS[b"laser"][1], gamepadmode.LASER)


# ft-dashpatch's STEAMVR_STATE: SteamVR's Gamepad mode (2) with the panel on top of the focus stack
# a Frametop screen, a game's Frametop panel, the Full Keyboard, Steam's own, or none.
STATE = r"""
const st = {mode: 2, top: 3};
globalThis.window = {
  FrameStore: {frames: []},
  OverlayStore: {m_mapAllOverlays: new Map([[1, {sOverlayKey: 'frametop.screen.1', unStandalonePanelSGID: 3}],
                                            [2, {sOverlayKey: 'frametop.app.200', unStandalonePanelSGID: 4}],
                                            [3, {sOverlayKey: 'valve.steam.gamepadui.main', unStandalonePanelSGID: 5}],
                                            [4, {sOverlayKey: 'org.framekeyboard.keys', unStandalonePanelSGID: 6}]])},
  get inputFocusStore() { return {computedInputFocus: {m_eSystemPanelInteractionMode: st.mode,
    m_entryAtTopOfStack: st.top ? {m_unPanelSGID: st.top, m_bPanelVisible: true, m_bCanTakeKeyboardFocus: true} : null}}; },
};
const state = change => { Object.assign(st, {mode: 2, top: 3}, change); return eval(%s); };
process.stdout.write(JSON.stringify([state({}), state({top: 4}), state({top: 6}), state({top: 5}), state({top: 0}),
                                     state({mode: 1, top: 5})]));
"""


@unittest.skipUnless(NODE, "the expressions are JavaScript: no node here")
class State(unittest.TestCase):
    def test_gamepad_mode_on_a_frametop_panel_is_laser_mode(self):
        dashpatch = load("dashpatch", "pointer/dashpatch/ft-dashpatch.py")
        js = STATE % json.dumps(dashpatch.STEAMVR_STATE)
        out = json.loads(subprocess.run([NODE, "-e", js], capture_output=True, text=True, check=True).stdout)
        self.assertEqual(out, [
            "0 0 0 0 frametop.screen.1",            # a Frametop screen on top: laser mode (2026-10-25)
            "0 0 0 0 frametop.app.200",             # a game's Frametop panel: the same
            "0 0 0 0 org.framekeyboard.keys",       # the Full Keyboard: the same
            "1 0 0 0 valve.steam.gamepadui.main",   # Steam's panel: SteamVR's Gamepad mode
            "1 0 0 0 -",                            # nothing on top: Gamepad mode
            "0 0 0 0 valve.steam.gamepadui.main",   # laser mode
        ])

    def test_the_watch_takes_its_reply(self):
        # ft-dashpatch's watch checks "<theater> <state>" against this before it splits it
        import re
        self.assertTrue(re.fullmatch(r"[01] [01] \d+ [01] \d+ \S+", "0 1 2868840 0 0 frametop.app.200"))


if __name__ == "__main__":
    unittest.main()
