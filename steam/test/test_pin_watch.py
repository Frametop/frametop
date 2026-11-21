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
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="ft-pin-watch-")
        self.addCleanup(self.tmp.cleanup)
        self.clock = [1000, 1000]  # one boot: kept across guard runs

    def run_watch(self, samples, *, pauses=None, query_ms=0):
        samples = iter(samples)
        publications = []
        clock = self.clock
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
        with (patch.object(pin_lock.ft_pinlock, "STATE", Path(self.tmp.name) / "state"),
              patch.object(pin_lock.ft_pinlock, "enabled", return_value=True),
              patch.object(pin_lock.ft_pinlock, "clocks", side_effect=lambda: tuple(clock)),
              patch.object(pin_lock, "publish", side_effect=lambda permit, stamp: publications.append(permit)),
              patch.object(pin_lock.steamui, "evaluate", side_effect=evaluate),
              patch.object(pin_lock.signal, "signal"), patch.object(pin_lock.time, "sleep", sleep)):
            with self.assertRaises(StopIteration):
                pin_lock.watch()
        return publications

    def restart(self, boot_ms, mono_ms):
        """The guard is down for a while: boottime runs ahead of monotonic by any suspend."""
        self.clock[0] += boot_ms
        self.clock[1] += mono_ms

    def unlocked_guard(self):
        self.assertEqual(self.run_watch([snapshot(active=True, mandatory=True), snapshot()]),
                         [False, False, True, False])

    def test_initial_false_waits_for_steam_owned_lock_cycle(self):
        states = self.run_watch([snapshot(), snapshot(active=True, mandatory=True), snapshot()])
        self.assertEqual(states, [False, False, False, True, False])

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

    def test_scheduling_gap_resumes_the_same_unlock(self):
        states = self.run_watch(
            [snapshot(active=True, mandatory=True), snapshot(), snapshot()],
            pauses=[(150, 150), (800, 800)])
        self.assertEqual(states, [False, False, True, False, True, False])

    def test_scheduling_gap_without_a_record_requires_new_authentication(self):
        with patch.object(pin_lock.pin_carry, "save", side_effect=OSError("full")):
            states = self.run_watch(
                [snapshot(active=True, mandatory=True), snapshot(), snapshot()],
                pauses=[(150, 150), (800, 800)])
        self.assertEqual(states, [False, False, True, False, False, False])

    def test_suspend_discards_unexpired_unlock(self):
        states = self.run_watch(
            [snapshot(active=True, mandatory=True), snapshot(), snapshot()],
            pauses=[(150, 150), (300, 150)])
        self.assertEqual(states, [False, False, True, False, False, False])

    def test_slow_response_cannot_establish_unlock_proof(self):
        states = self.run_watch(
            [snapshot(active=True, mandatory=True), snapshot()], query_ms=800)
        self.assertFalse(any(states))

    def test_slow_no_pin_response_cannot_allow_access(self):
        states = self.run_watch([snapshot(configured=False)], query_ms=800)
        self.assertFalse(any(states))

    def test_repeated_unknown_never_requests_a_prompt(self):
        states = self.run_watch([snapshot()] * 20)
        self.assertFalse(any(states))

    def test_failed_observation_resumes_in_the_same_ui(self):
        states = self.run_watch([snapshot(active=True, mandatory=True), snapshot(),
                                 pin_lock.steamui.SteamUnreachable("test"), snapshot(), snapshot(),
                                 snapshot(active=True, mandatory=True), snapshot()])
        self.assertEqual(states, [False, False, True, False, True, True, False, True, False])

    def test_unlock_unseen_after_a_seen_lock_is_not_resumed(self):
        states = self.run_watch([snapshot(active=True, mandatory=True), snapshot(),
                                 snapshot(active=True, mandatory=True),
                                 pin_lock.steamui.SteamUnreachable("test"), snapshot(), snapshot()])
        self.assertEqual(states, [False, False, True, False, False, False, False, False])

    def test_restarted_guard_resumes_an_unlock_it_saw(self):
        self.unlocked_guard()
        self.restart(2000, 2000)
        self.assertEqual(self.run_watch([snapshot(), snapshot()]), [False, True, True, False])

    def test_restarted_guard_never_resumes_across_a_suspend(self):
        self.unlocked_guard()
        self.restart(5000, 2000)  # 3 s asleep: Steam's PIN-on-wake may not show yet
        self.assertEqual(self.run_watch([snapshot(), snapshot(active=True, mandatory=True), snapshot()]),
                         [False, False, False, True, False])

    def test_suspend_while_running_ends_the_record(self):
        self.run_watch([snapshot(active=True, mandatory=True), snapshot(), snapshot(), snapshot()],
                       pauses=[(150, 150), (3000, 150)])
        self.restart(2000, 2000)
        self.assertFalse(any(self.run_watch([snapshot()])))

    def test_restarted_guard_needs_the_same_ui_context(self):
        self.unlocked_guard()
        self.restart(2000, 2000)
        self.assertFalse(any(self.run_watch([snapshot(generation=2000), snapshot(generation=1000.5)])))

    def test_restarted_guard_needs_a_recent_record(self):
        self.unlocked_guard()
        self.restart(pin_lock.pin_carry.CARRY_MS, pin_lock.pin_carry.CARRY_MS)
        self.assertFalse(any(self.run_watch([snapshot()])))

    def test_restarted_guard_cannot_resume_past_a_lock_or_pin_change(self):
        for spent in (snapshot(active=True, mandatory=True), snapshot(resetting=True),
                      snapshot(configured=False), snapshot(mandatory=True)):
            self.unlocked_guard()
            self.run_watch([spent])
            self.restart(2000, 2000)
            self.assertFalse(any(self.run_watch([snapshot()])), spent)

    def test_restarted_guard_keeps_steam_lock_and_prompts(self):
        self.unlocked_guard()
        self.restart(2000, 2000)
        states = self.run_watch([snapshot(active=True), snapshot(), snapshot(active=True, mandatory=True)])
        self.assertEqual(states, [False, False, True, False, False])  # cancellable prompt, then its end

    def test_ui_restart_cannot_reuse_old_unlock(self):
        states = self.run_watch([snapshot(active=True, mandatory=True), snapshot(),
                                 snapshot(generation=2000), snapshot(generation=2000),
                                 snapshot(generation=2000, active=True, mandatory=True),
                                 snapshot(generation=2000)])
        self.assertEqual(states, [False, False, True, False, False, False, True, False])

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
