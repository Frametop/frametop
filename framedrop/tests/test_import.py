"""FrameDrop's installer progress window imports offline (DEE-311): a smoke test, the component
had none. The rest of framedrop/ is shell (syntax-gated by test-bash) and runs on a devkit or
the headset.

  python3 framedrop/tests/test_import.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "installer"))


def have_pygobject():
    try:
        import gi
        gi.require_version("Gtk", "4.0")
        from gi.repository import Gtk  # noqa: F401
        return True
    # AttributeError: a "gi" that isn't PyGObject (the dev image has an empty namespace one)
    except (ImportError, ValueError, AttributeError):
        return False


@unittest.skipUnless(have_pygobject(), "the progress window is GTK 4: no PyGObject here (it runs on the Frame)")
class Import(unittest.TestCase):
    def test_imports(self):
        import progress
        self.assertTrue(progress.__doc__ or hasattr(progress, "main"))


if __name__ == "__main__":
    unittest.main()
