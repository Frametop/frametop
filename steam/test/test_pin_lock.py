"""Explicit current-state acceptance still denies missing or inconsistent data."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import pin_lock  # noqa: E402


def snapshot(**changes):
    return dict(generation=1000, active=False, mandatory=False, resetting=False,
                configured=True, **changes)


class PinState(unittest.TestCase):
    def setUp(self):
        self.observer = pin_lock.Observer()

    def state(self, **changes):
        value = snapshot()
        value.update(changes)
        return self.observer.sample(value)

    def unlock(self):
        self.assertEqual(self.state(active=True, mandatory=True), "locked")
        self.assertEqual(self.state(), "unlocked")

    def test_valid_inactive_state_at_start_is_unlocked(self):
        self.assertEqual(self.state(), "unlocked")
        self.assertEqual(self.observer.reason, "steam-unlocked")

    def test_mandatory_lock_then_dismissal(self):
        self.unlock()
        self.assertEqual(self.state(active=True, mandatory=True), "locked")

    def test_cancellable_dialog_denies_until_dismissed(self):
        self.assertEqual(self.state(active=True), "locked")
        self.assertEqual(self.state(), "unlocked")

    def test_new_context_requires_current_valid_state(self):
        self.unlock()
        self.assertEqual(self.state(generation=2000, active=True, mandatory=True), "locked")
        self.assertEqual(self.state(generation=2000), "unlocked")

    def test_disconnect_does_not_prevent_fresh_state_recovery(self):
        self.unlock()
        self.observer.invalidate()
        self.assertEqual(self.observer.reason, "awaiting-steam-state")
        self.assertEqual(self.state(), "unlocked")

    def test_malformed_data_invalidates_unlock(self):
        for value in (None, False, {}, {"active": False}, snapshot() | {"active": 0},
                      snapshot() | {"generation": float("nan")}, snapshot() | {"generation": True},
                      snapshot() | {"generation": 0}, snapshot() | {"unexpected": True}):
            self.unlock()
            self.assertEqual(self.observer.sample(value), "unknown")
            self.assertEqual(self.state(), "unlocked")

    def test_reset_denies_until_current_state_clears(self):
        self.unlock()
        self.assertEqual(self.state(resetting=True), "unknown")
        self.assertEqual(self.state(), "unlocked")

    def test_no_pin_accepts_only_consistent_inactive_state(self):
        self.assertEqual(self.state(configured=False), "unlocked")
        self.assertEqual(self.observer.reason, "steam-pin-disabled")
        self.assertEqual(self.state(configured=False, active=True), "locked")
        self.assertEqual(self.state(configured=False, mandatory=True), "unknown")
        self.assertEqual(self.state(configured=False, resetting=True), "unknown")

    def test_inconsistent_props_do_not_authorize(self):
        self.unlock()
        self.assertEqual(self.state(mandatory=True), "unknown")
        self.assertEqual(self.state(), "unlocked")

    def test_no_secret_is_requested(self):
        self.assertNotIn("strPIN", pin_lock.SNAPSHOT)
        self.assertNotIn("strOwnerAccountName", pin_lock.SNAPSHOT)
        self.assertNotIn("LockTicket", pin_lock.SNAPSHOT)

    def test_observer_is_strictly_passive(self):
        source = (Path(__file__).resolve().parents[1] / "pin_lock.py").read_text()
        self.assertNotIn("SetActiveLockScreenProps", source)
        self.assertNotIn("REQUEST_LOCK", source)

    def test_diagnostic_reason_identifies_lost_proof(self):
        self.assertEqual(self.observer.sample(None), "unknown")
        self.assertEqual(self.observer.reason, "invalid-steam-snapshot")
        self.unlock()
        self.assertEqual(self.state(mandatory=True), "unknown")
        self.assertEqual(self.observer.reason, "inconsistent-lock-flags")
        self.assertEqual(self.state(), "unlocked")
        self.assertEqual(self.observer.reason, "steam-unlocked")


if __name__ == "__main__":
    unittest.main()
