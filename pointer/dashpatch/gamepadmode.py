"""Gamepad mode for a Frametop panel: what ft-screens asks of SteamVR's dashboard for it
(screens/vrgamepad.cpp; when the mode is on is screens/padmode.h's).

The mode is Frametop's own: ft-screens takes its lasers off every panel and the input relay reads
the controllers raw (input/vrpad.py). SteamVR has two small parts in it, both asked on
@ft_dashpatch:

  "gamepad"  the pill was clicked with the Steam menu open: the dashboard closes (CLOSE). An open
             dashboard lasers everything, whatever our panels ask.
  "laser"    the mode ended on the wearer's word (both grips, the mouse): SteamVR, if it is in its
             own Gamepad mode, brings its laser back (LASER), as its "laser mouse required" prompt
             does. It would by itself once a controller points at a panel that takes a laser; this
             doesn't wait for that.

Until 2026-11-27 the pill used SteamVR's own Gamepad mode instead, for a dashboard overlay of ours
(frametop.gamepad): a focus push and a closed dashboard up to SteamVR 2.18, and on 2.19, which
ignores a push for a page that isn't showing, the dashboard dimmed on that overlay's tab. That put
a card in front of the wearer for as long as the mode lasted, and the grips didn't end it (headset,
2026-11-27). The dimming bought nothing: SteamVR's input focus was the panel clicked last all
through it, as with the dashboard closed.
"""

# The dashboard closes: SteamVR 2.18's own call, or 2.19's request (its HideDashboard is
# {dashboard_visibility_action: 3, reason}).
CLOSE = """(() => {
  const dash = window.Dashboard;
  if (dash && dash.hideDashboard) { dash.hideDashboard('Frametop gamepad mode'); return 'the dashboard closes'; }
  if (dash && dash.showOrHideDashboardOrFrameInternal) {
    dash.showOrHideDashboardOrFrameInternal({dashboard_visibility_action: 3, vrlink_request_split: 0,
      relatch_dashboard: {condition: 0, deadzone: 8}, reason: 'Frametop gamepad mode'});
    return 'the dashboard closes';
  }
  return 'no way to close the dashboard in this SteamVR';
})()"""

# SteamVR's laser comes back, when it is in its Gamepad mode (m_eSystemPanelInteractionMode 2).
LASER = """(() => {
  const s = window.inputFocusStore, f = s && s.computedInputFocus;
  if (!f) return 'no input focus to read';
  if (f.m_eSystemPanelInteractionMode != 2) return 'SteamVR has its laser already';
  if (!s.ForceActivateLaserMouse) return 'no way to ask for the laser in this SteamVR';
  if (!s.canForceActivateLaserMouse) return 'SteamVR sees no hand controller';
  s.ForceActivateLaserMouse();
  return 'SteamVR asked for its laser';
})()"""

# ft-screens' messages on @ft_dashpatch: what each is called in the log, and what it runs.
ASKS = {b"gamepad": ("gamepad pill", CLOSE), b"laser": ("gamepad mode", LASER)}
