"""Remote Displays imports offline: a smoke test, the component had none. HOME is a
temp folder; no Vibepollo host is reached. Pairing and streaming need a host and the headset
(the verify-remote-streams skill). Needs PySide6 (the image's python3).

  python3 remote-displays/tests/test_import.py
"""
import os
import sys
import tempfile
import unittest

os.environ["HOME"] = tempfile.mkdtemp(prefix="ft-remote-displays-test-")
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


class Import(unittest.TestCase):
    def test_imports(self):
        import ft_remote_displays
        self.assertTrue(ft_remote_displays.__doc__)


if __name__ == "__main__":
    unittest.main()
