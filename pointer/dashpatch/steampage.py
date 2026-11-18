"""Steam's own page in SteamVR's dashboard, while ft-screens shows Steam's window as a tile.

ft-screens (screens/vrsteamwin.cpp) takes Steam's dashboard window into a Frametop tile and
shrinks Steam's page (valve.steam.gamepadui.main) to nothing. SteamVR still stands its controls
for that page (Show Keyboard, Float in World, View in Theater, More Options) under the page's
bottom edge, which then sits at its middle: a strip of four buttons over the tiles (user,
2026-11-18: "it doesn't look good for it to be visible"), and View in Theater put the page into an
invisible theater with no way back. So, read and written by ft-dashpatch's SteamVR watch:

- whether SteamVR shows Steam's page in its theater (ft-screens then gives Steam's windows back,
  so SteamVR's theater draws them with its own way out: the user's "Hand off to SteamVR"), and
- the strip hidden while ft-screens holds Steam's window ("steamstrip 1" on @ft_dashpatch, again
  every second; a lease, so with ft-screens gone the strip comes back by itself), and
- View in Theater, now from the grab bar menu of Frametop's Steam tile ("steamtheater"):
  TO_THEATER, the same call as SteamVR's button.

The strip is hidden with a style rule, not by changing SteamVR's state: its panel stays where it
is, drawing nothing, and the laser goes through it (testbench, 2026-11-18: clicks there reached
the tile behind).
"""

STEAM_PAGE = "valve.steam.gamepadui.main"
LAPSE_S = 3.0  # the lease without a renewal (ft-screens renews it every second)

# (hide) -> 1 when SteamVR's theater shows Steam's page, else 0; puts in or takes out the rule
# that hides the controls under it.
JS = """((hide) => {
  let frame = null;
  for (const f of (window.FrameStore && window.FrameStore.frames) || [])
    if ((f.pages || []).some(p => p.summonOverlayKey == '%s')) frame = f;
  const id = frame && frame.m_unFrameID;
  const stack = (window.DashboardStore && window.DashboardStore.m_rgTheaterFrameStack) || [];
  const theater = id && stack.some(f => f && f.m_unFrameID == id) ? 1 : 0;
  const css = hide && id ? '[id="legacy-frame-controls-' + id + '"] > * { visibility: hidden !important; }' : '';
  let rule = document.getElementById('ft-steam-strip');
  if (css && !rule) {
    rule = document.createElement('style');
    rule.id = 'ft-steam-strip';
    document.head.appendChild(rule);
  }
  if (rule && rule.textContent != css) rule.textContent = css;
  return theater;
})""" % STEAM_PAGE

# View in Theater from Frametop's Steam tile (its grab bar menu): what SteamVR's own button does.
TO_THEATER = """(() => {
  for (const f of (window.FrameStore && window.FrameStore.frames) || [])
    if ((f.pages || []).some(p => p.summonOverlayKey == '%s')) {
      f.docking.SetInTheater(false);
      return "Steam's page in SteamVR's theater";
    }
  return "no Steam page in SteamVR's dashboard";
})()""" % STEAM_PAGE

THEATER_ASK = b"steamtheater"


def expression(hide):
    """The watch's expression for this sample."""
    return f"{JS}({'true' if hide else 'false'})"


class StripLease:
    """ft-screens' "steamstrip 1" (hide SteamVR's controls under Steam's page) for LAPSE_S
    seconds, "steamstrip 0" ends it at once (Steam's window given back)."""

    def __init__(self):
        self.until = 0.0

    def take(self, message, now):
        """A "steamstrip 1|0" message: True when it was one."""
        if message == b"steamstrip 1":
            self.until = now + LAPSE_S
        elif message == b"steamstrip 0":
            self.until = 0.0
        else:
            return False
        return True

    def held(self, now):
        return now < self.until
