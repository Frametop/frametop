"""Where you look, on Frametop Settings' Together page (YourShares.qml): whether the room gets
it, as roompage.py reads ft-together's `gaze`, and the switch as a question to ft-together
("Where you look" in together/README.md).

A part of the page's backend (ft_together_settings.py), which makes it, asks `gaze` with
everything else it asks, and gives it the two things it needs: the answers, and a way to ask.
"""
from PySide6.QtCore import Property, QObject, Signal, Slot

import roompage  # (what the answers mean, without Qt)


class Gaze(QObject):
    changed = Signal()

    def __init__(self, told, do):
        """told(key): the last answer to ft-together's "gaze" as a dict, or None. do(text): asks
        ft-together `text`, tells its person why when it says no, and asks for everything
        again."""
        super().__init__()
        self._told, self._do = told, do

    def _row(self):
        return roompage.gaze(self._told("gaze"))

    @Property(bool, notify=changed)
    def can(self):
        return self._row()["can"]

    @Property(bool, notify=changed)
    def shared(self):
        return self._row()["shared"]

    @Property(str, notify=changed)
    def text(self):
        return self._row()["text"]

    @Slot(bool)
    def setShared(self, on):
        self._do("gaze on" if on else "gaze off")
