"""ft-remote-settings imports offline (DEE-311): a smoke test, the component had none. HOME is
a temp folder; nothing is read from or written to the host.

  python3 remote/tests/test_import.py
"""
import importlib.machinery
import importlib.util
import os
import tempfile
import unittest

os.environ["HOME"] = tempfile.mkdtemp(prefix="ft-remote-settings-test-")
REMOTE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def have_pygobject():
    try:
        import gi
        gi.require_version("Gtk", "4.0")
        from gi.repository import Gtk  # noqa: F401
        return True
    # AttributeError: a "gi" that isn't PyGObject (the dev image has an empty namespace one)
    except (ImportError, ValueError, AttributeError):
        return False


@unittest.skipUnless(have_pygobject(), "ft-remote-settings is GTK 4: no PyGObject here (it runs on the Frame)")
class Import(unittest.TestCase):
    def test_imports(self):
        loader = importlib.machinery.SourceFileLoader("ft_remote_settings", os.path.join(REMOTE, "ft-remote-settings"))
        module = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
        loader.exec_module(module)   # its main runs only as a script
        self.assertTrue(callable(module.write_conf))


if __name__ == "__main__":
    unittest.main()
