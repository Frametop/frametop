"""Offline lease, suspend and file-validation checks; never touch the live lease."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "conf"))
import ft_pinlock as gate  # noqa: E402


class Lease(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="ft-pin-lease-")
        self.addCleanup(self.tmp.cleanup)
        self.file = Path(self.tmp.name) / "state"
        for target, value in (("STATE", self.file), ("enabled", lambda: True),
                              ("clocks", lambda: (1000, 1000))):
            p = patch.object(gate, target, value)
            p.start()
            self.addCleanup(p.stop)

    def write(self, text=None, mode=0o600):
        self.file.write_text(text or f"FTPIN1 1 900 900 {os.getpid()}\n")
        self.file.chmod(mode)

    def test_missing_denied(self):
        self.assertFalse(gate.allowed())

    def test_fresh_private_live_lease(self):
        self.write()
        self.assertTrue(gate.allowed())

    def test_denial_and_bad_wire(self):
        for text in ("", "FTPIN1 0 900 900 2\n", "FTPIN1 true 900 900 2\n", "1", "FTPIN2 1 900 900 2\n",
                     "FTPIN1 1 -1 900 2\n", "FTPIN1 1 900 900 2\nextra", "x" * 160):
            self.file.write_text(text)
            self.file.chmod(0o600)
            self.assertFalse(gate.allowed(), repr(text))

    def test_permissions_and_symlink_denied(self):
        self.write(mode=0o644)
        self.assertFalse(gate.allowed())
        target = self.file.with_suffix(".target")
        self.file.rename(target)
        target.chmod(0o600)
        self.file.symlink_to(target)
        self.assertFalse(gate.allowed())

    def test_nonregular_file_denied_without_blocking(self):
        os.mkfifo(self.file, 0o600)
        self.assertFalse(gate.allowed())

    def test_dead_observer_denied(self):
        self.write()
        with patch.object(gate.os, "kill", side_effect=ProcessLookupError):
            self.assertFalse(gate.allowed())

    def test_suspend_expiry_and_future_proof(self):
        self.assertTrue(gate.fresh(900, 900, 1000, 1000))
        self.assertFalse(gate.fresh(250, 250, 1000, 1000))
        self.assertFalse(gate.fresh(900, 900, 1300, 1000))
        self.assertFalse(gate.fresh(1100, 900, 1000, 1000))
        self.assertFalse(gate.fresh(900, 1100, 1000, 1000))

    def test_option_disabled_has_no_dependency_on_steam(self):
        with patch.object(gate, "enabled", return_value=False):
            self.assertTrue(gate.allowed())


if __name__ == "__main__":
    unittest.main()
