"""Frametop Settings' Quick Settings page: the palm menu's tiles (user, 2026-11-25: "completely
customize the palm menu ... what the text on a action should be, and then what the command should
run"; before, 2026-11-19, which toggles its Quick Settings shows).

The page's backend (QuickSettingsPages.qml; settings/ft_settings.py makes it). It edits
~/.config/frametop-palm.txt (conf/ft_palm_menu.py), which ft-screens reads each time the palm's card
shows, so a change is there the next time. palm_tiles.py has the edits and the actions to pick from.
"""
import os
import sys

from PySide6.QtCore import Property, QObject, Signal, Slot

REPO = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
sys.path.insert(0, os.path.join(REPO, "conf"))
sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
import ft_palm_menu as palm  # noqa: E402
import ft_store  # noqa: E402
import palm_tiles  # noqa: E402

# The page's groups: (page, title, help).
GROUPS = (
    ("menu", "Palm menu", "The card's first page. Pinching with that same hand starts on Steam Menu if it's shown here, "
                          "otherwise on the first tile."),
    ("quick", "Quick Settings page", "What the card's Quick Settings button opens."),
    ("eyes", "Eye tracking row", "A wide row under the tiles, shown only while gaze mode is on."),
)


class Moved(Exception):
    """The file isn't what the page shows."""


class Backend(QObject):
    changed = Signal()
    message = Signal(str, bool)

    def __init__(self, conf=None, path=None):
        super().__init__()
        self._conf, self._path = conf, path   # frametop.conf's and the palm file's (None: the real ones)
        self._seen = None   # the entries the page last got: its rows go by their place in these

    def _entries(self):
        return palm.load(self._path, self._conf)

    def _edit(self, fn, *args):
        """fn(entries, *args), saved; whether it was. Not if the file isn't what the page shows any more:
        a row's place could be another tile's."""
        def change(entries):
            if self._seen is not None and entries != self._seen:
                raise Moved
            return fn(entries, *args)
        saved = False
        try:
            self._seen = palm.update(change, self._path, self._conf)
            saved = True
        except Moved:
            self.message.emit("The palm menu had changed elsewhere: look again and try once more", False)
        except (OSError, ft_store.StoreError) as e:
            self.message.emit(f"Couldn't save the palm menu: {e}", True)
        self.changed.emit()
        return saved

    @Property("QVariantList", notify=changed)
    def groups(self):
        """[{page, title, help, max, shown, full, fixed, tiles: [{page, index, label, action, on, first, last}]}]"""
        entries = self._seen = self._entries()
        out = []
        for page, title, about in GROUPS:
            tiles = palm.of(entries, page)
            out.append({"page": page, "title": title, "help": about, "max": palm.MAX[page], "fixed": page == "eyes",
                        "shown": len(palm.shown(entries, page)), "full": palm_tiles.full(entries, page),
                        "tiles": [{"page": page, "index": i, "label": e["label"], "action": e["action"], "on": e["on"],
                                   "first": i == 0, "last": i == len(tiles) - 1} for i, e in enumerate(tiles)]})
        return out

    @Property("QVariantList", notify=changed)
    def actions(self):
        """[{value, text}]: what a tile can run, and what one has."""
        return palm_tiles.with_missing(palm_tiles.choices(), self._entries())

    @Property("QVariantList", notify=changed)
    def newActions(self):
        """[{value, text}]: what a new tile can run."""
        return palm_tiles.choices()

    @Property(int, constant=True)
    def labelMax(self):
        return palm.LABEL_MAX

    @Slot(str, int, bool)
    def setShown(self, page, index, on):
        if on and palm_tiles.full(self._entries(), page):
            self.message.emit(f"Only {palm.MAX[page]} fit on this page: hide one first", False)
            self.changed.emit()
            return
        self._edit(palm_tiles.set_shown, page, index, on)

    @Slot()
    def refresh(self):
        """The page came up: the file, your commands and the profiles as they are now."""
        self.changed.emit()

    @Slot(str, int, str)
    def setLabel(self, page, index, text):
        self._edit(palm_tiles.set_label, page, index, text)

    @Slot(str, int, str)
    def setAction(self, page, index, action):
        self._edit(palm_tiles.set_action, page, index, action)

    @Slot(str, str)
    def add(self, page, action):
        was_full = palm_tiles.full(self._entries(), page)
        if self._edit(palm_tiles.add, page, action) and was_full:
            self.message.emit(f"Added hidden: {palm.MAX[page]} fit on this page, so hide one to show it", False)

    @Slot(str, int)
    def remove(self, page, index):
        self._edit(palm_tiles.remove, page, index)

    @Slot(str, int, int)
    def move(self, page, index, by):
        self._edit(palm_tiles.move, page, index, by)

    @Slot()
    def restoreDefaults(self):
        try:
            palm.restore(self._path)
        except (OSError, ft_store.StoreError) as e:
            self.message.emit(f"Couldn't save the palm menu: {e}", True)
        self.changed.emit()
