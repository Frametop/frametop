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
        for mixin in (self.ui.PasswordPages, self.ui.ProgressPage):
            clash = [n for n in vars(mixin) if not n.startswith("__") and hasattr(Gtk.ApplicationWindow, n)]
            self.assertEqual(clash, [], f"{mixin.__name__} shadows {clash}")

    def test_every_self_call_resolves(self):
        for name in ("ui.py", "password_pages.py", "progress_page.py"):
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
