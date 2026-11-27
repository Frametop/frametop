"""Uninstalling from the Frametop Installer, offline: uninstall.sh --plan's lines read back, the
boxes and the --delete and --keep flags they give, what its exit status means, the service it
starts in (host commands recorded, nothing run), its steps on the progress bar, and the pages'
ends on stand-in windows (no display)."""
import pathlib
import re
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "installer"))
from frametop_installer import host, job, uninstall  # noqa: E402

try:
    import gi
    gi.require_version("Gtk", "4.0")
    from gi.repository import Gtk
except (ImportError, ValueError, AttributeError):
    Gtk = None

# What uninstall.sh --plan said on a headset with a release running, the dev container and no checkout
PLAN = """sudo yes
step1 yes
running ft-screens ft-pointer input relay desktop session
ask releases y 941M
ask settings n 247M
ask voice y 322M
ask avatars n 4.0K
ask recordings n 246G
ask container n -
ask distrobox y -
"""


class Plan(unittest.TestCase):
    def test_its_lines(self):
        p = uninstall.parse_plan(PLAN)
        self.assertEqual((p.step1, p.sudo, p.running), (True, True, "ft-screens ft-pointer input relay desktop session"))
        self.assertEqual(list(p.asks), ["releases", "settings", "voice", "avatars", "recordings", "container",
                                        "distrobox"])
        self.assertEqual((p.asks["voice"], p.asks["container"]), ((True, "322M"), (False, "")))
        self.assertTrue(p.anything)

    def test_nothing_left_and_lines_it_doesnt_know(self):
        p = uninstall.parse_plan("sudo no\nstep1 no\nask everything y 1G\nask voice\nnoise\n")
        self.assertEqual((p.step1, p.sudo, p.running, p.asks, p.anything), (False, False, "", {}, False))

    def test_every_question_has_a_box(self):
        """Each KEY uninstall.sh asks about (its --delete and --keep list) has a title here."""
        text = (REPO / "uninstall.sh").read_text()
        keys = text.split("[[ ${2:-} =~ ^(")[1].split(")$")[0].split("|")
        self.assertEqual(sorted(keys), sorted(uninstall.CHOICES))
        asked = set(re.findall(r'(?:^|\s)ask (\w+) "', text, re.M)) - {"frametop", "restart", "KEY"}   # (step 1's, and ask's own comment)
        self.assertEqual(asked, set(uninstall.CHOICES))

    def test_the_boxes_and_flags(self):
        p = uninstall.parse_plan(PLAN)
        self.assertEqual(uninstall.flags(p, {}), ["--delete", "releases", "--keep", "settings", "--delete", "voice",
                                                 "--keep", "avatars", "--keep", "recordings", "--keep", "container",
                                                 "--keep", "distrobox"])
        b = uninstall.boxes(p, {"releases": False, "settings": True, "container": True})
        self.assertEqual(b["releases"], (True, False))   # always goes
        self.assertEqual((b["settings"], b["container"], b["distrobox"]), ((True, True), (True, True), (True, True)))

    def test_distrobox_goes_only_with_the_build_container(self):
        p = uninstall.parse_plan(PLAN)
        self.assertEqual(uninstall.boxes(p, {"distrobox": True})["distrobox"], (False, False))
        no_dev = uninstall.parse_plan("ask distrobox y -\n")
        self.assertEqual(uninstall.boxes(no_dev, {})["distrobox"], (True, True))

    def test_what_its_exit_status_means(self):
        self.assertEqual([uninstall.outcome(rc) for rc in (0, 3, 5, None, 1, 2)],
                         ["done", "restart", "leftovers", "unfinished", "failed", "failed"])


class Start(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="ft-uninstall-")
        cache = pathlib.Path(self.tmp.name, "cache")
        self.saved = (host.CACHE, host.BIN, job.STATE, job.LOG, job.RESULT, host.run)
        host.CACHE, host.BIN = cache, cache / "bin"
        job.STATE, job.LOG, job.RESULT = cache / "state.json", cache / "install.log", cache / "result"
        self.calls = []
        host.run = lambda argv, input=None, timeout=60: self.calls.append(list(argv)) or (0, b"", b"")

    def tearDown(self):
        host.CACHE, host.BIN, job.STATE, job.LOG, job.RESULT, host.run = self.saved
        self.tmp.cleanup()

    def test_the_uninstall_command(self):
        self.assertEqual(job.start_uninstall(["--keep", "settings"], True), "")
        cmd = next(c for c in self.calls if c[0] == "systemd-run")
        self.assertIn(f"--unit={job.UNIT}", cmd)
        self.assertIn(f"--setenv=SUDO_ASKPASS={host.BIN / 'askpass'}", cmd)
        self.assertEqual(cmd[cmd.index("/usr/bin/bash"):],
                         ["/usr/bin/bash", str(host.BIN / "run-install.sh"), str(host.CACHE), "0", "--",
                          "/usr/bin/bash", str(host.BIN / "uninstall.sh"), "--yes", "--from-installer",
                          "--keep", "settings"])
        self.assertEqual((host.BIN / "uninstall.sh").read_bytes(), (REPO / "uninstall.sh").read_bytes())
        state = job.load_state()   # (an uninstall's state still loads: version is "")
        self.assertEqual((state["kind"], state["flags"], state["sudo"], state["restart"]),
                         ("uninstall", ["--keep", "settings"], True, False))

    def test_the_app_carries_it(self):
        """The bundle has its own uninstall.sh, staged from there with no repo or release about:
        uninstalling never depends on Frametop being (properly) installed."""
        build = (REPO / "installer/flatpak/build.sh").read_text()
        self.assertRegex(build, r'\ninstall -m755 "\$root/uninstall\.sh" "\$share/uninstall\.sh"\n')
        app = pathlib.Path(self.tmp.name, "app/share/frametop-installer")   # as /app/share/frametop-installer
        (app / "host").mkdir(parents=True)
        for name in host.HOST_FILES:
            (app / "host" / name).write_text(name)
        (app / "get.sh").write_text("get")
        (app / "uninstall.sh").write_text("the app's own")
        with mock.patch.object(host, "DATA", app):
            host.stage()
        self.assertEqual((host.BIN / "uninstall.sh").read_text(), "the app's own")

    def test_a_test_run_is_uninstall_sh_dry_run(self):
        job.start_uninstall([], False, dry_run=True)
        cmd = next(c for c in self.calls if c[0] == "systemd-run")
        self.assertFalse([c for c in cmd if "SUDO_ASKPASS" in c])
        self.assertEqual(cmd[-3:], ["--yes", "--from-installer", "--dry-run"])

    def test_its_steps(self):
        p = job.Progress()
        p.feed(b"\n\x1b[1m== Step 1 of 2: stop Frametop from starting\x1b[0m\nThis removes:\n")
        self.assertEqual(p.step, "Step 1 of 2: Stop Frametop from starting")
        self.assertAlmostEqual(p.fraction, 0.25)
        p.feed(b"\x1b[1m== Restart the headset to finish\x1b[0m\n")
        self.assertEqual(p.step, "Restart the headset to finish")
        p.feed(b"\x1b[1m== Step 2 of 2: delete what's left\x1b[0m\n")
        self.assertAlmostEqual(p.fraction, 0.75)


class Label:
    def __init__(self):
        self.text, self.visible, self.fraction = "", True, 0

    def set_label(self, t):
        self.text = t

    def set_visible(self, v):
        self.visible = v

    def set_fraction(self, f):
        self.fraction = f


@unittest.skipUnless(Gtk, "no GTK 4 bindings here")
class Pages(unittest.TestCase):
    def setUp(self):
        from frametop_installer import uninstall_page
        self.mod = uninstall_page
        self.ran = []
        for name, fake in (("button", lambda text, on_click, css=(): (text, on_click)),
                           ("label", lambda text="", **k: text),
                           ("in_thread", lambda work, done, failed: done(work()))):
            p = mock.patch.object(uninstall_page, name, fake)
            p.start()
            self.addCleanup(p.stop)
        p = mock.patch.object(uninstall_page.host, "run",
                              lambda argv, timeout=60: self.ran.append(argv) or (0, b"", b""))
        p.start()
        self.addCleanup(p.stop)

    def window(self, dry_run=False, state=None):
        pages, buttons = [], []
        win = types.SimpleNamespace(dry_run=dry_run, un_dry=False, state=state or {}, status=Label(), step=Label(),
                                    bar=Label(), note=Label(), close="close", show_uninstall="show_uninstall",
                                    show_versions="show_versions",
                                    done_buttons=types.SimpleNamespace(append=buttons.append),
                                    page=lambda name, *w, buttons=(): pages.append((name, w, [b[0] for b in buttons])),
                                    show_message=lambda *a: pages.append(("message", a, [])))
        for name in ("uninstall_finished", "show_uninstall_state", "show_finish_uninstall", "restart_headset",
                     "forget_installer_files", "test_run", "show_restart_needed", "uninstall_planned"):
            setattr(win, name, types.MethodType(getattr(self.mod.UninstallPages, name), win))
        return win, pages, buttons

    def test_a_restart_to_finish(self):
        win, _, buttons = self.window(state={"kind": "uninstall"})
        win.uninstall_finished(3)
        self.assertEqual(win.status.text, "Restart the headset to finish")
        self.assertEqual([b[0] for b in buttons], ["Close", "Restart now"])
        buttons[1][1]()
        self.assertEqual(self.ran, [["systemctl", "reboot"]])

    def test_a_test_run_never_restarts_or_removes_anything(self):
        win, pages, buttons = self.window(state={"kind": "uninstall", "dry_run": True})
        win.uninstall_finished(3)
        buttons[1][1]()
        win.uninstall_finished(0)
        self.assertEqual(self.ran, [])
        self.assertEqual(pages[0][1][0], "Test run")

    def test_done_takes_the_installers_own_files(self):
        win, _, _ = self.window(state={"kind": "uninstall"})
        win.uninstall_finished(0)
        self.assertEqual(win.status.text, "Frametop is uninstalled")
        self.assertIn("flatpak uninstall --delete-data io.github.frametop.Installer", win.step.text)
        self.assertEqual(self.ran, [["rm", "-rf", "--", str(host.CACHE)]])

    def test_what_needed_the_password_stays(self):
        win, _, buttons = self.window(state={"kind": "uninstall"})
        win.uninstall_finished(5)
        self.assertEqual([b[0] for b in buttons], ["Try again", "Close"])
        self.assertEqual(self.ran, [])   # (the state stays, to try again)

    def test_finish_before_the_restart_says_restart(self):
        win, pages, _ = self.window()
        win.uninstall_planned(uninstall.parse_plan("step1 no\nrunning ft-screens\nask releases y 1G\n"),
                              ["--delete", "releases"])
        self.assertEqual((pages[-1][0], pages[-1][2]), ("restart", ["Close", "Restart now"]))

    def test_finish_with_nothing_left_stops_offering_it(self):
        win, pages, _ = self.window(state={"kind": "uninstall"})
        win.uninstall_planned(uninstall.parse_plan("sudo no\nstep1 no\n"), ["--delete", "releases"])
        self.assertEqual(pages[-1][0], "message")
        self.assertEqual(self.ran, [["rm", "-rf", "--", str(host.CACHE)]])

    def test_opened_again_after_the_first_step(self):
        win, pages, _ = self.window()
        with mock.patch.object(self.mod.job, "save_state") as saved:
            win.show_uninstall_state("failed", {"kind": "uninstall", "flags": ["--keep", "settings"],
                                                "started": 0}, 3)
        self.assertEqual((pages[-1][0], pages[-1][2]), ("resume", ["Not now", "Finish uninstalling"]))
        saved.assert_not_called()   # it offers to finish every time it opens, until it has


if __name__ == "__main__":
    unittest.main()
