"""macOS High Performance Screen Sharing hosts on the Remote Displays page: mDNS _rfb._tcp
discovery, sign-in through ft-appless, credential file cleanup, hidden Vibepollo-only switches,
and picking Main display vs a Retina virtual display in the dialog. HOME is a temp folder and
no network or headset is touched. Needs PySide6 and Kirigami (the image's python3).

  python3 remote-displays/test/test_apple_hosts.py
"""
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

os.environ["HOME"] = tempfile.mkdtemp(prefix="ft-applehosts-test-")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "remote-displays"), str(ROOT / "settings")]
import ft_remote_displays as rd  # noqa: E402
import ft_settings  # noqa: E402
from PySide6.QtCore import QMetaObject, QObject  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine  # noqa: E402

HOSTS = Path(rd.ft_hostapi.TOKEN_DIR)
ADDRESS = "192.0.2.10"

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


def items(item):
    yield item
    for child in item.childItems():
        yield from items(child)


class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QGuiApplication.instance() or QGuiApplication([])

    def setUp(self):
        HOSTS.mkdir(parents=True, exist_ok=True)
        for path in HOSTS.glob("*"):
            path.unlink()
        set_hosts()
        self.b = rd.Backend()
        self.ran, self.signed, self.appless = [], [], []
        self.b._run = lambda label, *args, stdin=None: self.ran.append((args, stdin))
        self.b.signedIn.connect(lambda *a: self.signed.append(a))

        def no_webui(*_a, **_kw):
            raise rd.ft_hostsignin.SignInError("Couldn't reach it: Connection refused")

        def fake_appless(*args, timeout=20, stdin=None):
            self.appless.append((args, stdin))
            Path(rd.ft_hostapi.apple_path(ADDRESS)).write_text("alice\nsecret\n")
            return 0, "signed in to 192.0.2.10 as alice\n", ""

        old_signin = rd.ft_hostsignin.sign_in
        old_appless = rd.ft_layout.run_appless
        rd.ft_hostsignin.sign_in = no_webui
        rd.ft_layout.run_appless = fake_appless
        self.addCleanup(setattr, rd.ft_hostsignin, "sign_in", old_signin)
        self.addCleanup(setattr, rd.ft_layout, "run_appless", old_appless)


class BackendTest(Base):
    def test_parse_avahi_combines_rfb_and_skips_loopback_and_tailscale(self):
        raw = "\n".join([
            "=;wlan0;IPv4;Studio\\032Mac;_rfb._tcp;local;mac.local;192.0.2.10;5900;",
            "=;wlanap;IPv4;Studio\\032Mac;_rfb._tcp;local;mac.local;198.51.100.10;5900;",
            "=;lo;IPv4;SteamFrame;_rfb._tcp;local;frame.local;127.0.0.1;5900;",
            "=;tailscale0;IPv4;Remote;_rfb._tcp;local;ts.local;100.64.0.2;5900;",
        ])
        self.assertEqual(rd.parse_avahi(raw),
                         [{"name": "Studio Mac", "address": "192.0.2.10", "dongle": "198.51.100.10"}])

    def test_sign_in_falls_back_to_appless_and_remove_cleans_apple_creds(self):
        self.b.signIn("Studio Mac", ADDRESS, "198.51.100.10", "alice", "secret")
        self.assertTrue(spin(self.app, lambda: self.signed))
        self.assertEqual(self.signed[-1], ("Studio Mac", True, ""))
        self.assertEqual(self.appless, [(("auth", ADDRESS, "--login", "--via", "198.51.100.10"), "alice\nsecret\n")])
        self.assertEqual(self.b._logins, {})
        self.assertFalse((HOSTS / f"{ADDRESS}.pin").exists())
        self.assertEqual(len(self.b.hosts), 1)
        h = self.b.hosts[0]
        self.assertEqual((h["kind"], h["hasToken"], h["direct"]), ("apple", True, "198.51.100.10"))

        ready = []
        self.b.monitorsReady.connect(lambda *a: ready.append(a))
        self.b.listMonitors(ADDRESS)
        self.assertTrue(spin(self.app, lambda: ready))
        self.assertEqual(ready[-1][0], ADDRESS)
        self.assertEqual([m["app"] for m in ready[-1][1]], ["apple:physical"])

        self.b.removeHost("Studio Mac")
        self.assertFalse(Path(rd.ft_hostapi.apple_path(ADDRESS)).exists())
        self.assertEqual(self.b.hosts, [])


class PageTest(Base):
    def setUp(self):
        super().setUp()
        Path(rd.ft_hostapi.apple_path(ADDRESS)).write_text("alice\nsecret\n")
        set_hosts({"name": "Studio Mac", "address": ADDRESS, "kind": "apple", "displays": []})
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

    def test_vibepollo_switches_are_hidden_on_apple_host_card(self):
        for name in ("sendMic", "input-keyboard", "input-mouse", "appWindows", "shareClipboard"):
            found = [i for i in items(self.root.contentItem()) if i.objectName() == name]
            self.assertEqual(len(found), 1, name)
            self.assertFalse(found[0].property("visible"), f"{name} must be hidden on an apple host")

    @staticmethod
    def _val(v):
        return v.toVariant() if hasattr(v, "toVariant") else v

    def test_display_dialog_picks_main_or_4k_retina_virtual_display(self):
        dialog = self.root.findChild(QObject, "displayDialog")
        dialog.setProperty("host", "Studio Mac")
        dialog.setProperty("address", ADDRESS)
        dialog.setProperty("kind", "apple")
        dialog.setProperty("added", [])
        QMetaObject.invokeMethod(dialog, "open")
        self.b.listMonitors(ADDRESS)
        self.assertTrue(spin(self.app, lambda: len(self._val(dialog.property("monitors")) or []) == 1))
        self.assertEqual(self._val(dialog.property("chosen")), {"apple:physical": True})
        self.assertEqual(dialog.property("count"), 1)

        # Ticking virtual display unticks Main display on an apple host and queues 3840x2160 apple:virtual.
        vcheck = self.root.findChild(QObject, "virtualDisplayCheck")
        vcheck.setProperty("checked", True)
        QMetaObject.invokeMethod(vcheck, "toggled")
        spin(self.app, seconds=0.1)
        self.assertEqual(self._val(dialog.property("chosen")), {})
        self.assertTrue(dialog.property("virtualChosen"))
        self.assertEqual(dialog.property("count"), 1)

        QMetaObject.invokeMethod(dialog, "accept")
        spin(self.app, seconds=0.1)
        self.assertEqual(len(self.ran), 1)
        args, stdin = self.ran[0]
        self.assertIsNone(stdin)
        self.assertEqual(args[:9], ("remote", "add", "Studio Mac", ADDRESS, "apple:virtual",
                                    "--label", "Virtual", "--size", "3840x2160"))


if __name__ == "__main__":
    unittest.main()
