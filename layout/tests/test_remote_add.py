"""ft-layout remote add, without a headset or a host: a bad option fails before any pairing, a
client paired for a display that then isn't saved is unpaired, and --pin pairs with a PIN typed in
on the host (issue 107). ft-stream is a fake that records."""
import importlib.util
import io
import json
import os
import pathlib
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock
from contextlib import redirect_stdout


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import ft_pinpair as pinpair  # noqa: E402


def load_layout_module(tmp):
    path = pathlib.Path(__file__).resolve().parents[1] / "ft_layout.py"
    spec = importlib.util.spec_from_file_location("remote_add_layout", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CONF_PATH = str(tmp / "frametop.conf")  # none: the screens backend
    module.LAYOUT_PATH = str(tmp / "layout.json")
    return module


class RemoteAdd(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m = load_layout_module(pathlib.Path(self.tmp.name))
        self.calls = []
        self.unpair_code = 0

        def fake_stream(*args, timeout=60, stdin=None):  # never a real ft-stream or host
            self.calls.append(args)
            if args[0] == "unpair":
                return self.unpair_code, "" if self.unpair_code else "unpaired", "no host"
            return 0, "paired", ""

        def no_screens():
            raise self.m.NoAnswer("ft-screens isn't running")

        self.live_code = 0

        def fake_live(*args, timeout=60):  # --pin: what ft-stream prints goes on as it comes
            self.calls.append(args)
            print("PIN 0421: enter it on the host (its Web UI's PIN pairing), with the device name x", flush=True)
            return self.live_code, "PIN 0421: ...\npaired.\n", ""

        self.m.run_stream = fake_stream
        self.m.run_stream_live = fake_live
        self.m.screens_socket = no_screens
        self.m.save_layout({"hosts": []})

    def tearDown(self):
        self.tmp.cleanup()

    def add(self, *opts):
        out = io.StringIO()
        with redirect_stdout(out):
            self.m.remote_command(["add", "oma7", "192.0.2.30", "Desktop", *opts])
        return out.getvalue()

    def hosts(self):
        with open(self.m.LAYOUT_PATH) as f:
            return json.load(f)["hosts"]

    def test_adds_a_display(self):
        self.add("--fps", "90", "--bitrate", "40000", "--size", "1920x1080")
        self.assertEqual([c[0] for c in self.calls], ["pair"])
        d = self.hosts()[0]["displays"][0]
        self.assertEqual((d["fps"], d["bitrate"], d["size"]), (90, 40000, [1920, 1080]))

    def test_bad_options_never_pair(self):
        for opts in (("--fps", "60fps"), ("--fps", "-1"), ("--bitrate", "50M"), ("--bitrate", "1.5"),
                     ("--size", "big"), ("--size", "1920x")):
            with self.subTest(opts=opts):
                with self.assertRaisesRegex(RuntimeError, f"remote add: {opts[0]} is"):
                    self.add(*opts)
                self.assertEqual(self.calls, [])
                self.assertEqual(self.hosts(), [])

    def test_failure_after_pairing_unpairs(self):
        def broken_save(layout):
            raise self.m.LayoutFileError("the layout file doesn't read")

        self.m.save_layout = broken_save
        with self.assertRaises(self.m.LayoutFileError):
            self.add("--fps", "60")
        self.assertEqual([c[0] for c in self.calls], ["pair", "unpair"])
        self.assertEqual(self.calls[1], ("unpair", "192.0.2.30", "--id", "frametop-oma7-desktop"))
        self.assertEqual(self.hosts(), [])

    def test_a_failed_unpair_is_said(self):
        self.unpair_code = 1

        def full_disk(layout):
            raise OSError("disk full")

        self.m.save_layout = full_disk
        out = io.StringIO()
        with redirect_stdout(out), self.assertRaisesRegex(OSError, "disk full"):
            self.m.remote_command(["add", "oma7", "192.0.2.30", "Desktop", "--port", "48000"])
        self.assertEqual(self.calls[1][:2], ("unpair", "192.0.2.30:48000"))
        self.assertIn("unpairing frametop-oma7-desktop from 192.0.2.30 failed too: no host", out.getvalue())

    def test_pin_pairing_says_the_pin_and_saves_a_pin_host(self):
        out = self.add("--pin", "--size", "1920x1080")
        self.assertEqual(self.calls, [("pair", "192.0.2.30", "--id", "frametop-oma7-desktop", "--pin")])
        self.assertIn("PIN 0421", out)
        host = self.hosts()[0]
        self.assertEqual((host["kind"], host["displays"][0]["app"]), ("pin", "Desktop"))

    def test_pin_and_login_together_never_pair(self):
        with self.assertRaisesRegex(RuntimeError, "--login or --pin"):
            self.add("--pin", "--login")
        self.assertEqual(self.calls, [])

    def test_a_pin_never_typed_in_is_said(self):
        self.live_code = pinpair.TIMED_OUT
        with self.assertRaisesRegex(RuntimeError, "no PIN typed in on the host within 3 minutes"):
            self.add("--pin")
        self.assertEqual(self.hosts(), [])

    def during_the_pin(self, change):
        """change() runs while the PIN waits to be typed in, as another remote add might."""
        live = self.m.run_stream_live

        def waiting(*args, timeout=60):
            change()
            return live(*args, timeout=timeout)

        self.m.run_stream_live = waiting

    def other_display(self, host, n):
        self.m.remote_add_save(self.m.load_layout(), host, "192.0.2.40", 0, {
            "id": f"{host}-desktop-{n}", "client": f"frametop-{host}-{n}", "app": "Desktop", "label": "Desktop",
            "size": [1920, 1080], "fps": 60, "bitrate": 0, "metres": 1.0})

    def test_what_changed_while_the_pin_waited_stays(self):
        self.during_the_pin(lambda: self.other_display("dody", 1))
        self.add("--pin")
        self.assertEqual(sorted(h["name"] for h in self.hosts()), ["dody", "oma7"])

    def test_the_same_name_taken_while_the_pin_waited_gets_another(self):
        def same():
            self.m.remote_add_save(self.m.load_layout(), "oma7", "192.0.2.30", 0, {
                "id": "oma7-desktop", "client": "frametop-oma7-desktop-x", "app": "Desktop", "label": "Desktop",
                "size": [1920, 1080], "fps": 60, "bitrate": 0, "metres": 1.0})

        self.during_the_pin(same)
        self.assertIn("oma7-desktop-2\n", self.add("--pin"))
        self.assertEqual([d["id"] for d in self.hosts()[0]["displays"]], ["oma7-desktop", "oma7-desktop-2"])

    def test_full_while_the_pin_waited_unpairs(self):
        self.during_the_pin(lambda: [self.other_display("dody", n) for n in range(self.m.REMOTE_MAX)])
        with self.assertRaisesRegex(RuntimeError, f"at most {self.m.REMOTE_MAX}"):
            self.add("--pin")
        self.assertEqual(self.calls[-1], ("unpair", "192.0.2.30", "--id", "frametop-oma7-desktop"))
        self.assertEqual(len(self.hosts()[0]["displays"]), self.m.REMOTE_MAX)

    def test_a_pin_host_answers_on_its_gamestream_port(self):
        tried = []

        class Refused:
            def __init__(self, address, timeout):
                tried.append(address)
                raise OSError("refused")

        with mock.patch.object(self.m.socket, "create_connection", Refused):  # the real module's: put back
            self.m.host_answers({"address": "192.0.2.30", "kind": "pin", "displays": [{}]})
            self.m.host_answers({"address": "192.0.2.31", "displays": [{}]})
        self.assertEqual(tried, [("192.0.2.30", 47989), ("192.0.2.31", self.m.WEB_UI_PORT)])


class LivePin(unittest.TestCase):
    """ft_pinpair.run_live with real processes: a stand-in ft-stream that says a PIN and then
    waits for it forever, its command line the one ft-stream's would be. Its lines come as
    they're said, and the time running out, or remote cancel-pin, ends that program itself: it's
    in the container, where no signal to ft-layout, scripts/in-box or distrobox-host-exec
    reaches it."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        tmp = pathlib.Path(self.tmp.name)
        self.pid_file = tmp / "pid"
        os.mkfifo(tmp / "never")
        self.stub = tmp / "ft-stream"
        # exec -a: the waiting program's command line is "STUB ARGS", as a real ft-stream's is
        self.stub.write_text("#!/bin/bash\necho \"PIN 0421: enter it on the host\"\n"
                             f"echo $$ > {self.pid_file}\nexec 3<>{tmp / 'never'}\n"
                             'exec -a "$0 $*" cat <&3\n')
        self.stub.chmod(0o755)
        waiting = mock.patch.object(pinpair, "WAITING", str(tmp / "waiting"))
        waiting.start()
        self.addCleanup(waiting.stop)

        class NoKill:  # our own kill would only reach scripts/in-box: what's in the box must end it
            def __init__(self, *a):
                pass
            start = cancel = __init__

        no_kill = mock.patch.object(pinpair.threading, "Timer", NoKill)
        no_kill.start()
        self.addCleanup(no_kill.stop)

    def run_live(self, timeout):
        out = io.StringIO()
        with redirect_stdout(out):
            result = pinpair.run_live(str(self.stub), ("pair", "192.0.2.30", "--id", "frametop-x", "--pin"),
                                      lambda cmd: cmd, timeout=timeout)
        return result, out.getvalue()

    def wait_for_pid(self):
        for _ in range(200):
            if self.pid_file.exists() and self.pid_file.read_text().strip():
                return int(self.pid_file.read_text())
            time.sleep(0.05)
        self.fail("the stand-in ft-stream never started")

    def assert_gone(self):
        with self.assertRaises(ProcessLookupError, msg="ft-stream still waits for the PIN"):
            os.kill(int(self.pid_file.read_text()), 0)
        self.assertFalse(os.path.exists(pinpair.WAITING), "something still says a pairing waits")

    def test_the_pin_comes_at_once_and_the_time_ends_ft_stream(self):
        start = time.monotonic()
        (code, said, _), printed = self.run_live(timeout=1)
        self.assertLess(time.monotonic() - start, 10)
        self.assertEqual(code, pinpair.TIMED_OUT)
        self.assertIn("PIN 0421", printed)
        self.assertIn("PIN 0421", said)
        self.assert_gone()

    def test_cancel_ends_the_one_waiting(self):
        self.assertFalse(pinpair.cancel(), "none waits yet")
        result = []
        waiter = threading.Thread(target=lambda: result.append(self.run_live(timeout=30)))
        waiter.start()
        self.wait_for_pid()
        time.sleep(0.2)  # past the exec
        with open(pinpair.WAITING) as f:
            self.assertEqual(f.read().strip(), f"{self.stub} pair 192.0.2.30 --id frametop-x --pin")
        self.assertEqual(os.stat(pinpair.WAITING).st_mode & 0o777, 0o600)
        self.assertTrue(pinpair.cancel())
        waiter.join(10)
        self.assertFalse(waiter.is_alive())
        self.assertEqual(result[0][0][0], pinpair.CANCELLED)
        self.assert_gone()

    def test_cancel_ends_nothing_else(self):
        for odd in ("/usr/bin/sleep 60", "/a/ft-stream pair 1.2.3.4 --id x --pin; rm -r ~", ""):
            with self.subTest(odd=odd):
                pathlib.Path(pinpair.WAITING).write_text(odd + "\n")
                with mock.patch.object(pinpair.subprocess, "run") as run:
                    self.assertFalse(pinpair.cancel())
                run.assert_not_called()
        self.assertEqual(pinpair._ere("/a/b.c pair 1.2.3.4 --id x+y --pin"), r"/a/b\.c pair 1\.2\.3\.4 --id x\+y --pin")

    def test_its_own_exit_is_kept(self):
        self.stub.write_text("#!/bin/sh\necho paired\nexit 3\n")
        (code, said, _), _ = self.run_live(timeout=5)
        self.assertEqual((code, said), (3, "paired\n"))
        self.assertFalse(os.path.exists(pinpair.WAITING))


if __name__ == "__main__":
    unittest.main()
