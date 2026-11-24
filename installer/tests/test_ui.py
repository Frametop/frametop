"""The Frametop Installer's window, without a display: it imports, its pages' mixins don't shadow
a GTK window method (the window would quietly call the wrong one), and every method a page calls
on the window exists. Skipped where GTK 4's Python bindings aren't installed."""
import ast
import pathlib
import subprocess
import sys
import tempfile
import threading
import types
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
PKG = REPO / "installer" / "frametop_installer"
sys.path.insert(0, str(REPO / "installer"))

try:
    import gi
    gi.require_version("Gtk", "4.0")
    from gi.repository import Gtk
except (ImportError, ValueError, AttributeError):   # (a gi folder with no bindings in it has no require_version)
    Gtk = None


@unittest.skipUnless(Gtk, "no GTK 4 bindings here")
class Window(unittest.TestCase):
    def setUp(self):
        from frametop_installer import ui
        self.ui = ui

    def test_pages_dont_shadow_gtk(self):
        for mixin in (self.ui.PasswordPages, self.ui.ProgressPage, self.ui.UpdatePage):
            clash = [n for n in vars(mixin) if not n.startswith("__") and hasattr(Gtk.ApplicationWindow, n)]
            self.assertEqual(clash, [], f"{mixin.__name__} shadows {clash}")

    def test_every_self_call_resolves(self):
        for name in ("ui.py", "password_pages.py", "progress_page.py", "update_page.py"):
            tree = ast.parse((PKG / name).read_text())
            called = {n.func.attr for n in ast.walk(tree)
                      if isinstance(n, ast.Call) and isinstance(n.func, ast.Attribute)
                      and isinstance(n.func.value, ast.Name) and n.func.value.id == "self"}
            missing = sorted(c for c in called if not hasattr(self.ui.Window, c))
            self.assertEqual(missing, [], f"{name} calls self.{missing}, which the window doesn't have")

    def test_work_off_the_main_loop_ends_in_done_or_failed(self):
        from gi.repository import GLib

        from frametop_installer.widgets import in_thread

        def run(work):
            got, loop = [], GLib.MainLoop()

            def end(kind):
                return lambda r: (got.append((kind, str(r))), loop.quit())
            in_thread(work, end("done"), end("failed"))
            GLib.timeout_add(3000, loop.quit)
            loop.run()
            return got

        self.assertEqual(run(lambda: 7), [("done", "7")])

        def boom():
            raise OSError("no disk")
        self.assertEqual(run(boom), [("failed", "no disk")])   # not a spinner forever

    def test_install_again_of_a_test_run_leaves_the_window_real(self):
        began = []
        win = types.SimpleNamespace(dry_run=False, password=None,
                                    state={"version": "0.3.0", "dry_run": True, "flags": ["--yes"]},
                                    begin=lambda *a, **kw: began.append((a, kw)))
        self.ui.Window.install_again(win)
        self.assertEqual(began, [((None, "0.3.0", "", ["--yes"], False), {"dry_run": True})])
        self.assertFalse(win.dry_run)   # Change choices after it installs for real

    def begun(self, win_dry_run=False, ready=True, **kw):
        """Window.begin with a password on a stand-in window: (the helper's arguments, what job.start
        got, the messages shown)."""
        ui, made, started, shown = self.ui, [], [], []

        class FakeHelper:
            def __init__(self, *a):
                made.append(a)
            give = close = lambda self, *a: None
            wait_ready = lambda self: ready   # noqa: E731
        saved = ui.Helper, ui.in_thread, ui.Gtk, ui.label, ui.host.stage, ui.job.start
        ui.Helper, ui.in_thread = FakeHelper, lambda work, done, failed: done(work())
        ui.Gtk, ui.label = types.SimpleNamespace(Spinner=lambda **k: None), lambda *a, **k: None
        ui.host.stage = lambda: None
        ui.job.start = lambda *a, **k: started.append((a, k)) or ""
        try:
            win = types.SimpleNamespace(dry_run=win_dry_run, helper=None, page=lambda *a, **k: None,
                                        ask_again="ask_again", helper_gone="helper_gone",
                                        forget_password=lambda: None, show_progress=lambda: shown.append("progress"),
                                        show_message=lambda *a: shown.append(a))
            ui.Window.begin(win, "pw", "0.3.0", "stable", ["--yes"], False, **kw)
        finally:
            ui.Helper, ui.in_thread, ui.Gtk, ui.label, ui.host.stage, ui.job.start = saved
        return made, started, shown

    def test_an_install_hears_when_its_password_helper_goes(self):
        made, started, shown = self.begun()
        self.assertEqual(made, [("ask_again", "helper_gone")])
        self.assertEqual((len(started), shown), (1, ["progress"]))

    def test_no_install_starts_before_its_password_helper_listens(self):
        made, started, shown = self.begun(ready=False)
        self.assertEqual(started, [])
        self.assertEqual(shown[0][0], "The install didn't start")
        self.assertIn("password helper", shown[0][1])

    def test_install_again_of_a_test_run_starts_a_test_run(self):
        made, started, shown = self.begun(win_dry_run=False, dry_run=True)
        self.assertIs(started[0][1]["dry_run"], True)
        made, started, shown = self.begun(win_dry_run=True)   # (and a test window's installs are test runs)
        self.assertIs(started[0][1]["dry_run"], True)

    def test_a_password_helper_that_stops_is_said_where_its_seen(self):
        class Seen:
            visible = None

            def set_visible(self, on):
                self.visible = on
        closed, logged = [], []
        page = types.SimpleNamespace(helper=types.SimpleNamespace(close=lambda: closed.append(True)), finished=False,
                                     state={"sudo": True}, ask_box=Seen(), helper_note=Seen(), add_log=logged.extend,
                                     stack=types.SimpleNamespace(get_visible_child_name=lambda: "progress"))
        self.ui.ProgressPage.helper_gone(page)
        self.assertEqual((closed, page.helper), ([True], None))
        self.assertEqual((page.ask_box.visible, page.helper_note.visible), (False, True))   # not only in Details
        self.assertRegex((PKG / "progress_page.py").read_text(),   # ...and it's on the page
                         r'self\.page\("progress",[^)]*self\.helper_note')

    def test_the_end_of_the_log_is_read_before_it_finishes(self):
        from frametop_installer import job
        with tempfile.TemporaryDirectory() as d:
            saved = job.LOG, job.RESULT
            job.LOG, job.RESULT = pathlib.Path(d, "install.log"), pathlib.Path(d, "result")
            try:
                shown, ended = [], []
                page = types.SimpleNamespace(finished=False, state={"version": "0.3.0", "started": 1},
                                             progress=job.Progress(), offset=0, add_log=shown.extend,
                                             finish=lambda where, rc: ended.append((where, rc)))
                page.read_log = lambda: self.ui.ProgressPage.read_log(page)
                job.LOG.write_bytes(b"== 2/13 3D mouse\n")
                page.read_log()                                 # a tick
                with open(job.LOG, "ab") as f:                  # then it failed, before the next one
                    f.write(b"podman: no space left on device\ninstall.sh: stopped")
                job.RESULT.write_text("1\n")
                self.ui.ProgressPage.unit_seen(page, "failed")
                self.assertEqual(shown, ["== 2/13 3D mouse", "podman: no space left on device", "install.sh: stopped"])
                self.assertEqual(ended, [("failed", 1)])
            finally:
                job.LOG, job.RESULT = saved


@unittest.skipUnless(Gtk, "no GTK 4 bindings here")
class Updates(unittest.TestCase):
    """What opening the window says about updates: a newer Frametop, and a newer installer."""

    def setUp(self):
        from frametop_installer import releases, selfupdate, ui, update_page
        self.ui, self.releases, self.selfupdate, self.page = ui, releases, selfupdate, update_page
        self.update = selfupdate.Update("0.3.0-exp.27", "https://x/FrametopInstaller.flatpak", "https://x/SHA256SUMS")

    def release(self, channel, version):
        return self.releases.Release(channel, version, f"v{version}", "abc1234", 1, "tested", "")

    def test_a_newer_frametop_than_the_installed_one_is_said_first(self):
        found = {"stable": self.release("stable", "0.3.0"), "experimental": self.release("experimental", "0.3.0-exp.27")}
        title, note, news = self.page.installed_note("0.3.0-exp.26", "experimental", found)
        self.assertEqual((title, news), ("Update Frametop", True))
        self.assertIn("Frametop 0.3.0-exp.27 is out. You have 0.3.0-exp.26.", note)
        title, note, news = self.page.installed_note("0.3.0-exp.27", "experimental", found)
        self.assertEqual((title, news), ("Install Frametop", False))
        self.assertIn("the newest experimental release", note)
        # stable's newest isn't news to an experimental install, and nothing is to a local build
        self.assertFalse(self.page.installed_note("0.3.0", "stable", found)[2])
        title, note, news = self.page.installed_note("0.3.0-local.077e678", "experimental", found)
        self.assertEqual((title, news), ("Install Frametop", False))
        self.assertIn("Installed now: Frametop 0.3.0-local.077e678 (experimental)", note)
        found["experimental"] = LookupError("couldn't reach GitHub")
        self.assertFalse(self.page.installed_note("0.3.0-exp.26", "experimental", found)[2])

    def window(self, own="0.3.0-exp.26", check=None, where="system", install=None, reopened=True, **kw):
        """A stand-in window with the installer's own update faked: (the window, what it showed)."""
        seen, page, su = [], self.page, self.selfupdate
        saved = (page.in_thread, page.Gtk, page.label, page.button, su.own_version, su.check, su.installation,
                 su.download, su.install, su.reopen)

        def put_back():
            (page.in_thread, page.Gtk, page.label, page.button, su.own_version, su.check, su.installation,
             su.download, su.install, su.reopen) = saved
        self.addCleanup(put_back)

        def in_thread(work, done, failed):
            try:
                r = work()
            except Exception as e:
                return failed(e)
            done(r)
        page.in_thread, page.Gtk = in_thread, types.SimpleNamespace(Spinner=lambda **k: None)
        page.label = lambda text="", **k: text
        page.button = lambda text, on_click, **k: (text, on_click)
        su.own_version = lambda: own
        su.check = check or (lambda own, get: self.update)
        su.installation = lambda: where
        su.download = lambda update: seen.append(("download", update.version)) or "/c/FrametopInstaller.flatpak"
        su.install = install or (lambda path, where: seen.append(("install", path, where)))
        su.reopen = lambda: seen.append(("reopen",)) or reopened
        win = types.SimpleNamespace(update_declined=False, get="get", dry_run=False, busy=False, close="close",
                                    page=lambda name, *w, buttons=(): seen.append((name, w, dict(buttons))),
                                    get_application=lambda: types.SimpleNamespace(quit=lambda: seen.append(("quit",))))
        for name in ("look_for_installer_update", "show_installer_update", "update_installer"):
            setattr(win, name, types.MethodType(getattr(self.ui.UpdatePage, name), win))
        for k, v in kw.items():
            setattr(win, k, v)
        return win, seen

    def test_a_newer_installer_is_offered_before_anything_else(self):
        win, seen = self.window()
        win.look_for_installer_update(lambda: seen.append(("then",)))
        name, widgets, buttons = seen[-1]
        self.assertEqual((name, list(buttons)), ("update", ["Not now", "Update"]))
        self.assertIn("This is Frametop Installer 0.3.0-exp.26. Version 0.3.0-exp.27 is out.", widgets[1])
        self.assertTrue(any("asks for your password" in w for w in widgets))   # installed for every user
        self.assertNotIn(("then",), seen)
        buttons["Not now"]()
        self.assertEqual(seen[-1], ("then",))
        win.look_for_installer_update(lambda: seen.append(("then again",)))    # ...and isn't offered twice
        self.assertEqual(seen[-1], ("then again",))

    def test_for_this_user_alone_it_doesnt_mention_a_password(self):
        win, seen = self.window(where="user")
        win.look_for_installer_update(lambda: None)
        self.assertFalse(any("password" in w for w in seen[-1][1]))

    def test_nothing_newer_or_no_answer_goes_straight_on(self):
        def offline(own, get):
            raise OSError("offline")
        for check in (lambda own, get: None, offline):
            win, seen = self.window(check=check)
            win.look_for_installer_update(lambda: seen.append(("then",)))
            self.assertEqual([s[0] for s in seen], ["loading", "then"])
        win, seen = self.window(own="")   # run from the repo: it doesn't even ask
        win.look_for_installer_update(lambda: seen.append(("then",)))
        self.assertEqual(seen, [("then",)])

    def test_update_installs_it_where_this_one_is_and_opens_the_new_one(self):
        win, seen = self.window()
        win.look_for_installer_update(lambda: seen.append(("then",)))
        seen[-1][2]["Update"]()
        self.assertEqual(seen[-4:], [("download", "0.3.0-exp.27"), ("install", "/c/FrametopInstaller.flatpak", "system"),
                                     ("reopen",), ("quit",)])
        self.assertFalse(win.busy)

    def test_an_update_that_fails_says_why_and_lets_you_go_on(self):
        def refused(path, where):
            raise self.selfupdate.UpdateError("Not authorized")
        win, seen = self.window(install=refused)
        win.look_for_installer_update(lambda: seen.append(("then",)))
        seen[-1][2]["Update"]()
        name, widgets, buttons = seen[-1]
        self.assertEqual((name, list(buttons)), ("update", ["Not now", "Try again"]))
        self.assertTrue(any("The update didn't install: Not authorized." in w for w in widgets))
        self.assertNotIn(("reopen",), seen)
        buttons["Not now"]()
        self.assertEqual(seen[-1], ("then",))

    def test_one_that_cant_open_itself_says_to_open_it(self):
        win, seen = self.window(reopened=False)
        win.look_for_installer_update(lambda: None)
        seen[-1][2]["Update"]()
        self.assertEqual(seen[-1][1][0], "Frametop Installer 0.3.0-exp.27 is installed")
        self.assertNotIn(("quit",), seen)

    def test_a_test_run_downloads_and_checks_it_but_installs_nothing(self):
        win, seen = self.window(dry_run=True)
        win.look_for_installer_update(lambda: seen.append(("then",)))
        seen[-1][2]["Update"]()
        self.assertEqual(seen[-2:], [("download", "0.3.0-exp.27"), ("then",)])
        self.assertFalse(any(s[0] in ("install", "reopen", "quit") for s in seen))

    def test_an_install_under_way_isnt_interrupted_for_it(self):
        # the look for a newer installer comes after "running" went to the progress page
        src = (PKG / "ui.py").read_text()
        self.assertLess(src.index('if where == "running":'), src.index("self.look_for_installer_update(go_on)"))


@unittest.skipUnless(Gtk, "no GTK 4 bindings here")
class PasswordHelper(unittest.TestCase):
    """The window's end of the host helper (askpass.Helper), with a stand-in helper process."""

    def helper(self, script):
        from frametop_installer import askpass, host
        saved = host.popen
        host.popen = lambda argv: subprocess.Popen([sys.executable, "-c", script], stdin=subprocess.PIPE,
                                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.addCleanup(setattr, host, "popen", saved)
        self.gone = []
        h = askpass.Helper(lambda: None, lambda: self.gone.append(True))
        self.addCleanup(h.close)
        return h

    def wait(self, h, timeout=5):
        """wait_ready off the main loop, as the window calls it."""
        from gi.repository import GLib
        got, loop = [], GLib.MainLoop()

        def work():
            got.append(h.wait_ready(timeout))
            GLib.idle_add(loop.quit)
        threading.Thread(target=work, daemon=True).start()
        GLib.timeout_add(8000, loop.quit)
        loop.run()
        return got

    def test_ready_once_it_listens(self):
        h = self.helper("import sys; print('ready', flush=True); sys.stdin.read()")
        self.assertEqual(self.wait(h), [True])
        self.assertEqual(self.gone, [])

    def test_one_that_dies_isnt_ready_and_says_so(self):
        h = self.helper("import sys; sys.exit(1)")   # as when its socket's folder isn't ours
        self.assertEqual(self.wait(h), [False])
        self.assertEqual(self.gone, [True])

    def test_one_that_never_listens_isnt_waited_for_forever(self):
        h = self.helper("import sys; sys.stdin.read()")
        self.assertEqual(self.wait(h, timeout=0.3), [False])


if __name__ == "__main__":
    unittest.main()
