"""The Debug button: host/debug-report.sh writes its file in Downloads with what's private hidden
(host/debug-mask.py), and the window finds the file it wrote. The script runs for real against a
made-up home folder (offline: no GitHub), with and without a stand-in Frametop release; the rest
is the window's logic without a display."""
import importlib.util
import os
import pathlib
import subprocess
import sys
import tempfile
import types
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "installer" / "host" / "debug-report.sh"
MASK = REPO / "installer" / "host" / "debug-mask.py"
sys.path.insert(0, str(REPO / "installer"))

_spec = importlib.util.spec_from_file_location("debug_mask", MASK)
debug_mask = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(debug_mask)

try:
    import gi
    gi.require_version("Gtk", "4.0")
    from gi.repository import Gtk  # noqa: F401
except (ImportError, ValueError, AttributeError):
    Gtk = None


class Script(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.home = pathlib.Path(self.tmp.name)
        conf = self.home / ".config"
        conf.mkdir()
        (conf / "frametop.conf").write_text("# a comment\nPOINTER=1\nVIBEPOLLO_TOKEN=abc123  # it pairs\n"
                                            "SCREEN_PIN_LEVEL=1\n")
        (conf / "frametop-layout.json").write_text(
            '{"hosts": [{"name": "PC", "address": "192.168.1.20", "auth_key": "s3cr3t", "pin": 4}]}')
        cache = self.home / ".cache" / "frametop-installer"
        cache.mkdir(parents=True)
        (cache / "install.log").write_text(
            "\x1b[1m== 1/13 distrobox\x1b[0m\nalready installed: distrobox: 1.8.2.5\n"
            "cloning https://someone:ghp_xyz@github.com/x/y\nmail me at a.person@example.com\n"
            "paired aa:bb:cc:dd:ee:ff on 127.0.0.1 via 100.101.102.103\nheadset cv.ABCDEF123456\n")

    def tearDown(self):
        self.tmp.cleanup()

    def run_it(self):
        env = {k: v for k, v in os.environ.items() if not k.startswith("XDG_")}
        env.update(HOME=str(self.home), FRAMETOP_DEBUG_OFFLINE="1")
        r = subprocess.run(["bash", str(SCRIPT), "9.9.9-test"], env=env, capture_output=True, text=True,
                           timeout=240)
        self.assertEqual(r.returncode, 0, r.stderr)
        last = r.stdout.strip().splitlines()[-1]
        self.assertTrue(last.startswith("wrote "), r.stdout)
        path = pathlib.Path(last[len("wrote "):])
        self.assertEqual(path.parent, self.home / "Downloads")
        return path.read_text()

    def test_it_writes_the_report_with_who_to_send_it_to(self):
        text = self.run_it()
        self.assertTrue(text.startswith("Frametop debug report, "))
        self.assertIn("Deej__ on Discord", text.splitlines()[1])
        self.assertIn("dj_anuz@protonmail.com", text)   # the header isn't masked
        for section in ("The headset", "SteamVR", "Frametop Installer", "Frametop", "Services", "What's running",
                        "Settings", "Recent warnings and errors"):
            self.assertIn(f"\n===== {section}\n", text)
        self.assertIn("this one: 9.9.9-test", text)
        self.assertIn("POINTER=1", text)
        self.assertNotIn("# a comment", text)

    def test_what_is_private_is_hidden(self):
        text = self.run_it()
        for secret in ("abc123", "s3cr3t", "ghp_xyz", "a.person@example.com", "aa:bb:cc:dd:ee:ff", "ABCDEF123456",
                       "192.168.1.20", "100.101.102.103", "\x1b["):
            self.assertNotIn(secret, text)
        self.assertIn("VIBEPOLLO_TOKEN=<hidden>", text)
        self.assertIn('"auth_key": "<hidden>"', text)
        self.assertIn('"pin": 4', text)               # (a pin is a screen's, not a secret)
        self.assertIn('"address": "<lan-ip>"', text)
        self.assertIn("via <tailnet-ip>", text)
        self.assertIn("127.0.0.1", text)
        self.assertIn("distrobox: 1.8.2.5", text)     # a version isn't an address
        self.assertIn("SCREEN_PIN_LEVEL=1", text)

    def test_no_release_says_so(self):
        text = self.run_it()
        self.assertIn("no releases installed", text)
        self.assertNotIn("Frametop's own report", text)

    def test_a_releases_own_report_goes_in_and_its_file_goes(self):
        scripts = self.home / ".local/share/frametop/releases/current/scripts"
        scripts.mkdir(parents=True)
        made = self.home / "own-report.txt"
        (scripts / "report.sh").write_text(f'printf "ft-screens debug: token=OWNSECRET\\n" > "{made}"\n'
                                           f'echo "wrote {made}"\n')
        text = self.run_it()
        self.assertIn("\n===== Frametop's own report", text)
        self.assertIn("ft-screens debug: token=<hidden>", text)
        self.assertFalse(made.exists())


class Mask(unittest.TestCase):
    """What the review found getting through, and what must stay."""

    def m(self, text, host="frame"):
        return debug_mask.mask(text, host)

    def test_secrets_anywhere_on_a_line(self):
        self.assertEqual(self.m("Nov 25 17:56:30 frame app[12]: token=SECRET done\n"),
                         "Nov 25 17:56:30 <host> app[12]: token=<hidden> done\n")
        self.assertEqual(self.m("GET /pair?x=1&pin=4821&access_token=QS"), "GET /pair?x=1&pin=<hidden>&access_token=<hidden>")
        self.assertNotIn("XYZXYZ", self.m('curl -H "Authorization: Bearer XYZXYZ" https://x'))
        self.assertEqual(self.m("see https://bob:pw123@host/x"), "see https://<hidden>@host/x")

    def test_nested_json_values(self):
        self.assertEqual(self.m('"auth": {"user": "bob", "pass": "hunter2"},'), '"auth": "<hidden>",')
        self.assertEqual(self.m('"tokens": ["a", "b"], "n": 1'), '"tokens": "<hidden>", "n": 1')
        self.assertEqual(self.m('"api_token": "s\\"3", "x": 2'), '"api_token": "<hidden>", "x": 2')

    def test_addresses_and_names(self):
        self.assertEqual(self.m("to 8.8.8.8, 10.1.2.3, 172.20.0.1, 100.64.0.9, 127.0.0.1"),
                         "to <ip>, <lan-ip>, <lan-ip>, <tailnet-ip>, 127.0.0.1")
        self.assertEqual(self.m("via 2601:abcd:1234::5 and fe80::1:2:3, ::1 at 17:56:30.123"),
                         "via <ipv6> and <ipv6>, ::1 at 17:56:30.123")
        self.assertEqual(self.m('SSID="My Home WiFi" joined'), "SSID=<hidden> joined")
        self.assertEqual(self.m("a.person@example.com aa:bb:cc:dd:ee:ff cv.ABCDEF123456"),
                         "<email> xx:xx:xx:xx:xx:xx cv.<serial>")

    def test_what_stays(self):
        for keep in ("distrobox: 1.8.2.5", "SteamVR version 2.19.5.1", "sudo: a password is set", '"pin": 4',
                     "key_bindings", "std::string", "Nov 25 17:56:30 other-box x: y", "SCREEN_PIN_LEVEL=1"):
            self.assertEqual(self.m(keep), keep)


@unittest.skipUnless(Gtk, "no GTK 4 bindings here")
class Window(unittest.TestCase):
    def setUp(self):
        from frametop_installer import debug_page, host
        self.page, self.host = debug_page, host

    def test_the_script_is_put_on_the_host_with_the_others(self):
        for f in (SCRIPT, MASK):
            self.assertIn(f.name, self.host.HOST_FILES)
            self.assertTrue(os.access(f, os.X_OK))

    def test_it_finds_the_file_the_script_wrote(self):
        ran = []

        def run(argv, timeout):
            ran.append(argv)
            return 0, b"Collecting\nwrote /home/x/Downloads/frametop-debug-1.txt\n", b""
        path = self.page.collect("0.3.0", run=run, stage=lambda: pathlib.Path("/b"))
        self.assertEqual(path, pathlib.Path("/home/x/Downloads/frametop-debug-1.txt"))
        self.assertEqual(ran, [["bash", "/b/debug-report.sh", "0.3.0"]])

    def test_a_report_that_wasnt_written_says_why(self):
        with self.assertRaisesRegex(self.page.ReportError, "no space"):
            self.page.collect("0.3.0", run=lambda a, timeout: (1, b"", b"x\nno space\n"), stage=lambda: pathlib.Path("/b"))
        with self.assertRaisesRegex(self.page.ReportError, "exit status 124"):
            self.page.collect("0.3.0", run=lambda a, timeout: (124, b"", b""), stage=lambda: pathlib.Path("/b"))

    def test_the_report_window_doesnt_shadow_gtk(self):
        clash = [n for n in vars(self.page.DebugWindow) if not n.startswith("__") and hasattr(Gtk.Window, n)]
        self.assertEqual(clash, [])

    def test_every_self_call_resolves(self):
        import ast
        tree = ast.parse((REPO / "installer" / "frametop_installer" / "debug_page.py").read_text())
        called = {n.func.attr for n in ast.walk(tree)
                  if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute)
                  and isinstance(n.func.value, ast.Name) and n.func.value.id == "self"}
        missing = sorted(c for c in called if not hasattr(self.page.DebugWindow, c)
                         and not hasattr(self.page.DebugPage, c))
        self.assertEqual(missing, [])

    def test_a_report_that_ends_after_its_window_closed_leaves_it_be(self):
        shown = []
        win = types.SimpleNamespace(gone=True, busy=True, report_show=lambda *a, **k: shown.append(a))
        self.page.DebugWindow.report_done(win, pathlib.Path("/x/y.txt"))
        self.page.DebugWindow.report_failed(win, OSError("x"))
        self.assertEqual((shown, win.busy), ([], False))

    def test_the_window_waits_longer_than_the_script(self):
        script = SCRIPT.read_text()
        budget = sum(int(n) for n in __import__("re").findall(r"\bt (\d+) ", script))
        self.assertLess(budget, self.page.TIMEOUT)


if __name__ == "__main__":
    unittest.main()
