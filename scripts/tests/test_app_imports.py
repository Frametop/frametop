"""Frametop's small apps import offline: Frametop Settings and its display, remote displays and
remote access backends, ft-steam, and FrameDrop's progress window. Each loads in its own python3, with HOME a temporary folder and
Qt offscreen; nothing is read from or sent to the host, Steam or ft-screens. Their main() runs only
as a script, so loading one starts nothing. The GTK 4 ones skip without PyGObject (they run on the
Frame).

  python3 scripts/tests/test_app_imports.py
"""
import os
import subprocess
import sys
import tempfile
import unittest

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")

LOAD = """
import importlib.machinery, importlib.util, sys
sys.path.insert(0, {folder!r})
loader = importlib.machinery.SourceFileLoader({name!r}, {path!r})
module = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
loader.exec_module(module)
missing = [n for n in {names!r} if not hasattr(module, n)]
assert not missing, f"missing: {{missing}}"
"""


def have_pygobject():
    try:
        import gi
        gi.require_version("Gtk", "4.0")
        from gi.repository import Gtk  # noqa: F401
        return True
    # AttributeError: a "gi" that isn't PyGObject (the dev image has an empty namespace one)
    except (ImportError, ValueError, AttributeError):
        return False


class Imports(unittest.TestCase):
    def load(self, rel, names=(), check=""):
        """Load the file at rel (from the repo) as a module in a new python3; it must have names,
        and check (Python, with module and os) must hold."""
        path = os.path.join(REPO, rel)
        name = os.path.basename(rel).replace("-", "_").removesuffix(".py")
        code = LOAD.format(folder=os.path.dirname(path), name=name, path=path, names=list(names))
        if check:
            code += f"import os\nassert {check}, {check!r}\n"
        env = dict(os.environ, HOME=tempfile.mkdtemp(prefix="ft-app-imports-test-"), QT_QPA_PLATFORM="offscreen")
        p = subprocess.run([sys.executable, "-c", code], env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, f"{rel} doesn't load:\n{p.stderr[-2000:]}")

    def test_display_settings(self):
        # its settings file under the temporary HOME, not the real one
        self.load("display-settings/ft_display_settings.py", ["CONF_PATH", "write_conf_value"],
                  'module.CONF_PATH.startswith(os.environ["HOME"])')
        self.load("display-settings/steam_settings.py")

    def test_remote_displays(self):
        self.load("remote-displays/ft_remote_displays.py")

    def test_steam(self):
        self.load("steam/steamui.py")
        self.load("steam/ft-steam", ["main"])

    def test_remote_access(self):
        # its files under the temporary HOME, not the real one
        self.load("remote/remote_access.py", ["Backend", "new_password"],
                  'module.CONF.startswith(os.environ["HOME"]) and module.VNC_PASSWORD.startswith(os.environ["HOME"])')

    def test_settings(self):
        self.load("settings/ft_settings.py", ["Settings", "main"])

    @unittest.skipUnless(have_pygobject(), "the progress window is GTK 4: no PyGObject here (it runs on the Frame)")
    def test_framedrop_progress(self):
        self.load("framedrop/installer/progress.py")


if __name__ == "__main__":
    unittest.main()
