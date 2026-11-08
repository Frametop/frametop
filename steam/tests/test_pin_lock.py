"""The diagnostic never equates an absent/uninitialized PIN UI with an unlock."""
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
        self.assertEqual(self.state(), "unlocked-after-observed-lock")

    def test_false_at_start_is_not_unlocked(self):
        self.assertEqual(self.state(), "unknown")
        self.assertEqual(self.state(), "unknown")

    def test_mandatory_lock_then_dismissal(self):
        self.unlock()
        self.assertEqual(self.state(active=True, mandatory=True), "locked")

    def test_cancellable_dialog_cannot_authorize(self):
        self.assertEqual(self.state(active=True), "locked")
        self.assertEqual(self.state(), "unknown")

    def test_context_reload_invalidates_unlock(self):
        self.unlock()
        self.assertEqual(self.state(generation=2000), "unknown")

    def test_disconnect_invalidates_unlock(self):
        self.unlock()
        self.observer.invalidate()
        self.assertEqual(self.state(), "unknown")

    def test_malformed_data_invalidates_unlock(self):
        for value in (None, False, {}, {"active": False}, snapshot() | {"active": 0},
                      snapshot() | {"generation": float("nan")}, snapshot() | {"generation": True},
                      snapshot() | {"generation": 0}, snapshot() | {"unexpected": True}):
            self.unlock()
            self.assertEqual(self.observer.sample(value), "unknown")
            self.assertEqual(self.state(), "unknown")

    def test_reset_or_configuration_change_invalidates_unlock(self):
        for changes in ({"resetting": True}, {"configured": False}):
            self.unlock()
            self.assertEqual(self.state(**changes), "unknown")
            self.assertEqual(self.state(), "unknown")

    def test_inconsistent_props_do_not_authorize(self):
        self.unlock()
        self.assertEqual(self.state(mandatory=True), "unknown")
        self.assertEqual(self.state(), "unknown")

    def test_no_secret_is_requested(self):
        self.assertNotIn("strPIN", pin_lock.SNAPSHOT)
        self.assertNotIn("strOwnerAccountName", pin_lock.SNAPSHOT)
        self.assertNotIn("LockTicket", pin_lock.SNAPSHOT)


if __name__ == "__main__":
    unittest.main()
