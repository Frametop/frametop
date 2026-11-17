"""Display Settings' dashboard tiles (Visibility & pins): whether a panel anchored to the SteamVR
dashboard stays shown with it closed, and the band the tiles fill round you (screens/tiling.h,
docs/design.md "Dashboard tiles"). Each is a frametop.conf value ft-screens reads at start and
takes live as `handsetting KEY VALUE`, as the drag bars' are. And whether the apps started from the
dashboard (Steam's gamescope session) come up as Frametop panels (GSAPP_PANELS; docs/design.md
"Gamescope apps"), which go on the dashboard the first time.

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

STAY = "DASH_TILE_STAY"
GSAPPS = "GSAPP_PANELS"   # on unless "0", as ft-screens reads it
# key: (default, lo, hi), as screens/vrtiles.cpp's TileSettings() and tiling::Params have them
NUMBERS = {
    "DASH_TILE_RAISE_DEG": (30.0, -30.0, 60.0),    # the band's bottom above the dashboard's bar
    "DASH_TILE_RADIUS": (1.6, 0.8, 4.0),           # metres from your head
    "DASH_TILE_HEIGHT_DEG": (60.0, 12.0, 120.0),   # how tall the band is asked to be
    "DASH_TILE_MAX_TOP_DEG": (55.0, 20.0, 80.0),   # ...but its top never higher (the column cap)
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
        return ft_layout.read_conf().get(STAY, "0") == "1"

    @Slot(bool)
    def setStay(self, on):
        self._set_value(STAY, "1" if on else "0")

    @Property(bool, notify=changed)
    def gsApps(self):
        return ft_layout.read_conf().get(GSAPPS, "1") != "0"

    @Slot(bool)
    def setGsApps(self, on):
        self._set_value(GSAPPS, "1" if on else "0")

    @Property(float, notify=changed)
    def raiseDeg(self):
        return self.number("DASH_TILE_RAISE_DEG")

    @Slot(float)
    def setRaiseDeg(self, deg):
        self.set_number("DASH_TILE_RAISE_DEG", deg)

    @Property(float, notify=changed)
    def radius(self):
        return self.number("DASH_TILE_RADIUS")

    @Slot(float)
    def setRadius(self, metres):
        self.set_number("DASH_TILE_RADIUS", metres)

    @Property(float, notify=changed)
    def heightDeg(self):
        return self.number("DASH_TILE_HEIGHT_DEG")

    @Slot(float)
    def setHeightDeg(self, deg):
        self.set_number("DASH_TILE_HEIGHT_DEG", deg)

    @Property(float, notify=changed)
    def maxTopDeg(self):
        return self.number("DASH_TILE_MAX_TOP_DEG")

    @Slot(float)
    def setMaxTopDeg(self, deg):
        self.set_number("DASH_TILE_MAX_TOP_DEG", deg)
