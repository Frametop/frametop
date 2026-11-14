#!/usr/bin/env python3
"""conf/ft_desktopapps.py: GioUnix's DesktopAppInfo where GLib has it, Gio's where it doesn't,
and no deprecation warning from reading a .desktop file either way."""
import importlib
import os
import sys
import tempfile
import unittest
import warnings

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "conf"))
try:
    import gi
    gi.require_version("Gio", "2.0")
    from gi.repository import Gio  # noqa: F401
    import ft_desktopapps
    HAVE_GI = True
# AttributeError: a "gi" that isn't PyGObject (the dev box has an empty gi/ namespace).
except (ImportError, ValueError, AttributeError):
    HAVE_GI = False

ENTRY = "[Desktop Entry]\nType=Application\nName=Test app\nExec=true\n"


def has_giounix():
    try:
        gi.require_version("GioUnix", "2.0")
        from gi.repository import GioUnix  # noqa: F401
        return True
    except (ValueError, ImportError):
        return False


@unittest.skipUnless(HAVE_GI, "no PyGObject here")
class DesktopAppsTest(unittest.TestCase):
    def test_reads_a_desktop_file_without_a_warning(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "test-app.desktop")
            with open(path, "w") as f:
                f.write(ENTRY)
            with warnings.catch_warnings():
                warnings.simplefilter("error")
                info = ft_desktopapps.DesktopAppInfo.new_from_filename(path)
                self.assertEqual(info.get_name(), "Test app")
                self.assertIsInstance(info, ft_desktopapps.DesktopAppInfo)

    @unittest.skipUnless(HAVE_GI and has_giounix(), "this GLib has no GioUnix")
    def test_giounix_where_glib_has_it(self):
        from gi.repository import GioUnix
        self.assertIs(ft_desktopapps.DesktopAppInfo, GioUnix.DesktopAppInfo)

    def test_gio_where_glib_lacks_giounix(self):
        real = gi.require_version

        def no_giounix(namespace, version):
            if namespace == "GioUnix":
                raise ValueError("Namespace GioUnix not available")
            return real(namespace, version)

        gi.require_version = no_giounix
        try:
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")  # Gio's own name warns on a GLib that has GioUnix
                module = importlib.reload(ft_desktopapps)
                self.assertIs(module.DesktopAppInfo, Gio.DesktopAppInfo)
        finally:
            gi.require_version = real
            importlib.reload(ft_desktopapps)


if __name__ == "__main__":
    unittest.main()
