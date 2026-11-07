"""Frametop Settings' Remote access page's backend (remote/remote_access.py), offline: remote-ctl.sh's
status in words, the switch writing REMOTE and starting or stopping remote access, the password
(8 letters and digits, readable only by you, hidden until shown), and copying. HOME is a temp
folder, and remote-ctl.sh is a fake script, never the real one on the host. Needs PySide6.

  python3 remote/test/test_remote_access.py
"""
import os
import stat
import sys
import tempfile
import time
import unittest
from pathlib import Path

os.environ["HOME"] = tempfile.mkdtemp(prefix="ft-remote-access-test-")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "remote"))
import remote_access as ra  # noqa: E402
from PySide6.QtGui import QGuiApplication  # noqa: E402

STATUS = {"running": "capable=1\nrunning=1\naddress=100.64.0.7\nname=frame\nport=5900\n",
          "stopped": "capable=1\nrunning=0\naddress=100.64.0.7\nname=frame\nport=5900\n",
          "incapable": "capable=0\nrunning=0\naddress=100.64.0.7\nport=5901\n"}


class Words(unittest.TestCase):
    def test_address(self):
        self.assertEqual(ra.address_text(ra.parse_status(STATUS["running"])), "frame  (100.64.0.7)")
        self.assertEqual(ra.address_text(ra.parse_status(STATUS["incapable"])), "100.64.0.7:5901")
        self.assertEqual(ra.address_text({}), "")

    def test_state(self):
        self.assertEqual(ra.state_text(ra.parse_status(STATUS["running"]), True), "On: waiting for viewers")
        self.assertEqual(ra.state_text(ra.parse_status(STATUS["stopped"]), False), "Off")
        self.assertEqual(ra.state_text(ra.parse_status(STATUS["incapable"]), True), "Turns on at the next desktop restart")
        self.assertIn("no address", ra.state_text({}, True))

    def test_new_password(self):
        path = Path(tempfile.mkdtemp()) / "creds" / "vnc-password"
        pw = ra.new_password(str(path))
        self.assertRegex(pw, r"^[A-Za-z0-9]{8}$")
        self.assertEqual(ra.read_password(str(path)), pw)
        self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(path.parent.stat().st_mode), 0o700)
        self.assertNotEqual(ra.new_password(str(path)), pw)


class Page(unittest.TestCase):
    def setUp(self):
        self.app = QGuiApplication.instance() or QGuiApplication([])
        self.tmp = Path(tempfile.mkdtemp())
        self.log = self.tmp / "ctl.log"
        self.status = self.tmp / "status"
        self.status.write_text(STATUS["stopped"])
        self.code = self.tmp / "code"
        self.code.write_text("0")
        ctl = self.tmp / "remote-ctl.sh"
        ctl.write_text(f'#!/bin/sh\necho "$1" >> {self.log}\n'
                       f'[ "$1" = status ] && cat {self.status}\nexit $(cat {self.code})\n')
        ctl.chmod(0o755)
        old = ra.CTL, ra.host_command
        ra.CTL, ra.host_command = str(ctl), lambda *cmd: list(cmd)
        self.addCleanup(lambda: (setattr(ra, "CTL", old[0]), setattr(ra, "host_command", old[1])))
        self.conf = self.tmp / "frametop.conf"
        self.conf.write_text("# mine\nREMOTE=0\n")
        self.pw = self.tmp / "vnc-password"
        self.b = ra.Backend(conf=str(self.conf), password_path=str(self.pw))
        self.messages = []
        self.b.message.connect(lambda text, err: self.messages.append((text, err)))

    def wait(self, until, seconds=5):
        end = time.monotonic() + seconds
        while not until() and time.monotonic() < end:
            self.app.processEvents()
            time.sleep(0.01)
        self.assertTrue(until())

    def ran(self):
        return self.log.read_text().split() if self.log.exists() else []

    def test_checks_only_while_shown(self):
        self.assertEqual(self.b.state, "Checking…")
        self.b.setActive(True)
        self.wait(lambda: self.b.state == "Off")
        self.assertEqual(self.b.address, "frame  (100.64.0.7)")
        self.assertTrue(self.b.poll.isActive())
        self.b.setActive(False)
        self.assertFalse(self.b.poll.isActive())

    def test_switch_on_and_off(self):
        self.b.setEnabled(True)
        self.assertTrue(self.b.busy)
        self.wait(lambda: self.messages)
        self.assertEqual(self.messages[-1], ("Remote access on", False))
        self.assertIn("REMOTE=1", self.conf.read_text())
        self.assertIn("# mine", self.conf.read_text())
        self.assertTrue(self.b.enabled)
        self.b.setEnabled(False)
        self.wait(lambda: len(self.messages) == 2)
        self.assertEqual(self.messages[-1], ("Remote access off", False))
        self.assertIn("REMOTE=0", self.conf.read_text())
        self.assertEqual([v for v in self.ran() if v != "status"], ["start", "stop"])

    def test_on_in_a_desktop_that_started_without_it(self):
        self.code.write_text(str(ra.NOT_CAPABLE))
        self.b.setEnabled(True)
        self.wait(lambda: self.messages)
        self.assertEqual(self.messages[-1], ("Remote access turns on at the next desktop restart", False))

    def test_password_hidden_until_shown(self):
        self.assertFalse(self.b.hasPassword)
        self.assertIn("first starts", self.b.password)
        self.b.copyPassword()
        self.assertTrue(self.messages[-1][1])
        self.b.changePassword()
        pw = ra.read_password(str(self.pw))
        self.assertEqual(self.b.password, ra.HIDDEN)
        self.b.setRevealed(True)
        self.assertEqual(self.b.password, pw)
        self.b.copyPassword()
        self.assertEqual(QGuiApplication.clipboard().text(), pw)
        self.assertEqual(self.messages[-1], ("Password copied", False))

    def test_new_password_restarts_a_running_one(self):
        self.status.write_text(STATUS["running"])
        self.b.setActive(True)
        self.wait(lambda: self.b.state == "On: waiting for viewers")
        self.b.changePassword()
        self.wait(lambda: "restart" in self.ran())
        self.wait(lambda: self.messages)
        self.assertEqual(self.messages[-1], ("New password set: remote access restarted", False))


if __name__ == "__main__":
    unittest.main()
