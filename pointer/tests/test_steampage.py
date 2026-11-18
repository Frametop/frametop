"""Steam's page in SteamVR's dashboard: the strip's lease and the watch's expression."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("steampage", ROOT / "pointer/dashpatch/steampage.py")
steampage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(steampage)


class StripLeaseTests(unittest.TestCase):
    def test_held_until_it_lapses(self):
        lease = steampage.StripLease()
        self.assertFalse(lease.held(10.0))
        self.assertTrue(lease.take(b"steamstrip 1", 10.0))
        self.assertTrue(lease.held(10.0 + steampage.LAPSE_S - 0.1))
        self.assertFalse(lease.held(10.0 + steampage.LAPSE_S))

    def test_renewal_extends_it(self):
        lease = steampage.StripLease()
        lease.take(b"steamstrip 1", 10.0)
        lease.take(b"steamstrip 1", 11.0)
        self.assertTrue(lease.held(11.0 + steampage.LAPSE_S - 0.1))

    def test_zero_ends_it_at_once(self):
        lease = steampage.StripLease()
        lease.take(b"steamstrip 1", 10.0)
        self.assertTrue(lease.take(b"steamstrip 0", 10.5))
        self.assertFalse(lease.held(10.5))

    def test_other_messages_are_not_its(self):
        lease = steampage.StripLease()
        for msg in (b"gamepad", b"steamstrip", b"steamstrip 2", b"steamstrip 1 extra"):
            self.assertFalse(lease.take(msg, 10.0))
        self.assertFalse(lease.held(10.0))


class ExpressionTests(unittest.TestCase):
    def test_hide_flag_is_passed(self):
        self.assertTrue(steampage.expression(True).endswith("(true)"))
        self.assertTrue(steampage.expression(False).endswith("(false)"))

    def test_it_names_steams_page_and_scopes_the_rule_to_its_frame(self):
        e = steampage.expression(True)
        self.assertIn("'valve.steam.gamepadui.main'", e)
        self.assertIn("legacy-frame-controls-", e)
        self.assertIn("m_rgTheaterFrameStack", e)


if __name__ == "__main__":
    unittest.main()
