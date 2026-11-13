"""Remote Displays' "Show its apps and windows in the launcher" switch: what the backend keeps
and sets after the sign-in, and the switch and its dialog on the page. HOME is a temp folder,
the sign-in (conf/ft_hostsignin.py, tested on its own) and ft-layout are stand-ins, and
ft-screens and the hosts are never asked. Needs PySide6 and Kirigami (the image's python3).

  python3 remote-displays/test/test_app_windows.py
"""
import os
import stat
import sys
import tempfile
import time
import unittest
from pathlib import Path

os.environ["HOME"] = tempfile.mkdtemp(prefix="ft-appwindows-test-")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "remote-displays"), str(ROOT / "settings")]
import ft_remote_displays as rd  # noqa: E402
import ft_settings  # noqa: E402  (Frametop Settings, the window the page is in)
from PySide6.QtCore import QMetaObject, QObject  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine  # noqa: E402

HOSTS = Path(rd.ft_hostapi.TOKEN_DIR)
ADDRESS = "192.0.2.7"

# Nothing outside this process: no question to ft-screens, no host pinged.
rd.Backend._check = lambda self: None
rd.Backend._ping_hosts = lambda self: None


def spin(app, until=lambda: False, seconds=3.0):
    end = time.monotonic() + seconds
    while time.monotonic() < end and not until():
        app.processEvents()
        time.sleep(0.01)
    app.processEvents()
    return until()


def set_hosts(*hosts):
    layout = rd.ft_layout.load_layout()
    layout["hosts"] = list(hosts)
    rd.ft_layout.save_layout(layout)


class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QGuiApplication.instance() or QGuiApplication([])

    def setUp(self):
        for path in HOSTS.glob("*"):
            path.unlink()
        set_hosts({"name": "PC", "address": ADDRESS, "kind": "vibepollo", "direct": ["10.0.0.9"]})
        self.b = rd.Backend()
        self.ran, self.answers, self.asked = [], [], []
        self.b._run = lambda label, *args, stdin=None: self.ran.append(args)
        self.b.appWindowsSet.connect(lambda *a: self.answers.append(a))
        self.sign_in = lambda addresses, user, password, known: (48989, "tok-1", "sha256//own")
        old = rd.ft_hostsignin.app_windows
        rd.ft_hostsignin.app_windows = lambda *a: (self.asked.append(a), self.sign_in(*a))[1]
        self.addCleanup(setattr, rd.ft_hostsignin, "app_windows", old)

    def answered(self):
        self.assertTrue(spin(self.app, lambda: self.answers), "no answer from the backend")
        return self.answers[-1]


class Backend(Base):
    def test_on_keeps_that_instances_token_and_sets_its_port(self):
        self.b.appWindowsOn("PC", "alice", "secret")
        self.assertEqual(self.answered(), ("PC", True, ""))
        self.assertEqual(self.asked[0][:3], (["10.0.0.9", ADDRESS], "alice", "secret"))  # the dongle first
        token, pin = HOSTS / f"{ADDRESS}-48989.token", HOSTS / f"{ADDRESS}-48989.pin"
        self.assertEqual((token.read_text(), pin.read_text()), ("tok-1\n", "sha256//own\n"))
        self.assertEqual(stat.S_IMODE(token.stat().st_mode), 0o600)
        self.assertFalse((HOSTS / f"{ADDRESS}.token").exists(), "the displays' token isn't this one")
        self.assertEqual(self.ran, [("remote", "host", "PC", "windows=48989")])
        for path in HOSTS.glob("*"):
            self.assertNotIn("secret", path.read_text(), "the password is never kept")

    def test_a_windows_pc_shares_them_on_its_own_port(self):
        self.sign_in = lambda *a: (47989, "tok-2", "sha256//host")
        self.b.appWindowsOn("PC", "alice", "secret")
        self.assertEqual(self.answered(), ("PC", True, ""))
        self.assertEqual((HOSTS / f"{ADDRESS}-47989.token").read_text(), "tok-2\n")
        self.assertEqual(self.ran, [("remote", "host", "PC", "windows=47989")])

    def test_a_refused_sign_in_keeps_and_sets_nothing(self):
        def refuse(*a):
            raise rd.ft_hostsignin.SignInError("Wrong user name or password")
        self.sign_in = refuse
        self.b.appWindowsOn("PC", "alice", "wrong")
        self.assertEqual(self.answered(), ("PC", False, "Wrong user name or password"))
        self.assertEqual((list(HOSTS.glob("*")), self.ran), ([], []))

    def test_a_host_that_isnt_there(self):
        self.b.appWindowsOn("Nope", "alice", "secret")
        self.assertEqual(self.answers, [("Nope", False, "There's no host called Nope")])
        self.assertEqual(self.asked, [])

    def test_the_pin_each_instance_must_have(self):
        """Its own from an earlier sign-in; else the host's, but only on the port the displays use:
        a second instance has a certificate of its own."""
        HOSTS.mkdir(parents=True, exist_ok=True)
        (HOSTS / f"{ADDRESS}.pin").write_text("sha256//host\n")
        self.b.appWindowsOn("PC", "alice", "secret")
        self.answered()
        known = self.asked[0][3]
        # asked before this sign-in's own pin was kept: read again now that it is
        self.assertEqual((known(48989), known(47989)), ("sha256//own", "sha256//host"))
        (HOSTS / f"{ADDRESS}-48989.pin").unlink()
        self.assertEqual(known(48989), "")
        (HOSTS / f"{ADDRESS}-47989.pin").write_text("sha256//its-own\n")
        self.assertEqual(known(47989), "sha256//its-own")

    def test_off_leaves_the_launcher_and_keeps_the_token(self):
        self.b.appWindowsOn("PC", "alice", "secret")
        self.answered()
        self.ran.clear()
        self.b.appWindowsOff("PC")
        self.assertEqual(self.ran, [("remote", "host", "PC", "windows=off")])
        self.assertTrue((HOSTS / f"{ADDRESS}-48989.token").exists(), "open window panels still use it")

    def test_the_host_list_says_whether_its_on(self):
        self.assertEqual([h["appWindows"] for h in self.b.hosts], [False])
        set_hosts({"name": "PC", "address": ADDRESS, "kind": "vibepollo", "windows": {"port": 48989}})
        self.assertEqual([h["appWindows"] for h in self.b.hosts], [True])


def items(item):
    """An item and everything drawn under it (a Repeater's cards aren't QObject children)."""
    yield item
    for child in item.childItems():
        yield from items(child)


class Page(Base):
    def setUp(self):
        super().setUp()
        self.settings = ft_settings.Settings(start="remote-displays", backends={"remote": self.b})
        self.engine = QQmlApplicationEngine()
        self.warnings = []
        self.engine.warnings.connect(lambda ws: self.warnings.extend(w.toString() for w in ws))
        if not ft_settings.load(self.engine, self.settings):
            self.skipTest("main.qml didn't load here (no Kirigami?): " + "; ".join(self.warnings[:3]))
        self.root = self.engine.rootObjects()[0]
        spin(self.app, seconds=0.3)

    def tearDown(self):
        self.engine.deleteLater()
        spin(self.app, seconds=0.05)

    def switch(self):
        found = [i for i in items(self.root.contentItem()) if i.objectName() == "appWindows"]
        self.assertEqual(len(found), 1, "one switch on the host's card")
        return found[0]

    def toggle(self, on):
        """The switch as a click leaves it: changed, then saying so."""
        switch = self.switch()
        switch.setProperty("checked", on)
        QMetaObject.invokeMethod(switch, "toggled")
        spin(self.app, seconds=0.1)

    def own_warnings(self):
        return [w for w in self.warnings if "not placed in the graphics scene" not in w]

    def test_the_switch_asks_for_the_login_and_turns_on_when_it_worked(self):
        switch = self.switch()
        dialog = self.root.findChild(QObject, "windowsDialog")
        self.assertFalse(switch.property("checked"))
        self.toggle(True)
        self.assertTrue(dialog.property("visible"))
        self.assertEqual(dialog.property("hostName"), "PC")
        self.assertFalse(self.switch().property("checked"), "not on until the sign-in worked")
        self.assertFalse(dialog.property("ok"), "nothing to sign in with yet")
        self.root.findChild(QObject, "windowsUser").setProperty("text", "alice")
        self.root.findChild(QObject, "windowsPassword").setProperty("text", "secret")
        QMetaObject.invokeMethod(dialog, "accept")
        self.assertTrue(spin(self.app, lambda: not dialog.property("visible")), dialog.property("error"))
        self.assertEqual(self.asked[0][1:3], ("alice", "secret"))
        self.assertEqual(self.root.findChild(QObject, "windowsPassword").property("text"), "")
        self.assertEqual(self.ran, [("remote", "host", "PC", "windows=48989")])
        self.assertEqual(self.own_warnings(), [])

    def test_a_refusal_stays_in_the_dialog(self):
        def refuse(*a):
            raise rd.ft_hostsignin.SignInError("Its Vibepollo has no app windows.")
        self.sign_in = refuse
        dialog = self.root.findChild(QObject, "windowsDialog")
        self.toggle(True)
        self.root.findChild(QObject, "windowsUser").setProperty("text", "alice")
        self.root.findChild(QObject, "windowsPassword").setProperty("text", "secret")
        QMetaObject.invokeMethod(dialog, "accept")
        self.assertTrue(spin(self.app, lambda: dialog.property("error")), "the refusal isn't shown")
        self.assertEqual(dialog.property("error"), "Its Vibepollo has no app windows.")
        spin(self.app, seconds=0.6)  # a closing dialog takes a moment to go
        self.assertTrue(dialog.property("visible"))
        self.assertFalse(dialog.property("working"))
        self.assertEqual(self.ran, [])

    def test_turning_it_off(self):
        set_hosts({"name": "PC", "address": ADDRESS, "kind": "vibepollo", "windows": {"port": 48989}})
        self.b.changed.emit()
        spin(self.app, seconds=0.2)
        self.assertTrue(self.switch().property("checked"))
        self.toggle(False)
        self.assertEqual(self.ran, [("remote", "host", "PC", "windows=off")])
        self.assertFalse(self.root.findChild(QObject, "windowsDialog").property("visible"))


if __name__ == "__main__":
    unittest.main()
