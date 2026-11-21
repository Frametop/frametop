"""Remote Displays' hosts paired by PIN (issue 107: an unmodified Sunshine, or any other Moonlight
host with no Web UI login to give): what the backend keeps and runs, how it finds the PIN in
ft-layout's output as it comes, and the dialog that shows it. Same stand-ins as
test_app_windows.py (HOME a temp folder, ft-layout not run). Needs PySide6 and Kirigami.

  python3 remote-displays/test/test_pin_pairing.py
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_app_windows import HOSTS, Base, rd, set_hosts, spin  # noqa: E402
import ft_settings  # noqa: E402
from PySide6.QtCore import QObject  # noqa: E402
from PySide6.QtQml import QQmlApplicationEngine  # noqa: E402

KVM = "192.0.2.44"


class FakeProc:
    """What _said reads from: ft-layout's output in the chunks it came in."""

    def __init__(self, *chunks):
        self.chunks = list(chunks)
        self.killed = False

    def readAllStandardOutput(self):
        return self.chunks.pop(0) if self.chunks else b""

    def kill(self):
        self.killed = True


class PinBackend(Base):
    def setUp(self):
        super().setUp()
        self.signed = []
        self.b.signedIn.connect(lambda *a: self.signed.append(a))

    def pin_host(self):
        return next(h for h in rd.ft_layout.load_layout()["hosts"] if h["name"] == "KVM")

    def test_a_pin_host_is_kept_with_no_token(self):
        self.b.addPinHost("KVM", KVM, "")
        self.assertEqual(self.signed, [("KVM", True, "")])
        self.assertEqual((self.pin_host()["address"], self.pin_host()["kind"]), (KVM, "pin"))
        self.assertEqual(list(HOSTS.glob(f"{KVM}*")), [], "nothing to keep: no token, no Web UI pin")
        h = next(x for x in self.b.hosts if x["name"] == "KVM")
        self.assertEqual((h["kind"], h["hasToken"]), ("pin", True), "ready for a display, no sign-in")

    def test_a_bad_address_or_name_keeps_nothing(self):
        self.b.addPinHost("KVM", "not an address!", "")
        self.b.addPinHost("PC", KVM, "")  # PC is another host's name
        self.assertEqual([ok for _, ok, _ in self.signed], [False, False])
        self.assertEqual([h["name"] for h in rd.ft_layout.load_layout()["hosts"]], ["PC"])

    def test_its_display_is_its_desktop_app(self):
        self.b.addPinHost("KVM", KVM, "")
        got = []
        self.b.monitorsReady.connect(lambda *a: got.append(a))
        self.b.listMonitors(KVM)
        self.assertTrue(spin(self.app, lambda: got))
        address, monitors, error = got[0]
        self.assertEqual((address, error, [m["app"] for m in monitors]), (KVM, "", ["Desktop"]))

    def test_adding_one_pairs_by_pin(self):
        ran = []
        self.b._run = lambda label, *args, stdin=None: ran.append((label, args, stdin))
        self.b.addPinHost("KVM", KVM, "")
        self.b.addDisplay("KVM", "Desktop", "Desktop", 1920, 1080, 60, 0, 0)
        label, args, stdin = ran[0]
        self.assertEqual(label, "Pairing Desktop with KVM")
        self.assertEqual((args[:5], args[-1], stdin), (("remote", "add", "KVM", KVM, "Desktop"), "--pin", None))

    def test_the_pin_is_said_once_as_it_comes(self):
        pins = []
        self.b.pairingPin.connect(lambda *a: pins.append(a))
        proc = FakeProc(b"host 192.0.2.44: Sunshine, not paired\nPI", b"N 0421: enter it on the host\n", b"paired.\n")
        for _ in range(3):
            self.b._said(proc, "Pairing Desktop with KVM")
        self.assertEqual(pins, [("Pairing Desktop with KVM", "0421")])
        self.assertIn("paired.", "".join(self.b._out))

    def test_cancel_ends_only_a_pairing(self):
        # by ft-layout remote cancel-pin: our own kill wouldn't reach ft-stream in its container
        started = []
        self.b._start_detached = started.append
        proc = FakeProc()
        self.b._proc, self.b._busy = proc, "Connecting"
        self.b.cancelPairing()
        self.assertEqual(started, [])
        self.b._busy = "Pairing Desktop with KVM"
        self.b.cancelPairing()
        self.assertEqual(len(started), 1)
        self.assertEqual((Path(started[0][-3]).name, started[0][-2:]), ("ft-layout", ["remote", "cancel-pin"]))
        self.assertFalse(proc.killed, "it ends as cancelled, by itself")
        self.b._proc, self.b._busy = None, ""


class PinPage(Base):
    def setUp(self):
        super().setUp()
        set_hosts({"name": "KVM", "address": KVM, "kind": "pin", "displays": []})
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

    def test_the_pin_shows_until_the_pairing_ends(self):
        dialog = self.root.findChild(QObject, "pinDialog")
        self.assertIsNotNone(dialog, "the PIN dialog is on the page")
        self.b._busy = "Pairing Desktop with KVM"
        self.b.busyChanged.emit()
        self.b.pairingPin.emit("Pairing Desktop with KVM", "0421")
        self.assertTrue(spin(self.app, lambda: dialog.property("opened")), "it opens with the PIN")
        self.assertEqual((dialog.property("pin"), dialog.property("what")), ("0421", "Pairing Desktop with KVM"))
        self.b._busy = ""
        self.b.busyChanged.emit()
        self.assertTrue(spin(self.app, lambda: not dialog.property("opened")), "it closes when the pairing ends")
        self.assertEqual([w for w in self.warnings if "not placed in the graphics scene" not in w], [])


if __name__ == "__main__":
    unittest.main()
