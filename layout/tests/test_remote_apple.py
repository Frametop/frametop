"""Apple High Performance Screen Sharing displays in ft-layout, offline: no headset or Mac."""
import importlib.util
import io
import json
import pathlib
import socket
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock


def load_layout_module(tmp):
    path = pathlib.Path(__file__).resolve().parents[1] / "ft_layout.py"
    spec = importlib.util.spec_from_file_location("remote_apple_layout", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CONF_PATH = str(tmp / "frametop.conf")
    module.LAYOUT_PATH = str(tmp / "layout.json")
    return module


class RemoteAppleTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.m = load_layout_module(pathlib.Path(self.tmp.name))
        self.stream_calls = []
        self.appless_calls = []

        def fake_stream(*args, timeout=60, stdin=None):
            self.stream_calls.append((args, stdin))
            return 0, "paired", ""

        def fake_appless(*args, timeout=30, stdin=None):
            self.appless_calls.append((args, stdin))
            return 0, "credentials ready", ""

        def no_screens():
            raise self.m.NoAnswer("ft-screens isn't running")

        self.m.run_stream = fake_stream
        self.m.run_appless = fake_appless
        self.m.screens_socket = no_screens
        self.m.save_layout({"hosts": []})

    def tearDown(self):
        self.tmp.cleanup()

    def hosts(self):
        with open(self.m.LAYOUT_PATH) as f:
            return json.load(f)["hosts"]

    def test_adds_retina_virtual_display_without_moonlight_pairing(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.m.remote_command(["add", "Studio Mac", "192.0.2.10", "apple:virtual"])
        self.assertEqual(self.stream_calls, [])
        self.assertEqual([c[0] for c in self.appless_calls], [("check", "192.0.2.10")])
        host = self.hosts()[0]
        self.assertEqual(host["kind"], "apple")
        d = host["displays"][0]
        self.assertEqual((d["id"], d["app"], d["label"], d["size"]),
                         ("studio-mac-virtual", "apple:virtual", "Virtual", [3840, 2160]))

    def test_login_flag_runs_appless_auth_with_stdin(self):
        out = io.StringIO()
        with mock.patch("sys.stdin", io.StringIO("alice\ns3cr3t\n")), redirect_stdout(out):
            self.m.remote_command(["add", "Mac", "192.0.2.10", "apple:physical", "--login"])
        self.assertEqual(self.stream_calls, [])
        self.assertEqual(self.appless_calls, [(("auth", "192.0.2.10", "--login"), "alice\ns3cr3t\n")])
        d = self.hosts()[0]["displays"][0]
        self.assertEqual((d["app"], d["label"], d["size"]), ("apple:physical", "Main", [1920, 1080]))

    def test_auth_subcommand_forwards_stdin_to_appless(self):
        out = io.StringIO()
        with mock.patch("sys.stdin", io.StringIO("s3cr3t\n")), redirect_stdout(out):
            self.m.remote_command(["auth", "mac.local", "--user", "alice", "--password-stdin"])
        self.assertEqual(self.appless_calls, [(("auth", "mac.local", "--user", "alice", "--password-stdin"), "s3cr3t\n")])

    def test_set_accepts_apple_virtual_scale_suffix_and_remove_skips_unpair(self):
        with redirect_stdout(io.StringIO()):
            self.m.remote_command(["add", "Mac", "192.0.2.10", "apple:virtual"])
        self.m.remote_command(["set", "mac-virtual", "app=apple:virtual@2", "size=4096x1152"])
        self.assertEqual(self.hosts()[0]["displays"][0]["app"], "apple:virtual@2")
        self.assertEqual(self.hosts()[0]["displays"][0]["size"], [4096, 1152])
        with redirect_stdout(io.StringIO()):
            self.m.remote_command(["remove", "mac-virtual"])
        self.assertEqual(self.hosts()[0]["displays"], [])
        self.assertEqual(self.stream_calls, [])

    def test_host_answers_probes_port_5900_for_apple_hosts(self):
        probed = []

        class DummyConn:
            def __enter__(self):
                return self

            def __exit__(self, *_):
                return False

        def fake_connect(addr_port, timeout=1.5):
            probed.append(addr_port)
            if addr_port[1] == 5900:
                return DummyConn()
            raise OSError("closed")

        host = {"name": "Mac", "address": "mac.local", "kind": "apple", "displays": []}
        with mock.patch.object(socket, "create_connection", side_effect=fake_connect):
            self.assertTrue(self.m.host_answers(host))
        self.assertEqual(probed, [("mac.local", 5900)])

    def test_set_and_host_configure_keyboard_layout_and_keymap(self):
        with redirect_stdout(io.StringIO()):
            self.m.remote_command(["add", "Studio Mac", "mac.local", "apple:virtual@1"])
        self.m.remote_command(["host", "Studio Mac", "kb_layout=qwerty", "keymap=leftalt:cmd"])
        self.m.remote_command(["set", "studio-mac-virtual", "kb_layout=dvorak", "keymap=caps:ctrl,lctrl:globe"])
        host = self.hosts()[0]
        disp = host["displays"][0]
        self.assertEqual(host["kb_layout"], "qwerty")
        self.assertEqual(host["keymap"], {"leftalt": "leftmeta"})
        self.assertEqual(disp["kb_layout"], "dvorak")
        self.assertEqual(disp["keymap"], {"capslock": "leftctrl", "leftctrl": "globe"})

        listed = io.StringIO()
        with redirect_stdout(listed):
            self.m.remote_command(["list"])
        self.assertIn("kb dvorak keymap leftalt:leftmeta,capslock:leftctrl,leftctrl:globe", listed.getvalue())

        self.m.remote_command(["set", "studio-mac-virtual", "keymap=leftctrl:fn"])
        self.assertEqual(self.hosts()[0]["displays"][0]["keymap"], {"leftctrl": "globe"})

        self.m.remote_command(["set", "studio-mac-virtual", "kb_layout=none", "keymap="])
        self.m.remote_command(["host", "Studio Mac", "kb_layout=", "keymap=none"])
        host = self.hosts()[0]
        disp = host["displays"][0]
        self.assertNotIn("kb_layout", host)
        self.assertNotIn("keymap", host)
        self.assertNotIn("kb_layout", disp)
        self.assertNotIn("keymap", disp)

        with self.assertRaises(RuntimeError):
            self.m.remote_command(["set", "studio-mac-virtual", "kb_layout=azerty-bogus"])
        with self.assertRaises(RuntimeError):
            self.m.remote_command(["set", "studio-mac-virtual", "keymap=capslock:notakey"])


    def test_pin_is_refused_for_a_mac(self):
        with self.assertRaisesRegex(RuntimeError, "--pin is for a Moonlight host"):
            self.m.remote_command(["add", "Mac", "192.0.2.10", "apple:virtual", "--pin"])
        self.assertEqual(self.hosts(), [])

    def test_at_sign_only_in_apple_apps(self):
        self.assertTrue(self.m.app_ok("apple:virtual@2"))
        self.assertFalse(self.m.app_ok("monitor@2"))
        with self.assertRaisesRegex(RuntimeError, "other characters"):
            self.m.remote_command(["add", "PC", "192.0.2.20", "monitor@2"])
        self.assertEqual(self.hosts(), [])

    def test_mac_and_moonlight_displays_never_share_a_host(self):
        self.m.save_layout({"hosts": [{"name": "Desk", "address": "192.0.2.20", "kind": "pin", "displays": []}]})
        with self.assertRaisesRegex(RuntimeError, "same kind of app"):
            self.m.remote_command(["add", "Desk", "192.0.2.20", "apple:virtual"])
        self.assertEqual(self.hosts()[0]["kind"], "pin")
        self.assertEqual(self.appless_calls, [])

    def test_failed_mac_check_saves_nothing(self):
        self.m.run_appless = lambda *args, timeout=30, stdin=None: (1, "", "no credentials in x.apple\n")
        with redirect_stdout(io.StringIO()), self.assertRaises(RuntimeError):
            self.m.remote_command(["add", "Mac", "192.0.2.10", "apple:virtual"])
        self.assertEqual(self.hosts(), [])
        self.assertEqual(self.stream_calls, [])


if __name__ == "__main__":
    unittest.main()
