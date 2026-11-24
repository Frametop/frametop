"""Display Settings' dashboard tiles (Visibility & pins): whether the panels anchored to the SteamVR
dashboard stay shown with it closed (each one not set the other way by its anchor button's long
press), and over a game in SteamVR's theater too, and the wall the tiles stand on round you (screens/tiling.h,
docs/design.md "Dashboard tiles"). Each is a frametop.conf value ft-screens reads at start and
takes live as `handsetting KEY VALUE`, as the drag bars' are. And whether the apps started from the
dashboard (Steam's gamescope session) come up as Frametop panels (GSAPP_PANELS; docs/design.md
"Gamescope apps"), which go on the dashboard the first time. And whether Steam's own dashboard
window and its menu are Frametop panels too (STEAM_DASH_PANEL, off unless "1"; docs/design.md
"Steam's dashboard window").

The QML reaches it as backend.dashTiles. The numbers' ranges are ft-screens' own
(TileSettings() in screens/vrtiles.cpp): a value outside one is refused there, so it's clamped here.
"""
import math
import os
import sys

from PySide6.QtCore import Property, QObject, Signal, Slot

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "layout"))
import ft_layout  # noqa: E402
from ft_layout import ft_conf  # noqa: E402

STAY = "DASH_TILE_STAY"   # on unless "0", as ft-screens reads it
STAY_THEATER = "DASH_TILE_STAY_THEATER"   # off unless "1"
LOOK = "DASH_TILE_LOOK"   # on unless "0": with no pointer on a panel, the tiling keys act on the one you look at
GSAPPS = "GSAPP_PANELS"   # on unless "0", as ft-screens reads it
STEAM_WINDOWS = "STEAM_DASH_PANEL"   # off unless "1", as ft-screens reads it
AT_BAR = "DASH_TILE_AT_BAR"   # on unless "0": the wall at Steam's bar, its desk line and distance (not the two below)
# key: (default, lo, hi), as screens/vrtiles.cpp's TileSettings() and tiling::Params have them
NUMBERS = {
    "DASH_TILE_DESK_DEG": (-12.0, -40.0, 20.0),     # the desk line the columns stand on, up from your eyes
    "DASH_TILE_RADIUS": (2.7, 0.8, 6.0),            # metres from your head to the tiles' corners
    "DASH_TILE_PX_PER_DEG": (48.0, 15.0, 120.0),    # a panel's natural size: this many of its pixels a degree
    "DASH_TILE_WALL_DEG": (80.0, 30.0, 300.0),      # how wide the desk gets before new panels stack up
    "DASH_TILE_CURVE_M": (0.0, 0.0, 12.0),          # the arc's radius, metres (0: its centre 2 m behind your eyes)
    "DASH_TILE_CENTRE_CURVE_M": (0.0, 0.0, 12.0),   # the centre display's own, the arc's its sides' then (0: one arc)
    "DASH_TILE_HEIGHT_CURVE_M": (0.0, 0.0, 12.0),   # the rows' over and under the centre display (0: the arc's)
}


class DashTiles(QObject):
    changed = Signal()

    def __init__(self, set_value, parent=None):
        """`set_value(key, text)` writes frametop.conf, tells a running ft-screens and emits the
        backend's changed (Backend._setBar), which the backend passes on to this one's."""
        super().__init__(parent)
        self._set_value = set_value

    def number(self, key):
        default, lo, hi = NUMBERS[key]
        value = ft_conf.number(ft_layout.read_conf(), key, default)
        if not lo <= value <= hi:
            ft_conf.warn(f"frametop.conf: {key}={value:g} isn't from {lo:g} to {hi:g}; using {default:g}")
            return default
        return value

    def set_number(self, key, value):
        default, lo, hi = NUMBERS[key]
        value = max(lo, min(hi, value)) if math.isfinite(value) else default
        self._set_value(key, f"{value:g}")

    @Property(bool, notify=changed)
    def stay(self):
        return ft_layout.read_conf().get(STAY, "1") != "0"

    @Slot(bool)
    def setStay(self, on):
        self._set_value(STAY, "1" if on else "0")

    @Property(bool, notify=changed)
    def stayTheater(self):
        return ft_layout.read_conf().get(STAY_THEATER, "0") == "1"

    @Slot(bool)
    def setStayTheater(self, on):
        self._set_value(STAY_THEATER, "1" if on else "0")

    @Property(bool, notify=changed)
    def lookFocus(self):
        return ft_layout.read_conf().get(LOOK, "1") != "0"

    @Slot(bool)
    def setLookFocus(self, on):
        self._set_value(LOOK, "1" if on else "0")

    @Property(bool, notify=changed)
    def gsApps(self):
        return ft_layout.read_conf().get(GSAPPS, "1") != "0"

    @Slot(bool)
    def setGsApps(self, on):
        self._set_value(GSAPPS, "1" if on else "0")

    @Property(bool, notify=changed)
    def steamWindows(self):
        return ft_layout.read_conf().get(STEAM_WINDOWS, "0") == "1"

    @Slot(bool)
    def setSteamWindows(self, on):
        self._set_value(STEAM_WINDOWS, "1" if on else "0")

    @Property(bool, notify=changed)
    def atBar(self):
        return ft_layout.read_conf().get(AT_BAR, "1") != "0"

    @Slot(bool)
    def setAtBar(self, on):
        self._set_value(AT_BAR, "1" if on else "0")

    @Property(float, notify=changed)
    def deskDeg(self):
        return self.number("DASH_TILE_DESK_DEG")

    @Slot(float)
    def setDeskDeg(self, deg):
        self.set_number("DASH_TILE_DESK_DEG", deg)

    @Property(float, notify=changed)
    def radius(self):
        return self.number("DASH_TILE_RADIUS")

    @Slot(float)
    def setRadius(self, metres):
        self.set_number("DASH_TILE_RADIUS", metres)

    @Property(float, notify=changed)
    def pxPerDeg(self):
        return self.number("DASH_TILE_PX_PER_DEG")

    @Slot(float)
    def setPxPerDeg(self, px):
        self.set_number("DASH_TILE_PX_PER_DEG", px)

    @Property(float, notify=changed)
    def wallDeg(self):
        return self.number("DASH_TILE_WALL_DEG")

    @Slot(float)
    def setWallDeg(self, deg):
        self.set_number("DASH_TILE_WALL_DEG", deg)

    @Property(float, notify=changed)
    def curveRadius(self):
        """The tiles' arc, its centre to the wall, metres; 0 when not set (ft-screens' kFocusBackM:
        the centre 2 m behind your eyes, whatever the wall's distance)."""
        return self.number("DASH_TILE_CURVE_M")

    @Slot(float)
    def setCurveRadius(self, metres):
        self.set_number("DASH_TILE_CURVE_M", metres)

    @Property(float, notify=changed)
    def centreCurveRadius(self):
        """The centre display's own arc (and the tiles' over and under it), metres, the tiles' arc
        then its sides' only (screens/tilecurve.h); 0 when it's the same arc."""
        return self.number("DASH_TILE_CENTRE_CURVE_M")

    @Slot(float)
    def setCentreCurveRadius(self, metres):
        self.set_number("DASH_TILE_CENTRE_CURVE_M", metres)

    @Property(float, notify=changed)
    def heightCurveRadius(self):
        """The curve up and down, metres, past the centre display's top and bottom edges (the rows over
        and under it lean toward you); 0 when it's the arc's."""
        return self.number("DASH_TILE_HEIGHT_CURVE_M")

    @Slot(float)
    def setHeightCurveRadius(self, metres):
        self.set_number("DASH_TILE_HEIGHT_CURVE_M", metres)
