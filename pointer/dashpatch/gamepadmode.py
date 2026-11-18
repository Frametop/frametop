"""Gamepad mode for a Frametop panel: ft-screens' gamepad pill (screens/vr.cpp, "gamepad pill").

The pill's click ("gamepad" on @ft_dashpatch) gives SteamVR's controllers to frametop.gamepad, the
dashboard overlay ft-screens owns, which hands the pad to the input relay (input/vrpad.py).

Up to SteamVR 2.18 that was SteamVR's own Enter Gamepad Mode for a tab that isn't in front: push
the page's input focus, then close the dashboard. SteamVR 2.19 (SteamOS 0.5) ignores a focus push
for a page that isn't showing, so that closed the dashboard and nothing took the pad (headset,
2026-11-18: lasers stuck, grips dead). 2.19 has a diminished dashboard instead, which is what its
pill does for the tab in front: so this switches the dashboard to frametop.gamepad's tab (ft-screens
paints a card on it saying which panel has the pad), waits for SteamVR to allow the diminished
mode there, and enters it (testbench 2026-11-18: our page on top of the focus stack, ft-screens'
pid for the input). Either way the dashboard closes or dims only once our page has the focus; if
it never does, the dashboard goes back to the tab it showed.

When the diminished mode ends (a grip, the system button, the dashboard closing), `Session.leave`
switches the dashboard back to that tab, so the card isn't what the dashboard shows next.
"""

import json

KEY = "frametop.gamepad"
WAIT_S = 2.0   # for the switch and the focus (well under a second on the testbench, 2026-11-18)
STEP_S = 0.1
HOME_KEY = "valve.steam.gamepadui.main"   # the tab to go back to when the one before isn't known

_PAGE = """
  let page = null;
  for (const fr of (window.FrameStore && window.FrameStore.frames) || [])
    for (const p of fr.pages || []) if (p.summonOverlayKey == '%s') page = p;
  const d = window.DashboardStore, dash = window.Dashboard;
  const f = window.inputFocusStore && window.inputFocusStore.computedInputFocus;
  const top = f && f.m_entryAtTopOfStack && f.m_entryAtTopOfStack.m_unPanelSGID;
  const took = !!(page && page.mainPanelSGID && top == page.mainPanelSGID);
  const active = d && d.activeFrame && d.activeFrame.activePage && d.activeFrame.activePage.summonOverlayKey;
""" % KEY


def _switch(key_js):
    """A request that shows tab `key_js` (a JS expression) in the open dashboard, without opening it."""
    return ("dash.showOrHideDashboardOrFrameInternal({dashboard_visibility_action: 0, target: {overlay_key: %s},"
            " vrlink_request_split: 0, relatch_dashboard: {condition: 0, deadzone: 8},"
            " reason: 'Frametop gamepad mode'})" % key_js)


# One step of entering Gamepad mode: a word, a space, and what to say.
#   "old <said>"   SteamVR 2.18: the focus is pushed; check it with TOOK, then HIDE
#   "wait <key>"   2.19: switching to our tab (from <key>, the tab it showed): ask again
#   "dim <key>"    2.19: diminished on our tab; check the focus with TOOK
#   anything else: why it can't
STEP = """(() => {%s
  if (!page || !page.inputFocus) return 'no frametop.gamepad page (ft-screens not running?)';
  if (dash && dash.hideDashboard) { page.inputFocus.PushInputFocus(); return 'old pushed'; }
  if (!d || !d.EnterDiminishedState || !dash || !dash.showOrHideDashboardOrFrameInternal)
    return 'no way into Gamepad mode in this SteamVR';
  if (!d.isFullyVisible) return 'no dashboard open';
  if (active != '%s') { %s; return 'wait ' + (active || '-'); }
  if (!d.canBeInDiminishedModeForActiveFrame) return 'wait -';
  d.EnterDiminishedState();
  return 'dim -';
})()""" % (_PAGE, KEY, _switch(json.dumps(KEY)))

# "1" once our page tops SteamVR's focus stack (the pad is ours).
TOOK = """(() => {%s
  return took ? '1' : '0';
})()""" % _PAGE

# SteamVR 2.18: close the dashboard as its Enter Gamepad Mode does.
HIDE = """(() => {
  const d = window.Dashboard;
  if (d && d.hideDashboard) d.hideDashboard('Frametop gamepad mode');
  return 'hidden';
})()"""


def back(key):
    """Switches the dashboard back to tab `key` if it still shows ours: 'back' or 'left'."""
    return """(() => {%s
  if (active != '%s' || !dash || !dash.showOrHideDashboardOrFrameInternal) return 'left';
  %s;
  return 'back';
})()""" % (_PAGE, KEY, _switch(json.dumps(key)))


# "on" while the diminished mode on our tab lasts, else "off".
STILL = """(() => {%s
  return d && d.visibilityState == 2 && active == '%s' ? 'on' : 'off';
})()""" % (_PAGE, KEY)


def enter(evaluate, sleep):
    """Gives SteamVR's controllers to frametop.gamepad, or leaves the dashboard as it was.
    `evaluate(expr)` runs an expression on SteamVR's dashboard page. Returns (what came of it,
    the tab to go back to once the diminished mode ends, or None when there's nothing to undo).
    At most WAIT_S for SteamVR to switch and dim, and WAIT_S more for the focus."""
    tries = int(WAIT_S / STEP_S)
    prev = None
    for _ in range(tries):
        said = evaluate(STEP) or ""
        word, _, rest = said.partition(" ")
        if word == "old":
            for _ in range(tries):
                if evaluate(TOOK) == "1":
                    evaluate(HIDE)
                    return "focus on frametop.gamepad, dashboard closed", None
                sleep(STEP_S)
            return "SteamVR didn't give frametop.gamepad the focus: dashboard left open", None
        if word == "wait":
            if prev is None and rest not in ("-", KEY):
                prev = rest
            sleep(STEP_S)
            continue
        if word == "dim":
            prev = prev or HOME_KEY
            for _ in range(tries):
                if evaluate(TOOK) == "1":
                    return "Gamepad mode on frametop.gamepad (dashboard diminished)", prev
                sleep(STEP_S)
            evaluate(back(prev))
            return f"SteamVR didn't give frametop.gamepad the focus: back to {prev}", None
        return said or "no reply", None
    if prev is not None:
        evaluate(back(prev))
    return f"SteamVR didn't let frametop.gamepad's tab dim: back to {prev or 'where it was'}", None


class Session:
    """The tab to go back to while Frametop's diminished Gamepad mode lasts (set by the pill's
    thread, read by the watch's: one attribute, swapped whole)."""

    def __init__(self):
        self.prev = None

    def start(self, prev):
        self.prev = prev

    def leave(self, evaluate):
        """While a session lasts: once SteamVR has left it, the dashboard back to its tab.
        True when it ended now."""
        prev = self.prev
        if prev is None or evaluate(STILL) != "off":
            return False
        evaluate(back(prev))
        self.prev = None
        return True
