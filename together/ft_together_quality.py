"""How good the video of your screens may get, on Frametop Settings' Together page
(YourShares.qml): Low, Balanced or High, as roompage.py reads ft-together's `quality`, and the
choice as a question to ft-together ("A shared panel's video" in together/README.md).

A part of the page's backend (ft_together_settings.py), which makes it, asks `quality` with
everything else it asks, and gives it the three things it needs: the answer, a way to ask, and
a way to keep the choice for the next time ft-together starts (frametop.conf).
"""
from PySide6.QtCore import Property, QObject, Signal, Slot

import roompage  # (what the answer means, without Qt)


class Quality(QObject):
    changed = Signal()

    def __init__(self, said, do, keep):
        """said(): the last answer to ft-together's "quality", or None. do(text, done): asks
        ft-together `text`, tells its person why when it says no, calls done(answer) when it
        says ok, and asks for everything again. keep(name): the choice, for the next start."""
        super().__init__()
        self._said, self._do, self._keep = said, do, keep

    def _row(self):
        return roompage.quality(self._said())

    @Property(bool, notify=changed)
    def can(self):
        return self._row()["can"]

    @Property(int, notify=changed)
    def index(self):
        return self._row()["index"]

    @Property(str, notify=changed)
    def text(self):
        return self._row()["text"]

    @Property("QStringList", constant=True)
    def names(self):
        return [title for _name, title, _text in roompage.QUALITIES]

    @Slot(int)
    def setIndex(self, index):
        if not 0 <= index < len(roompage.QUALITIES):
            return
        name = roompage.QUALITIES[index][0]
        self._do(f"quality {name}", lambda _answer: self._keep(name))
