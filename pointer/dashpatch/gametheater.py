"""View in Theater from a Steam game's Frametop panel (screens/gstheater.h, vrgsapps.cpp).

ft-screens takes the game's window off its panel (untags it), gamescope gives the game its own
overlay, valve.steam.desktopgame.<app id>, and SteamVR makes a frame for it. SteamVR put that frame
in its theater by itself once, with the dashboard closed, and docked it in the dashboard instead
with the dashboard open (testbench, 2026-11-18). So ft-screens asks ("gametheater <app id>" on
@ft_dashpatch), and ft-dashpatch waits for the frame and puts it in the theater the way SteamVR's
own View in Theater does (its docking's SetInTheater, which also closes the dashboard).
"""
import re

WAIT_S = 3.0   # for gamescope's overlay and SteamVR's frame for it (about a second, 2026-11-18)
STEP_S = 0.2

_ASK = re.compile(rb"gametheater ([1-9][0-9]{0,9})")


def ask(message):
    """The app id in a "gametheater <app id>" message, else None."""
    m = _ASK.fullmatch(message)
    if not m:
        return None
    app = int(m.group(1))
    return app if app <= 0xFFFFFFFF else None


def expression(app):
    """'in' when the game's frame is in SteamVR's theater now (put there by this, or already),
    'none' while there's no frame for it yet."""
    return """(() => {
  const key = 'valve.steam.desktopgame.%d';
  for (const f of (window.FrameStore && window.FrameStore.frames) || [])
    if ((f.pages || []).some(p => p.summonOverlayKey == key)) {
      const stack = (window.DashboardStore && window.DashboardStore.m_rgTheaterFrameStack) || [];
      if (!stack.some(t => t && t.m_unFrameID == f.m_unFrameID)) f.docking.SetInTheater();
      return 'in';
    }
  return 'none';
})()""" % app


def put_in_theater(app, evaluate, sleep):
    """Asks SteamVR until the game's frame is in its theater, for WAIT_S at most: what came of it.
    `evaluate(expr)` runs an expression on SteamVR's dashboard page."""
    tries = int(WAIT_S / STEP_S)
    for _ in range(tries):
        if evaluate(expression(app)) == "in":
            return f"game {app} in SteamVR's theater"
        sleep(STEP_S)
    return f"no frame for game {app} in SteamVR's dashboard after {WAIT_S:g} s"
