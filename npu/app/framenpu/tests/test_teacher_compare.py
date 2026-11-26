"""Comparison boundaries: compute the CPU prefix each frame, count requests,
include packing/staging, and keep ineligible precision modes out of timing."""

import importlib.util
from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / n) for n in ("src", "tests", "tools")]
import hardware_teacher_compare as C


class CompareFrameTests(unittest.TestCase):
    def test_mode_order_balances_each_position_with_two_or_three_modes(self):
        from collections import Counter

        for modes in (C.MODES, (C.MODES[0], C.MODES[2])):
            orders = [C.mode_order(modes, i) for i in range(2 * len(modes))]
            for position in range(len(modes)):
                self.assertEqual(
                    Counter(order[position] for order in orders),
                    Counter({mode: 2 for mode in modes}),
                )

    def test_cpu_prefix_is_computed_and_staged_inside_every_frame(self):
        events = []
        prefix, tail = SimpleNamespace(jobs=["prefix"]), SimpleNamespace(jobs=["tail"])
        outputs = {"8": object(), "10": object()}

        class Cpu:
            def compute(self, rgb):
                self.asserted = rgb
                events.append("cpu_compute")
                return outputs

        class Runner:
            def inputs(self, plan, blob):
                self.asserted = plan
                if blob("8") is not outputs["8"] or blob("10") is not outputs["10"]:
                    raise AssertionError("CPU outputs not staged")
                events.append("stage_features")

            def submit(self, plan, job, *, linked):
                events.append(job)
                return {"body_ms": 16, "wrapper_ms": 16.1, "wall_ms": 24}

            def read_values(self, plan, name, size):
                events.append("read_heads")
                return bytes(size)

        runner = Runner()
        runner.prefix, runner.tail = prefix, tail
        cpu = Cpu()
        for _ in range(2):
            sample, raw = C.frame(runner, object(), cpu)
            self.assertEqual(len(sample["jobs"]), 1)
            self.assertEqual(len(raw), 512)
            total = sum(
                sample[k]
                for k in (
                    "cpu_prefix_ms",
                    "input_stage_ms",
                    "prefix_submit_wall_ms",
                    "tail_submit_wall_ms",
                    "output_read_ms",
                )
            )
            self.assertAlmostEqual(total, sample["host_wall_ms"])
        self.assertEqual(events, ["cpu_compute", "stage_features", "tail", "read_heads"] * 2)

    def test_dsp_prefix_uses_two_requests_without_host_feature_staging(self):
        events = []

        class Runner:
            prefix = SimpleNamespace(jobs=["prefix"])
            tail = SimpleNamespace(jobs=["tail"])

            def stage_rgb(self, rgb):
                events.append("stage_rgb")

            def submit(self, plan, job, *, linked):
                events.append(job)
                return {"body_ms": 16, "wrapper_ms": 16.1, "wall_ms": 24}

            def read_values(self, plan, name, size):
                events.append("read_heads")
                return bytes(size)

        sample, _ = C.frame(Runner(), object())
        self.assertEqual(len(sample["jobs"]), 2)
        self.assertEqual(events, ["stage_rgb", "prefix", "tail", "read_heads"])
        total = sum(
            sample[k]
            for k in (
                "cpu_prefix_ms",
                "input_stage_ms",
                "prefix_submit_wall_ms",
                "tail_submit_wall_ms",
                "output_read_ms",
            )
        )
        self.assertAlmostEqual(total, sample["host_wall_ms"])


@unittest.skipUnless(importlib.util.find_spec("numpy"), "NumPy required")
class CompareSummaryTests(unittest.TestCase):
    def test_checkpoint_bounds_are_frozen_and_final_maps_require_exactness(self):
        import numpy as np

        tiny = 2.0**-24
        a, b = np.array([tiny]), np.array([2 * tiny])
        allow = np.array([2.0**-20])
        self.assertEqual(C.checkpoint_difference(a, b, allow)["different_values"], 1)
        with self.assertRaises(ValueError):
            C.checkpoint_difference(a, b, allow, final=True)
        with self.assertRaises(ValueError):
            C.checkpoint_difference(np.array([1.0]), np.array([1.001]), np.array([0.0001]))
        with self.assertRaises(ValueError):
            C.checkpoint_difference(np.array([tiny]), np.array([32 * tiny]), allow)

    def test_ineligible_modes_are_excluded_and_differences_are_paired(self):
        def sample(wall, n):
            return dict(
                host_wall_ms=wall,
                cpu_prefix_ms=1,
                input_stage_ms=2,
                prefix_submit_wall_ms=0,
                tail_submit_wall_ms=20,
                output_read_ms=0.1,
                jobs=[dict(wrapper_ms=16, wall_ms=24)] * n,
            )

        frames = {
            "cpu_fp32": [sample(35, 1), sample(30, 1)],
            "cpu_fp16": [],
            "dsp_prefix": [sample(50, 2), sample(65, 2)],
        }
        crops = {"a": {"comparison": {"frames": frames}}}
        with patch.object(C.HS, "summarize", return_value={}):
            summary = C.summarize(crops)["comparison"]
        self.assertEqual(summary["cpu_fp32"]["median_paired_wall_saving_ms"], 25)
        self.assertEqual(summary["cpu_fp32"]["dsp_total_ms"], 16)
        self.assertEqual(summary["dsp_prefix"]["dsp_total_ms"], 32)
        self.assertFalse(summary["cpu_fp16"]["eligible_on_all_crops"])
        self.assertEqual(summary["cpu_fp16"]["frames"], 0)
        self.assertNotIn("host_wall_ms", summary["cpu_fp16"])


if __name__ == "__main__":
    unittest.main()
