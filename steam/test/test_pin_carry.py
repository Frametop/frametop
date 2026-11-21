"""The guard's carried unlock record: private, exact, and only for the same UI and boot."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import pin_carry  # noqa: E402

BOOT = "0d6a3c1e-5b7f-4c2a-9e8d-1f2a3b4c5d6e"


class Carry(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="ft-pin-carry-")
        self.addCleanup(self.tmp.cleanup)
        for p in (patch.object(pin_carry.ft_pinlock, "STATE", Path(self.tmp.name) / "state"),
                  patch.object(pin_carry, "boot_id", lambda: BOOT)):
            p.start()
            self.addCleanup(p.stop)
        self.file = Path(self.tmp.name) / "frametop-pin-carry"

    def test_round_trip_is_private_and_exact(self):
        pin_carry.save(1763700000123.456, (5000, 4000))
        self.assertEqual(pin_carry.path(), self.file)
        self.assertEqual(self.file.stat().st_mode & 0o777, 0o600)
        self.assertEqual(pin_carry.load(), (1763700000123.456, 5000, 4000, BOOT))
        self.assertEqual(list(Path(self.tmp.name).iterdir()), [self.file])
        pin_carry.save(1000, (5100, 4100))
        self.assertEqual(pin_carry.load(), (1000.0, 5100, 4100, BOOT))
        pin_carry.clear()
        pin_carry.clear()  # nothing there: no error
        self.assertIsNone(pin_carry.load())

    def test_matches_same_ui_boot_and_no_suspend(self):
        record = (1000.0, 5000, 4000, BOOT)
        self.assertTrue(pin_carry.matches(record, 1000, (7000, 6000)))
        self.assertTrue(pin_carry.matches(record, 1000.0, (5000 + 59_999, 4000 + 59_999)))
        self.assertFalse(pin_carry.matches(None, 1000, (7000, 6000)))
        self.assertFalse(pin_carry.matches(record, 1000.5, (7000, 6000)))           # another UI context
        self.assertFalse(pin_carry.matches(record, 1000, (7050, 6000)))             # 50 ms asleep
        self.assertFalse(pin_carry.matches(record, 1000, (5000 + 60_000, 4000 + 60_000)))  # too old
        self.assertFalse(pin_carry.matches(record, 1000, (4999, 3999)))             # from the future
        self.assertFalse(pin_carry.matches((1000.0, 5000, 4000, BOOT[::-1]), 1000, (7000, 6000)))

    def test_unreadable_boot_id_does_not_match(self):
        with patch.object(pin_carry, "boot_id", side_effect=OSError("no proc")):
            self.assertFalse(pin_carry.matches((1000.0, 5000, 4000, BOOT), 1000, (7000, 6000)))

    def test_bad_records_are_ignored(self):
        good = f"FTCARRY1 1000.0 5000 4000 {BOOT}\n"
        for text in ("", "FTCARRY1\n", good.replace("FTCARRY1", "FTCARRY2"), good + "extra",
                     good.replace("1000.0", "nan"), good.replace("1000.0", "-1000.0"),
                     good.replace("5000", "-5000"), good.replace(BOOT, "x" * 36), "x" * 160,
                     good.replace("1000.0", "1000.0é")):
            self.file.write_text(text)
            self.file.chmod(0o600)
            self.assertIsNone(pin_carry.load(), repr(text))

    def test_unsafe_files_are_ignored(self):
        self.file.write_text(f"FTCARRY1 1000.0 5000 4000 {BOOT}\n")
        self.file.chmod(0o640)
        self.assertIsNone(pin_carry.load())
        self.file.chmod(0o600)
        self.assertIsNotNone(pin_carry.load())
        target = Path(self.tmp.name) / "elsewhere"
        self.file.rename(target)
        self.file.symlink_to(target)
        self.assertIsNone(pin_carry.load())
        os.unlink(self.file)
        os.mkfifo(self.file, 0o600)
        self.assertIsNone(pin_carry.load())
        os.unlink(self.file)
        self.file.mkdir(mode=0o700)
        self.assertIsNone(pin_carry.load())


if __name__ == "__main__":
    unittest.main()
