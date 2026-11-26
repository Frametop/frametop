"""Ensure host profiling reports a disjoint wall-time partition."""

import unittest

from hardware_host_profile import frame_breakdown


class HostProfileTests(unittest.TestCase):
    def fixture(self):
        return [
            {
                "host_phase_ms": {
                    "claim_ms": 1,
                    "ack_ms": 1,
                    "power_start_ms": 1,
                    "publish_wait_ms": 12,
                    "read_validate_ms": 3,
                },
                "wrapper_ms": 10,
                "host_profile": {"command_api_ms": 20, "replay_ms": 2},
            }
        ], {"replay_ms": 2, "input_pack_ms": 2, "head_read_ms": 1}

    def test_nested_replay_is_counted_once(self):
        commands, measured = self.fixture()
        result = frame_breakdown(30, 25, commands, measured)
        self.assertEqual(sum(result.values()), 30)
        self.assertEqual(result["completion_replay_ms"], 2)
        self.assertEqual(result["completion_other_ms"], 1)
        self.assertEqual(result["publication_over_wrapper_ms"], 2)
        self.assertEqual(result["body_other_ms"], 4)

    def test_all_requests_are_included(self):
        commands, measured = self.fixture()
        result = frame_breakdown(60, 50, commands * 2, {k: v * 2 for k, v in measured.items()})
        self.assertEqual(sum(result.values()), 60)
        self.assertEqual(result["native_wrapper_ms"], 20)
        self.assertEqual(result["completion_replay_ms"], 4)

    def test_replay_cannot_exceed_its_parent_scope(self):
        commands, measured = self.fixture()
        measured["replay_ms"] = 4
        with self.assertRaisesRegex(ValueError, "overlap"):
            frame_breakdown(30, 25, commands, measured)


if __name__ == "__main__":
    unittest.main()
