"""The Frametop Installer's window, without a display: it imports, its pages' mixins don't shadow
a GTK window method (the window would quietly call the wrong one), and every method a page calls
on the window exists. Skipped where GTK 4's Python bindings aren't installed."""
import ast
import pathlib
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
PKG = REPO / "installer" / "frametop_installer"
sys.path.insert(0, str(REPO / "installer"))

try:
    import gi
    gi.require_version("Gtk", "4.0")
    from gi.repository import Gtk
except (ImportError, ValueError):
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


if __name__ == "__main__":
    unittest.main()
