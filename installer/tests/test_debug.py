"""The Debug button: host/debug-report.sh writes its file in Downloads with what's private hidden,
the window finds the file it wrote, and pages that finish while the debug page is up don't take
it away. The script runs for real against a made-up home folder (offline: no GitHub, no Frametop
report); the rest is the window's logic without a display."""
import os
import pathlib
import subprocess
import sys
import tempfile
import types
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "installer" / "host" / "debug-report.sh"
sys.path.insert(0, str(REPO / "installer"))

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
        self.assertIn("192.168.x.x", text)
        self.assertIn("127.0.0.1", text)
        self.assertIn("distrobox: 1.8.2.5", text)     # a version isn't an address
        self.assertIn("SCREEN_PIN_LEVEL=1", text)


@unittest.skipUnless(Gtk, "no GTK 4 bindings here")
class Window(unittest.TestCase):
    def setUp(self):
        from frametop_installer import debug_page, host
        self.page, self.host = debug_page, host

    def test_the_script_is_put_on_the_host_with_the_others(self):
        self.assertIn("debug-report.sh", self.host.HOST_FILES)
        self.assertTrue(os.access(SCRIPT, os.X_OK))

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

    def test_a_page_that_finishes_meanwhile_waits_for_back(self):
        shown = []
        win = types.SimpleNamespace(on_debug=True, debug_back="version", debugging=False,
                                    stack=types.SimpleNamespace(set_visible_child_name=shown.append,
                                                                get_child_by_name=lambda n: n))
        win.reveal = lambda name: self.page.DebugPage.reveal(win, name)
        win.reveal("progress")                 # an install's progress, made while the report shows
        self.assertEqual((shown, win.debug_back), ([], "progress"))
        win.reveal("debug")
        self.assertEqual(shown, ["debug"])
        self.page.DebugPage.debug_close(win)
        self.assertEqual((shown, win.on_debug), (["debug", "progress"], False))
        win.reveal("parts")                    # and after Back, pages show as before
        self.assertEqual(shown[-1], "parts")

    def test_back_waits_for_a_report_being_made(self):
        win = types.SimpleNamespace(on_debug=True, debugging=True)
        self.page.DebugPage.debug_close(win)
        self.assertTrue(win.on_debug)


if __name__ == "__main__":
    unittest.main()
