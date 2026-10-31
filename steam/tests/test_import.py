"""ft-steam and steamui import offline: a smoke test, the component had none. Nothing
connects to Steam's UI (CEF on port 8080); that needs Steam running on the Frame.

  python3 steam/tests/test_import.py
"""
import importlib.machinery
import importlib.util
import os
import sys
import unittest

STEAM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, STEAM)


class Import(unittest.TestCase):
    def test_imports(self):
        import steamui
        self.assertTrue(steamui.__doc__)
        loader = importlib.machinery.SourceFileLoader("ft_steam", os.path.join(STEAM, "ft-steam"))
        module = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
        loader.exec_module(module)   # its main() runs only as a script
        self.assertTrue(callable(module.main))


if __name__ == "__main__":
    unittest.main()
