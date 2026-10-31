"""Display Settings imports offline: a smoke test, the component had none. HOME is a
temp folder; nothing is read from or sent to the host, Steam or ft-screens. Its saves go
through ft_store and ft_conf, whose own tests cover them. Needs PySide6 (the image's python3).

  python3 display-settings/tests/test_import.py
"""
import os
import sys
import tempfile
import unittest

os.environ["HOME"] = tempfile.mkdtemp(prefix="ft-display-settings-test-")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


class Import(unittest.TestCase):
    def test_imports(self):
        import ft_display_settings
        import steam_settings
        self.assertTrue(ft_display_settings.CONF_PATH.startswith(os.environ["HOME"]))
        self.assertTrue(callable(ft_display_settings.write_conf_value))
        self.assertTrue(steam_settings.__doc__)


if __name__ == "__main__":
    unittest.main()
