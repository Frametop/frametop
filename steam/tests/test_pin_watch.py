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
    def run_watch(self, samples, *, pauses=None, query_ms=0):
        samples = iter(samples)
        publications = []
        clock = [1000, 1000]
        pauses = iter(pauses or [])
        def evaluate(expression):
            self.assertEqual(expression, pin_lock.SNAPSHOT,
                             "Guard must only read Steam state, never request locking/authentication")
            value = next(samples)
            clock[0] += query_ms
            clock[1] += query_ms
            if isinstance(value, Exception):
                raise value
            return value
        def sleep(_seconds):
            boot, mono = next(pauses, (150, 150))
            clock[0] += boot
            clock[1] += mono
        with tempfile.TemporaryDirectory(prefix="ft-pin-watch-") as folder:
            with (patch.object(pin_lock.ft_pinlock, "STATE", Path(folder) / "state"),
                  patch.object(pin_lock.ft_pinlock, "enabled", return_value=True),
                  patch.object(pin_lock.ft_pinlock, "clocks", side_effect=lambda: tuple(clock)),
                  patch.object(pin_lock, "publish", side_effect=lambda permit, stamp: publications.append(permit)),
                  patch.object(pin_lock.steamui, "evaluate", side_effect=evaluate),
                  patch.object(pin_lock.signal, "signal"), patch.object(pin_lock.time, "sleep", sleep)):
                with self.assertRaises(StopIteration):
                    pin_lock.watch()
        return publications

    def test_fresh_initialized_inactive_start_does_not_require_lock_cycle(self):
        states = self.run_watch([snapshot(), snapshot(active=True, mandatory=True), snapshot()])
        self.assertEqual(states, [False, True, False, True, False])

    def test_unknown_and_reset_revoke_access(self):
        for failure in (None, snapshot(resetting=True), pin_lock.steamui.SteamUnreachable("test")):
            states = self.run_watch([snapshot(active=True, mandatory=True), snapshot(), failure])
            self.assertEqual(states[:3], [False, False, True])
            self.assertFalse(states[-1])
            self.assertFalse(states[-2])

    def test_no_pin_only_when_explicit_and_well_formed(self):
        states = self.run_watch([None, snapshot(configured=False),
                                          snapshot(configured=False, generation=True),
                                          snapshot(configured=False, mandatory=True)])
        self.assertEqual(states, [False, False, True, False, False, False])

    def test_scheduling_gap_denies_before_fresh_query_recovers(self):
        states = self.run_watch(
            [snapshot(active=True, mandatory=True), snapshot(), snapshot()],
            pauses=[(150, 150), (800, 800)])
        self.assertEqual(states, [False, False, True, False, True, False])

    def test_suspend_discards_unexpired_unlock(self):
        states = self.run_watch(
            [snapshot(active=True, mandatory=True), snapshot(), snapshot()],
            pauses=[(150, 150), (300, 150)])
        self.assertEqual(states, [False, False, True, False, True, False])

    def test_slow_response_cannot_establish_unlock_proof(self):
        states = self.run_watch(
            [snapshot(active=True, mandatory=True), snapshot()], query_ms=800)
        self.assertFalse(any(states))

    def test_slow_no_pin_response_cannot_allow_access(self):
        states = self.run_watch([snapshot(configured=False)], query_ms=800)
        self.assertFalse(any(states))

    def test_repeated_unknown_never_requests_a_prompt(self):
        states = self.run_watch([None] * 20)
        self.assertFalse(any(states))

    def test_failed_observation_denies_then_recovers_from_fresh_inactive_state(self):
        states = self.run_watch([snapshot(active=True, mandatory=True), snapshot(),
                                 pin_lock.steamui.SteamUnreachable("test"), snapshot(), snapshot(),
                                 snapshot(active=True, mandatory=True), snapshot()])
        self.assertEqual(states, [False, False, True, False, True, True, False, True, False])

    def test_ui_restart_uses_current_snapshot_instead_of_old_unlock(self):
        states = self.run_watch([snapshot(active=True, mandatory=True), snapshot(),
                                 snapshot(generation=2000, active=True), None,
                                 snapshot(generation=2000, active=True, mandatory=True),
                                 snapshot(generation=2000)])
        self.assertEqual(states, [False, False, True, False, False, False, True, False])

    def test_uninitialized_then_loaded_settings_recovers_without_lock_cycle(self):
        states = self.run_watch([None, {}, snapshot()])
        self.assertEqual(states, [False, False, False, True, False])

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
