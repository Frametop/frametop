"""The Debug button: host/debug-report.sh writes its file in Downloads with what's private hidden
(host/debug-mask.py), and the window finds the file it wrote. The script runs for real against a
made-up home folder (offline: no GitHub), with and without a stand-in Frametop release; the rest
is the window's logic without a display."""
import importlib.util
import os
import pathlib
import shutil
import re
import subprocess
import sys
import tempfile
import time
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "installer" / "host" / "debug-report.sh"
MASK = REPO / "installer" / "host" / "debug-mask.py"
FEATURES = REPO / "installer" / "host" / "debug-features.sh"
LIB = REPO / "installer" / "host" / "debug-lib.sh"
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
                                            "SCREEN_PIN_LEVEL=1\nHANDS_NPU=1\n")
        (conf / "frametop-layout.json").write_text(
            '{"hosts": [{"name": "PC", "address": "192.168.1.20", "auth_key": "s3cr3t", "pin": 4}]}')
        (conf / "frametop-input.json").write_text('{"key_bindings": {"Meta+K": "command:sshpass -p hunter2 ssh nas"}}')
        (conf / "frametop-commands.json").write_text(
            '{"commands": [{"name": "wake-nas", "run": "curl -u admin:pw9 http://nas/wake"}, {"name": "lights"}]}')
        cache = self.home / ".cache" / "frametop-installer"
        cache.mkdir(parents=True)
        (cache / "install.log").write_text(
            "\x1b[1m== 1/13 distrobox\x1b[0m\nalready installed: distrobox: 1.8.2.5\n"
            "cloning https://someone:ghp_xyz@github.com/x/y\nmail me at a.person@example.com\n"
            "paired aa:bb:cc:dd:ee:ff on 127.0.0.1 via 100.101.102.103\nheadset cv.ABCDEF123456\n")

    def tearDown(self):
        self.tmp.cleanup()

    def run_it(self, **more):
        env = {k: v for k, v in os.environ.items() if not k.startswith("XDG_")}
        env.update(HOME=str(self.home), FRAMETOP_DEBUG_OFFLINE="1", **more)
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
                        "Settings", "Recent warnings and errors", "Logs: the desktop and its screens",
                        "Logs: input and the pointer", "Logs: gaze and the eye tracker", "Logs: hand tracking",
                        "NPU hand tracking (ft-npud, on the DSP)", "Logs: power",
                        "Logs: Frametop Voice (ft-voiced's own lines, without what was heard)",
                        "Logs: Frametop Together (people's names left out, yours too)", "Logs: remote displays",
                        "Logs: floating windows and frunner",
                        "Logs: your commands (when each started and ended; not their output)",
                        "Logs: FrameDrop and the Bluetooth fixes", "Logs: SteamVR's compositor (errors, last 30)"):
            self.assertIn(f"\n===== {section}\n", text)
        self.assertIn("this one: 9.9.9-test", text)
        self.assertIn("POINTER=1", text)
        self.assertNotIn("# a comment", text)
        self.assertIn("frametop.conf: HANDS_NPU=1\n", text)

    def test_journal_lines_lose_the_hostname_in_any_locale(self):
        stubs = self.home / "stubs"
        stubs.mkdir()
        # (a stand-in journalctl that writes its month as the locale it's given would)
        (stubs / "journalctl").write_text('#!/bin/sh\n[ "$LC_ALL" = C ] && m=Nov || m=nov.\n'
                                          'echo "$m 25 17:56:30 $(cat /proc/sys/kernel/hostname) app[1]: up"\n')
        (stubs / "journalctl").chmod(0o755)
        text = self.run_it(PATH=f"{stubs}:{os.environ['PATH']}", LC_ALL="fr_FR.UTF-8")
        self.assertIn("Nov 25 17:56:30 <host> app[1]: up", text)

    def test_what_was_said_typed_or_run_and_who_was_there_stay_out(self):
        stubs = self.home / "stubs"
        stubs.mkdir()
        (stubs / "journalctl").write_text("""#!/bin/sh
v="Nov 25 10:00:00 box in-box[1]: 2026-11-25 10:00:00,001"
case "$*" in
  *"-p warning --since"*)
    echo "Nov 25 10:00:00 box dolphin[7]: KDirWatch: /home/x/Videos/WARNOUTPUT.mkv is gone"
    echo "Nov 25 10:00:00 box vrserver[8]: vrserver: a warning of SteamVR's" ;;
  *frametop-voice-whisper*)
    echo "Nov 25 10:00:00 box in-box[2]: [00:00:00.000 --> 00:00:02.000]  WHISPERHEARD"
    echo "Nov 25 10:00:00 box in-box[2]: whisper server listening" ;;
  *frametop-voice*)
    echo "$v INFO short window heard nothing; try 1 of 2: 'HEARDONE'"
    echo "$v INFO transcribed: 'HEARDTWO'"
    echo "Nov 25 10:00:00 box in-box[1]: HOOKSAID the assistant's answer"
    echo "$v INFO mic open" ;;
  *frametop-cmd-*)
    echo "Nov 25 10:00:00 box systemd[9]: Started Frametop command gonk."
    echo "Nov 25 10:00:00 box sh[10]: CMDOUTPUT" ;;
  *frametop-desktop*)
    echo "Nov 25 10:00:00 box dolphin[7]: KDirWatch: /home/x/Videos/APPOUTPUT.mkv is a file"
    echo "Nov 25 10:00:00 box kwin_wayland[8]: kwin: output WL-1 enabled" ;;
  *frametop-together*)
    echo "Nov 25 10:00:00 box ft-together[3]: ft-together: Alice Example (alice@github) at 100.101.1.2 port 7400"
    echo "Nov 25 10:00:01 box ft-together[3]: Bob Person came in."
    echo "Nov 25 10:00:02 box ft-together[3]: You are in Carol Name's room."
    echo "Nov 25 10:00:03 box ft-together[3]: dave@passkey asks to come in."
    echo "Nov 25 10:00:04 box in-box[4]: in a room of 3 as Erin Own, floor laid; 2 shown" ;;
  *frametop-input-relay*)
    echo "Nov 25 10:00:00 box python3[4]: typing: 2 characters a US layout (Steam) has no key for: TYPEDTEXT" ;;
  *ft-npud*)
    echo "Nov 25 10:00:00 box python[5]: self-test bit-exact; graph 769e4e61"
    echo "Nov 25 10:00:01 box python[5]: client connected" ;;
esac
""")
        (stubs / "journalctl").chmod(0o755)
        text = self.run_it(PATH=f"{stubs}:{os.environ['PATH']}")
        for gone in ("HEARDONE", "HEARDTWO", "HOOKSAID", "WHISPERHEARD", "CMDOUTPUT", "APPOUTPUT", "WARNOUTPUT",
                     "Alice", "alice", "Bob", "Carol", "dave", "Erin", "TYPEDTEXT", "client connected"):
            self.assertNotIn(gone, text)
        for kept in ("INFO mic open", "Started Frametop command gonk.", "kwin: output WL-1 enabled", "graph 769e4e61",
                     "<someone> came in.", "You are in <someone>'s room.", "<someone> asks to come in.",
                     "ft-together: <you> (<login>) at <tailnet-ip> port 7400", "has no key for: <hidden>",
                     "in a room of 3 as <you>, floor laid", "a warning of SteamVR's", "whisper server listening"):
            self.assertIn(kept, text)

    def test_an_older_report_sh_s_whole_session_log_is_left_out(self):
        scripts = self.home / ".local/share/frametop/releases/current/scripts"
        scripts.mkdir(parents=True)
        (scripts / "report.sh").write_text(
            'f="$HOME/r.txt"\nprintf "\\n===== Versions\\nv1\\n\\n===== /tmp/frametop-session.log (last 60 lines)\\n'
            'SESSIONAPP said a file\\n\\n===== Screens\\nkept\\n" >"$f"\necho "wrote $f"\n')
        text = self.run_it()
        self.assertNotIn("SESSIONAPP", text)
        self.assertIn("===== /tmp/frametop-session.log (last 60 lines)\n(left out here: it is above)\n", text)
        self.assertIn("===== Versions\nv1\n", text)
        self.assertIn("===== Screens\nkept\n", text)

    def test_a_cut_off_says_so(self):
        stubs = self.home / "stubs"
        stubs.mkdir()
        (stubs / "timeout").write_text(f'#!/bin/sh\n[ "$1" = 150 ] && exit 124\nexec {shutil.which("timeout")} "$@"\n')
        (stubs / "timeout").chmod(0o755)
        text = self.run_it(PATH=f"{stubs}:{os.environ['PATH']}")
        self.assertIn("(the feature logs stopped at 150 seconds: the rest of them is missing)", text)
        self.assertNotIn("stopped at 150 seconds", self.run_it())

    def test_a_service_folder_of_the_users_own_is_listed(self):
        own = self.home / "npud"
        own.mkdir()
        (own / "ft-npud.heads").write_text("x")
        (self.home / ".config" / "frametop.conf").write_text(f'HANDS_NPU="{own}"  # mine\n')
        text = self.run_it()
        self.assertIn(f"--- {own} (the heads file and the socket", text)
        self.assertIn("ft-npud.heads", text)

    def test_what_is_private_is_hidden(self):
        text = self.run_it()
        for secret in ("abc123", "s3cr3t", "ghp_xyz", "hunter2", "pw9", "curl -u", "a.person@example.com", "aa:bb:cc:dd:ee:ff", "ABCDEF123456",
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
        self.assertIn('"Meta+K": "command:<hidden>"', text)
        self.assertIn("frametop-commands.json: the names only\nwake-nas, lights\n", text)

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


class SessionLog(unittest.TestCase):
    def test_only_frametop_s_d_bus_s_and_the_portals_lines(self):
        with tempfile.TemporaryDirectory() as d:
            log = pathlib.Path(d) / "session.log"
            log.write_text("frametop: hand tracking on\n"
                           "dbus-daemon[5]: [session uid=1000 pid=5] Activating service name='org.kde.x' requested by "
                           "':1.9' (uid=1000 pid=7 comm=\"/usr/bin/okular /home/x/Documents/Ta\")\n"
                           "(okular:7): GLib-WARNING **: can't open /home/x/Documents/Taxes.pdf\n"
                           "APPSTDERR anything at all\n"
                           "(/usr/lib/xdg-desktop-portal:3): GLib-GObject-CRITICAL **: a portal's\n")
            r = subprocess.run(["bash", "-c", f'. "{LIB}"; sessionlog "$1" 10', "-", str(log)],
                               capture_output=True, text=True, check=True)
        self.assertEqual(r.stdout, "frametop: hand tracking on\n"
                         "dbus-daemon[5]: [session uid=1000 pid=5] Activating service name='org.kde.x' requested by "
                         "':1.9' (uid=1000 pid=7 comm=\"<hidden>\")\n"
                         "(/usr/lib/xdg-desktop-portal:3): GLib-GObject-CRITICAL **: a portal's\n")


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

    def test_what_round_two_found(self):
        self.assertEqual(self.m("connecting to 192.168.1.50. Then 10.0.0.5..."), "connecting to <lan-ip>. Then <lan-ip>...")
        self.assertEqual(self.m("1234 /frametop/x --token abc123 --password hunter2 -v"),
                         "1234 /frametop/x --token <hidden> --password <hidden> -v")
        self.assertEqual(self.m('"Meta+K": "command:curl -u a:b \\"http://x\\"", "n": 2'), '"Meta+K": "command:<hidden>", "n": 2')
        self.assertEqual(self.m("Authorization: token ghp_abc"), "Authorization: <hidden>")
        self.assertEqual(self.m("-----BEGIN OPENSSH PRIVATE KEY-----\nAAA\n-----END OPENSSH PRIVATE KEY-----\nok"),
                         "<hidden private key>\nok")
        self.assertEqual(self.m("gamepc.tail1a2b.ts.net is up"), "<tailnet-name> is up")

    def test_what_round_three_found(self):
        # the relay's journal: a command line isn't JSON, its quotes aren't escaped
        self.assertEqual(self.m('python3[812]: command:sh -c "echo hunter2 | sudo -S x"'), "python3[812]: command:<hidden>")
        self.assertEqual(self.m('"command:curl -H \\"Authorization: Bearer abc\\" https://x", "n": 2'),
                         '"command:<hidden>", "n": 2')
        for named in ("api-key=abc123", "X-API-Key: abc123", "--api-key abc123"):
            self.assertNotIn("abc123", self.m(named))
        self.assertEqual(self.m("- token expired"), "- token expired")

    def test_what_the_feature_logs_brought(self):
        self.assertEqual(self.m("typing: 6 characters the input method has no key for: привет"),
                         "typing: 6 characters the input method has no key for: <hidden>")
        self.assertEqual(self.m("bob@passkey and alice@github, not git@github.com"), "<login> and <login>, not <email>")
        self.assertEqual(self.m("18:00 click on 'Inbox (3) - Mail' (window)"), "18:00 click on '<hidden>' (window)")
        self.assertEqual(self.m("""menu on "Bob's Tax Return.pdf" (list)"""), "menu on '<hidden>' (list)")
        self.assertEqual(self.m("systemd[9]: Failed to start [systemd-run] /usr/bin/mpv /home/x/Videos/a.mkv."),
                         "systemd[9]: Failed to start [systemd-run] <hidden>")
        self.assertEqual(self.m(r"""click on 'it\'s "quoted" - Writer' (list)"""), "click on '<hidden>' (list)")
        self.assertEqual(self.m("1 /usr/bin/krdpserver --port 3389 -p Ab12Cd34 --certificate x.pem"),
                         "1 /usr/bin/krdpserver --port 3389 -p <hidden> --certificate x.pem")

    def test_an_unclosed_block_hides_only_its_line(self):
        self.assertEqual(self.m('warn: auth: [retry 1 of 3\n===== next\nkept'), 'warn: auth: "<hidden>"\n===== next\nkept')

    def test_long_runs_stay_quick(self):
        start = time.monotonic()
        self.m("-" * 20000 + "a." * 10000 + "ab12-" * 4000 + " " + "auth." * 8000 + " " + "tokentoken" * 10000)
        self.assertLess(time.monotonic() - start, 2)

    def test_what_stays(self):
        for keep in ("distrobox: 1.8.2.5", "SteamVR version 2.19.5.1", "sudo: a password is set", '"pin": 4',
                     "key_bindings", "std::string", "Nov 25 17:56:30 other-box x: y", "SCREEN_PIN_LEVEL=1",
                     "author: Jane", "user@1000.service: Failed", "--token --other"):
            self.assertEqual(self.m(keep), keep)


@unittest.skipUnless(Gtk, "no GTK 4 bindings here")
class Window(unittest.TestCase):
    def setUp(self):
        from frametop_installer import debug_page, host
        self.page, self.host = debug_page, host

    def test_the_script_is_put_on_the_host_with_the_others(self):
        for f in (SCRIPT, FEATURES, LIB, MASK):
            self.assertIn(f.name, self.host.HOST_FILES)
        for f in (SCRIPT, FEATURES, MASK):
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

    def test_debug_brings_back_a_report_being_made_and_starts_a_new_one_after(self):
        made = []

        class Fake:
            def __init__(self, parent):
                self.busy, self.destroyed, self.shown = True, False, 0
                made.append(self)

            def present(self):
                self.shown += 1

            def destroy(self):
                self.destroyed = True
        real, self.page.DebugWindow = self.page.DebugWindow, Fake
        try:
            owner = self.page.DebugPage()
            owner.show_debug()
            owner.show_debug()                      # still being made (its window may be hidden)
            self.assertEqual((len(made), made[0].shown), (1, 2))
            made[0].busy = False
            made[0].unseen = True
            owner.show_debug()                      # it ended while hidden: shown, once
            self.assertEqual((len(made), made[0].shown, made[0].unseen), (1, 3, False))
            owner.show_debug()                      # done and seen: a new report
            self.assertEqual((len(made), made[0].destroyed, made[1].shown), (2, True, 1))
        finally:
            self.page.DebugWindow = real

    def test_closing_only_hides_it(self):
        self.assertIn("hide_on_close=True", (REPO / "installer/frametop_installer/debug_page.py").read_text())

    def test_it_leaves_the_installers_own_page_alone(self):
        # (it used to replace it, and an install's progress page stops updating once it isn't shown)
        source = (REPO / "installer" / "frametop_installer" / "debug_page.py").read_text()
        self.assertEqual(re.findall(r"self\.(?:stack|page|reveal)\b", source), [])

    def test_the_window_and_the_file_say_the_same_who_to_send_it_to(self):
        lines = re.findall(r'^  echo "(.*)"$', SCRIPT.read_text(), re.M)
        start = next(i for i, line in enumerate(lines) if line.startswith("Send this file"))
        self.assertEqual(" ".join(lines[start:start + 2]), self.page.CONTACT.replace("Send it", "Send this file"))

    def test_the_window_waits_longer_than_the_script(self):
        script = SCRIPT.read_text()
        # (a sanity bound: the t SECONDS calls, the GitHub check's too, against the window's wait)
        budget = sum(int(n) for n in re.findall(r"\bt (\d+) ", script))
        self.assertLess(budget, self.page.TIMEOUT)


if __name__ == "__main__":
    unittest.main()
