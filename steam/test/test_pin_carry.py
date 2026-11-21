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
GOOD = f"FTCARRY1 1000.0 5000 4000 {BOOT} 3 1\n"
RECORD = (1000.0, 5000, 4000, BOOT, (3, 1))
SUSPENDS = pin_carry.suspends  # the real one, before setUp patches it


class Carry(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="ft-pin-carry-")
        self.addCleanup(self.tmp.cleanup)
        self.suspends = (3, 1)
        for p in (patch.object(pin_carry.ft_pinlock, "STATE", Path(self.tmp.name) / "state"),
                  patch.object(pin_carry, "boot_id", lambda: BOOT),
                  patch.object(pin_carry, "suspends", lambda: self.suspends)):
            p.start()
            self.addCleanup(p.stop)
        self.file = Path(self.tmp.name) / "frametop-pin-carry"

    def write(self, text, mode=0o600):
        self.file.write_text(text)
        self.file.chmod(mode)

    def test_round_trip_is_private_and_exact(self):
        pin_carry.save(1763700000123.456, (5000, 4000), (3, 1))  # a real timeOrigin's size
        self.assertEqual(pin_carry.path(), self.file)
        self.assertEqual(self.file.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.file.read_text(), f"FTCARRY1 1763700000123.456 5000 4000 {BOOT} 3 1\n")
        self.assertEqual(pin_carry.load(), (1763700000123.456, 5000, 4000, BOOT, (3, 1)))
        self.assertEqual(list(Path(self.tmp.name).iterdir()), [self.file])
        pin_carry.save(1000, (5100, 4100), (3, 1))  # a JSON integer
        self.assertEqual(pin_carry.load(), (1000.0, 5100, 4100, BOOT, (3, 1)))
        pin_carry.clear()
        pin_carry.clear()  # nothing there: no error
        self.assertIsNone(pin_carry.load())
        pin_carry.save(1000, (5100, 4100), None)  # the kernel's counts unknown: no record
        self.assertFalse(self.file.exists())

    def test_matches_same_ui_boot_and_no_suspend(self):
        self.assertTrue(pin_carry.matches(RECORD, 1000, (7000, 6000)))
        self.assertTrue(pin_carry.matches(RECORD, 1000.0, (5000 + 59_999, 4000 + 59_999)))
        self.assertTrue(pin_carry.matches(RECORD, 1000, (7049, 6000)))               # 49 ms: clock noise
        self.assertFalse(pin_carry.matches(None, 1000, (7000, 6000)))
        self.assertFalse(pin_carry.matches(RECORD, 1000.5, (7000, 6000)))            # another UI context
        self.assertFalse(pin_carry.matches(RECORD, 1000, (7050, 6000)))              # 50 ms asleep
        self.assertFalse(pin_carry.matches(RECORD, 1000, (5000 + 60_000, 4000 + 60_000)))  # too old
        self.assertFalse(pin_carry.matches(RECORD, 1000, (4999, 3999)))              # from the future
        self.assertFalse(pin_carry.matches(RECORD[:3] + (BOOT[::-1], (3, 1)), 1000, (7000, 6000)))
        for tried in ((4, 1), (3, 2)):                                                # a suspend, or a failed one
            self.suspends = tried
            self.assertFalse(pin_carry.matches(RECORD, 1000, (7000, 6000)), tried)

    def test_unreadable_boot_or_suspends_do_not_match(self):
        with patch.object(pin_carry, "boot_id", side_effect=OSError("no proc")):
            self.assertFalse(pin_carry.matches(RECORD, 1000, (7000, 6000)))
        self.suspends = None
        self.assertFalse(pin_carry.matches(RECORD, 1000, (7000, 6000)))

    def test_suspend_counts_come_from_sysfs(self):
        stats = Path(self.tmp.name) / "suspend_stats"
        stats.mkdir()
        (stats / "success").write_text("12\n")
        (stats / "fail").write_text("0\n")
        with patch.object(pin_carry, "SUSPEND_STATS", stats):
            self.assertEqual(SUSPENDS(), (12, 0))
            (stats / "fail").write_text("x\n")
            self.assertIsNone(SUSPENDS())
        with patch.object(pin_carry, "SUSPEND_STATS", stats / "missing"):
            self.assertIsNone(SUSPENDS())

    def test_bad_records_are_ignored(self):
        self.write(GOOD)
        self.assertEqual(pin_carry.load(), RECORD)
        for text in ("", "FTCARRY1\n", GOOD.replace("FTCARRY1", "FTCARRY2"), GOOD + "extra",
                     GOOD.replace("1000.0", "nan"), GOOD.replace("1000.0", "-1000.0"),
                     GOOD.replace("1000.0", "1e+16"), GOOD.replace("5000", "-5000"),
                     GOOD.replace(BOOT, "x" * 36), GOOD.replace(" 3 1", " 3"), "x" * 160,
                     GOOD.replace("1000.0", "1000.0é")):
            self.write(text)
            self.assertIsNone(pin_carry.load(), repr(text))

    def test_unsafe_files_are_ignored(self):
        self.write(GOOD, 0o640)
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
