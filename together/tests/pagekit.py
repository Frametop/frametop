"""What the tests of Frametop Settings' Together page share (test_roompage.py,
test_roomavatar.py): stand-ins that answer as ft-together, ft-roomvoice and ft-roompanels do, as
far as the page asks them, and the page's backend set up over them.
"""
import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "settings"))
import roomavatar  # noqa: E402
from fakescreens import Screens, serve  # noqa: E402

try:
    from PySide6.QtCore import QObject
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtQml import QQmlApplicationEngine
    import ft_settings
    import ft_together_settings as page
except ImportError as e:  # (a PC without PySide6)
    print(f"no PySide6 here, the rules only: {e}")
    page = ft_settings = None

ADA = {"id": 0, "name": "ada", "login": "ada@example.org", "away": 0, "linked": False, "spot": [0, 0, 0, 0]}
BEN = {"id": 1, "name": "ben", "login": "ben@example.org", "away": 2, "linked": True, "spot": [1, 0, 0, 0]}


def state(mode="idle", people=(), asks=(), note="", me=0, owner=0):
    return "ok " + json.dumps({"mode": mode, "note": note, "me": me, "owner": owner, "people": list(people), "panels": [], "asks": list(asks),
                               "codes": 0})


class Together:
    """ft-together, as far as the page asks it."""

    def __init__(self):
        self.mode, self.people, self.asks, self.note, self.tailscale = "idle", [], [], "", True
        self.looks = {"wear": "", "sent": False, "why": "", "people": []}
        self.asked = []

    def __call__(self, text):
        if text not in ("state", "here", "looks"):
            self.asked.append(text)
        if not self.tailscale:
            return "error Tailscale isn't running on this headset"
        w = text.split()
        if text == "state":
            return state(self.mode, self.people, self.asks, self.note)
        if text == "here":
            return "ok 100.64.0.7 47193 ada"
        if text == "looks":
            return "ok " + json.dumps(self.looks)
        if text == "open":
            if self.mode != "idle":
                return "error you are in a room already"
            self.mode, self.people = "hosting", [ADA]
            return "ok"
        if text == "invite":
            return "ok ABCD2345" if self.mode == "hosting" else "error it isn't your room"
        if w[0] == "join" and len(w) == 4:
            self.mode = "calling"
            return "ok"
        if w[0] in ("accept", "decline") and len(w) == 2:
            if not any(a["id"] == int(w[1]) for a in self.asks):
                return "error no such ask"
            self.asks = [a for a in self.asks if a["id"] != int(w[1])]
            if w[0] == "accept":
                self.people = [ADA, BEN]
            return "ok"
        if text == "leave":
            self.mode, self.people, self.asks = "idle", [], []
            return "ok"
        return "error what"


class Voice:
    def __init__(self):
        self.muted, self.asked = False, []

    def __call__(self, text):
        if text == "state":
            return "ok " + json.dumps({"room": True, "muted": self.muted, "mic": "closed" if self.muted else "open", "sent": 0, "level": 0,
                                       "playing": True, "voices": []})
        self.asked.append(text)
        self.muted = text == "mute"
        return "ok"


class Panels:
    def __init__(self):
        self.shared, self.asked = {}, []

    def __call__(self, text):
        if text == "state":
            return "ok " + json.dumps({"room": True, "floor": None, "panels": [],
                                       "shares": [{"screen": n, "panel": k, "told": True} for n, k in sorted(self.shared.items())]})
        self.asked.append(text)
        verb, number = text.split()
        if verb == "share":
            if int(number) > 2:
                return f"error screen {number} isn't one of this desktop's screens"
            self.shared[int(number)] = len(self.shared)
        else:
            self.shared.pop(int(number), None)
        return "ok"


@unittest.skipIf(page is None, "no PySide6 here")
class PageCase(unittest.TestCase):
    """The page's backend over stand-ins on sockets of the test's own. systemctl is a made-up
    command; the units' folder, frametop.conf and the avatars' folder are temp ones."""
    made = 0

    def setUp(self):
        self.app = QGuiApplication.instance() or QGuiApplication([])
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.units = Path(self.tmp.name)
        self.conf, self.avatars = self.units / "frametop.conf", self.units / "avatars"
        self.avatars.mkdir()
        PageCase.made += 1
        base = "ft_test_%d_%d_" % (os.getpid(), PageCase.made)
        self.together, self.voice, self.panels, self.screens = Together(), Voice(), Panels(), Screens()
        names = {}
        for to, stand_in in ((page.TOGETHER, self.together), (page.VOICE, self.voice), (page.PANELS, self.panels), (page.SCREENS, self.screens)):
            names[to] = base + to
            serve(stand_in, names[to])
        self.host = []
        old = page.host_command
        page.host_command = lambda *cmd: (self.host.append(cmd), self.systemctl)[1]
        self.addCleanup(setattr, page, "host_command", old)
        self.systemctl = ["true"]
        self.b = page.Backend(units=str(self.units), sockets=names, worn=roomavatar.Worn(conf=str(self.conf), folder=str(self.avatars)))
        self.said = []
        self.b.message.connect(lambda text, bad: self.said.append((text, bad)))

    def install(self, on=True):
        (self.units / page.UNIT).write_text("[Unit]\n")
        if on:
            (self.units / "default.target.wants").mkdir(exist_ok=True)
            (self.units / "default.target.wants" / page.UNIT).symlink_to(self.units / page.UNIT)

    def until(self, what, seconds=5.0):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self.app.processEvents()
            if what():
                return True
            time.sleep(0.01)
        return False

    def fresh(self, what):
        """Asks again until it is so, as the page's timer does."""
        return self.until(lambda: (self.b.refresh(), what())[1])

    def window(self):
        """The Settings window on the Together page: (its root, found(objectName), its warnings)."""
        settings = ft_settings.Settings(start="together", backends={"together": self.b})
        engine = QQmlApplicationEngine()
        warnings = []
        engine.warnings.connect(lambda ws: warnings.extend(w.toString() for w in ws))
        if not ft_settings.load(engine, settings):
            self.skipTest("main.qml didn't load here (no Kirigami?): " + "; ".join(warnings[:3]))
        self._window = (settings, engine)   # (kept while the test runs)
        root = engine.rootObjects()[0]
        self.addCleanup(self.b.setActive, False)
        self.assertTrue(self.until(lambda: root.property("pageTitle") == "Frametop Together"))
        return root, lambda name: root.findChild(QObject, name), warnings
