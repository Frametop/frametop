"""The room's panels on Frametop Settings' Together page (RoomPanels.qml): who asks to use
which of your screens, who can, and the panels there are, as controlpage.py reads them, and the
page's buttons as questions to ft-together ("Control of a shared panel" in together/README.md).

A part of the page's backend (ft_together_settings.py), which makes it, asks `control` with
everything else it asks, and gives it the two things it needs: the answers, and a way to ask.
"""
from PySide6.QtCore import Property, QObject, Signal, Slot

import controlpage  # (what the answers mean, without Qt)


class Control(QObject):
    changed = Signal()

    def __init__(self, told, do):
        """told(key): the last answer to ft-together's "state" or "control" as a dict, or None.
        do(text): asks ft-together `text`, tells its person why when it says no, and asks for
        everything again."""
        super().__init__()
        self._told, self._do = told, do

    def _answers(self):
        return self._told("state") or {}, self._told("control") or {}

    @Property("QVariantList", notify=changed)
    def asks(self):
        return controlpage.asks(*self._answers())

    @Property("QVariantList", notify=changed)
    def given(self):
        return controlpage.given(*self._answers())

    @Property(str, notify=changed)
    def note(self):
        return controlpage.note(*self._answers())

    @Property("QVariantList", notify=changed)
    def panels(self):
        return controlpage.panels(*self._answers())

    @Slot(int, int)
    def give(self, number, person):
        self._do(f"control give {int(number)} {int(person)}")

    @Slot(int, int)
    def take(self, number, person):
        """Takes it back from them, or says no to their ask."""
        self._do(f"control take {int(number)} {int(person)}")

    @Slot(int)
    def ask(self, panel):
        self._do(f"control ask {int(panel)}")
