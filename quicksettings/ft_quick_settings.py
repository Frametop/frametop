"""Frametop Settings' Quick Settings page: which toggles the palm menu's Quick Settings shows (user,
2026-11-19: "The actual toggles within that menu should be ones that you can configure from inside of
the FrameTop Settings application").

The page's backend (QuickSettingsPages.qml; settings/ft_settings.py makes it). The toggles there are
conf/ft_input_actions.py's QUICK_SETTINGS_ACTIONS (screens/quicksettings.h keeps the same list); the
ones picked are QUICK_SETTINGS in ~/.config/frametop.conf, a comma list of their names (unset: all of
them). ft-screens reads it each time the palm's Quick Settings opens, so a change shows the next time.
"""
import os
import sys

from PySide6.QtCore import Property, QObject, Signal, Slot

REPO = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
sys.path.insert(0, os.path.join(REPO, "conf"))
import ft_conf  # noqa: E402  (frametop.conf's grammar, docs/config.md)
from ft_input_actions import QUICK_SETTINGS_ACTIONS, QUICK_SETTINGS_KEY  # noqa: E402

NAMES = tuple(name for name, _ in QUICK_SETTINGS_ACTIONS)


def picked(conf):
    """The toggles QUICK_SETTINGS picks in `conf` (frametop.conf read), in the catalogue's order: unset,
    every one; names it doesn't know are left out (as ft-screens does: quicksettings::Chosen)."""
    value = conf.get(QUICK_SETTINGS_KEY)
    if value is None:
        return list(NAMES)
    chosen = set(ft_conf.items(value))
    return [n for n in NAMES if n in chosen]


class Backend(QObject):
    changed = Signal()
    message = Signal(str, bool)

    def __init__(self, conf=None):
        super().__init__()
        self._conf = conf   # frametop.conf's path (None: the real one)

    def _picked(self):
        return picked(ft_conf.read(self._conf))

    @Property("QVariantList", notify=changed)
    def toggles(self):
        """[{name, label, on}] for each toggle, in the card's order."""
        on = set(self._picked())
        return [{"name": n, "label": label, "on": n in on} for n, label in QUICK_SETTINGS_ACTIONS]

    @Slot(str, bool)
    def setShown(self, name, on):
        if name not in NAMES:
            return
        now = set(self._picked())
        now = now | {name} if on else now - {name}
        try:
            ft_conf.set_value(QUICK_SETTINGS_KEY, ft_conf.LIST_SEP.join(n for n in NAMES if n in now), self._conf)
        except OSError as e:
            self.message.emit(f"Couldn't save frametop.conf: {e.strerror or e}", True)
        self.changed.emit()
