"""Exercise the real watch loop with fake Steam and an isolated private lease."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import pin_lock  # noqa: E402


def snapshot(**changes):
    value = dict(generation=1000, active=False, mandatory=False, resetting=False, configured=True)
    value.update(changes)
    return value


class Watch(unittest.TestCase):
    def run_watch(self, samples):
        samples = iter(samples)
        publications, requests = [], []
        def evaluate(expression):
            if expression == pin_lock.REQUEST_LOCK:
                requests.append(expression)
                return True
            value = next(samples)
            if isinstance(value, Exception):
                raise value
            return value
        def sleep(_seconds):
            return None
        with tempfile.TemporaryDirectory(prefix="ft-pin-watch-") as folder:
            with (patch.object(pin_lock.ft_pinlock, "STATE", Path(folder) / "state"),
                  patch.object(pin_lock.ft_pinlock, "enabled", return_value=True),
                  patch.object(pin_lock, "publish", side_effect=lambda permit, stamp: publications.append(permit)),
                  patch.object(pin_lock.steamui, "evaluate", side_effect=evaluate),
                  patch.object(pin_lock.signal, "signal"), patch.object(pin_lock.time, "sleep", sleep)):
                with self.assertRaises(StopIteration):
                    pin_lock.watch()
        return publications, requests

    def test_initial_false_requires_native_authentication(self):
        states, requests = self.run_watch([snapshot(), snapshot(active=True, mandatory=True), snapshot()])
        self.assertEqual(states, [False, False, False, True, False])
        self.assertEqual(len(requests), 1)

    def test_unknown_and_reset_revoke_access(self):
        for failure in (None, snapshot(resetting=True), pin_lock.steamui.SteamUnreachable("test")):
            states, _ = self.run_watch([snapshot(active=True, mandatory=True), snapshot(), failure])
            self.assertEqual(states[:3], [False, False, True])
            self.assertFalse(states[-1])
            self.assertFalse(states[-2])

    def test_no_pin_only_when_explicit_and_well_formed(self):
        states, requests = self.run_watch([None, snapshot(configured=False),
                                          snapshot(configured=False, generation=True),
                                          snapshot(configured=False, mandatory=True)])
        self.assertEqual(states, [False, False, True, False, False, False])
        self.assertEqual(requests, [])

    def test_atomic_private_publication(self):
        with tempfile.TemporaryDirectory(prefix="ft-pin-publish-") as folder:
            path = Path(folder) / "state"
            with patch.object(pin_lock.ft_pinlock, "STATE", path):
                pin_lock.publish(True, (1000, 999))
                self.assertEqual(path.read_text(), f"FTPIN1 1 1000 999 {os.getpid()}\n")
                self.assertEqual(path.stat().st_mode & 0o777, 0o600)
                pin_lock.publish(False, (1001, 1000))
                self.assertTrue(path.read_text().startswith("FTPIN1 0 "))
                self.assertEqual(list(Path(folder).iterdir()), [path])


if __name__ == "__main__":
    unittest.main()
