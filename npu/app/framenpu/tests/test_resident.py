"""Offline lifetime/placement and CLI checks for the proposed resident executor."""

import contextlib
from copy import deepcopy
import io
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu.cli import main
from framenpu.resident import ARENA_BYTES, ARENA_END, ARENA_START, load_resident_plan, plan_resident


def tensor(name, kind="activation", size=128, **fields):
    return dict(name=name, kind=kind, shape=[size], dtype="u8", layout="linear", **fields)


def schedule():
    return {
        "format": "framenpu-resident-plan",
        "version": 1,
        "name": "chain",
        "code_bytes": 128,
        "control_bytes": 0,
        "tensors": [
            tensor("x", "input"),
            tensor("w", "weight"),
            tensor("a"),
            tensor("b"),
            tensor("c"),
        ],
        "steps": [
            {"name": "first", "inputs": ["x", "w"], "outputs": ["a"]},
            {"name": "second", "inputs": ["a"], "outputs": ["b"]},
            {"name": "third", "inputs": ["b"], "outputs": ["c"]},
        ],
        "outputs": ["c"],
    }


def allocations(plan):
    return {entry["name"]: entry for entry in plan["allocations"]}


class ResidentPlannerTests(unittest.TestCase):
    def assert_safe_layout(self, plan):
        entries = plan["allocations"]
        for entry in entries:
            self.assertGreaterEqual(entry["offset"], ARENA_START)
            self.assertLessEqual(entry["offset"] + entry["reserved_bytes"], plan["arena_limit"])
            self.assertEqual(entry["offset"] % entry["alignment"], 0)
            self.assertEqual(entry["reserved_bytes"] % 128, 0)
            self.assertGreaterEqual(entry["reserved_bytes"], entry["bytes"])
        for index, left in enumerate(entries):
            for right in entries[index + 1 :]:
                time_overlap = max(left["lifetime"][0], right["lifetime"][0]) < min(
                    left["lifetime"][1], right["lifetime"][1]
                )
                byte_overlap = max(left["offset"], right["offset"]) < min(
                    left["offset"] + left["reserved_bytes"],
                    right["offset"] + right["reserved_bytes"],
                )
                self.assertFalse(time_overlap and byte_overlap, (left, right))
        peak = max(
            sum(
                entry["reserved_bytes"]
                for entry in entries
                if entry["lifetime"][0] <= event < entry["lifetime"][1]
            )
            for event in range(len(plan["steps"]) + 2)
        )
        self.assertEqual(plan["peak_live_bytes"], peak)
        self.assertEqual(
            plan["required_bytes"],
            max(e["offset"] + e["reserved_bytes"] for e in entries) - ARENA_START,
        )
        self.assertGreaterEqual(plan["required_bytes"], peak)
        self.assertEqual(plan["headroom_bytes"], plan["arena_bytes"] - plan["required_bytes"])

    def test_residual_example_preserves_skip_and_reports_real_storage_cost(self):
        plan = load_resident_plan(ROOT / "examples/resident_residual.json")
        self.assert_safe_layout(plan)
        records = allocations(plan)
        self.assertEqual(records["input"]["lifetime"], [0, 4])
        self.assertEqual(records["clipped"]["lifetime"], [4, 6])
        output = next(t for t in plan["tensors"] if t["name"] == "output")
        self.assertEqual(output["allocation"], "clipped")
        self.assertEqual(plan["required_bytes"], 187392)
        self.assertEqual(plan["without_reuse_bytes"], 297984)
        self.assertEqual(plan["arena_limit"], ARENA_END)
        self.assertFalse(plan["execution_supported"])

    def test_last_consumer_allows_reuse_only_in_a_later_step(self):
        plan = plan_resident(schedule())
        self.assert_safe_layout(plan)
        records = allocations(plan)
        self.assertEqual(records["x"]["offset"], records["b"]["offset"])
        self.assertEqual(records["a"]["offset"], records["c"]["offset"])
        self.assertEqual(records["a"]["lifetime"], [1, 3])
        self.assertNotEqual(records["a"]["offset"], records["b"]["offset"])

    def test_inputs_outputs_and_scratch_are_simultaneously_live(self):
        spec = schedule()
        spec["steps"][0]["scratch_bytes"] = 129
        plan = plan_resident(spec)
        self.assert_safe_layout(plan)
        live = [e for e in plan["allocations"] if e["lifetime"][0] <= 1 < e["lifetime"][1]]
        self.assertEqual({e["name"] for e in live}, {"@code", "x", "w", "a", "@scratch:first"})
        self.assertEqual(allocations(plan)["@scratch:first"]["reserved_bytes"], 256)

    def test_weights_code_and_control_remain_live_across_the_whole_run(self):
        spec = schedule()
        spec["control_bytes"] = 257
        records = allocations(plan_resident(spec))
        for name in ("@code", "@control", "w"):
            self.assertEqual(records[name]["lifetime"], [0, 5])
        self.assertEqual(records["@control"]["reserved_bytes"], 384)

    def test_alias_chain_extends_root_lifetime_even_with_forward_declarations(self):
        spec = schedule()
        spec["tensors"][:0] = [tensor("view2", alias_of="view1"), tensor("view1", alias_of="x")]
        spec["steps"][2]["inputs"].append("view2")
        plan = plan_resident(spec)
        self.assert_safe_layout(plan)
        self.assertEqual(allocations(plan)["x"]["lifetime"], [0, 4])
        views = [t for t in plan["tensors"] if t["alias_of"]]
        self.assertTrue(all(t["allocation"] == "x" for t in views))
        self.assertFalse({"view1", "view2"} & allocations(plan).keys())

    def test_alias_alignment_and_final_readback_are_applied_to_root(self):
        spec = schedule()
        spec["tensors"].append(tensor("view", alias_of="a", alignment=4096))
        spec["outputs"].append("view")
        plan = plan_resident(spec)
        self.assert_safe_layout(plan)
        root = allocations(plan)["a"]
        self.assertEqual(root["offset"] % 4096, 0)
        self.assertEqual(root["lifetime"], [1, 5])
        self.assertGreaterEqual(
            plan["without_reuse_bytes"], sum(a["reserved_bytes"] for a in plan["allocations"])
        )

    def test_whole_buffer_reshape_and_exact_padded_alias_are_supported(self):
        spec = schedule()
        spec["tensors"][0].update(shape=[8, 8], dtype="f16", layout="matrix")
        spec["tensors"].append(dict(tensor("flat", alias_of="x", size=64), dtype="f16"))
        spec["steps"][0]["inputs"][0] = "flat"
        plan_resident(spec)
        spec["tensors"][0]["strides"] = [20, 2]
        spec["tensors"][-1].update(shape=[8, 8], strides=[20, 2])
        plan = plan_resident(spec)
        self.assertEqual(next(t for t in plan["tensors"] if t["name"] == "flat")["bytes"], 156)

    def test_alias_cycles_unknown_targets_partial_views_and_type_punning_fail(self):
        for fields in (
            {"alias_of": "missing"},
            {"alias_of": "view"},
            {"alias_of": "x", "shape": [127]},
            {"alias_of": "x", "shape": [64], "dtype": "f16"},
            {"alias_of": "x", "kind": "weight"},
        ):
            spec = schedule()
            view = tensor("view")
            view.update(fields)
            spec["tensors"].append(view)
            with self.subTest(fields=fields), self.assertRaises(ValueError):
                plan_resident(spec)
        spec = schedule()
        spec["tensors"] += [tensor("v1", alias_of="v2"), tensor("v2", alias_of="v1")]
        with self.assertRaisesRegex(ValueError, "cycle"):
            plan_resident(spec)

    def test_reordered_padded_alias_is_rejected(self):
        spec = schedule()
        spec["tensors"][0].update(shape=[2, 3], strides=[8, 1])
        spec["tensors"].append(tensor("view", size=11, alias_of="x"))
        with self.assertRaisesRegex(ValueError, "whole-buffer"):
            plan_resident(spec)

    def test_reads_must_follow_producers_and_outputs_define_fresh_storage(self):
        for step, key, value in (
            (0, "inputs", ["a"]),
            (0, "inputs", ["missing"]),
            (1, "outputs", ["a"]),
            (0, "outputs", ["x"]),
            (0, "outputs", ["missing"]),
            (0, "outputs", []),
            (0, "outputs", ["a", "a"]),
        ):
            spec = schedule()
            spec["steps"][step][key] = value
            with self.subTest(step=step, key=key, value=value), self.assertRaises(ValueError):
                plan_resident(spec)
        spec = schedule()
        spec["tensors"].append(tensor("view", alias_of="x"))
        spec["steps"][0]["outputs"] = ["view"]
        with self.assertRaisesRegex(ValueError, "fresh"):
            plan_resident(spec)

    def test_unproduced_activations_unknown_outputs_and_duplicate_names_fail(self):
        bad_specs = []
        spec = schedule()
        spec["tensors"].append(tensor("unused"))
        bad_specs.append(spec)
        spec = schedule()
        spec["outputs"] = ["missing"]
        bad_specs.append(spec)
        spec = schedule()
        spec["tensors"].append(tensor("x"))
        bad_specs.append(spec)
        spec = schedule()
        spec["steps"][1]["name"] = "first"
        bad_specs.append(spec)
        for spec in bad_specs:
            with self.subTest(spec=spec), self.assertRaises(ValueError):
                plan_resident(spec)

    def test_invalid_format_fields_and_boolean_integer_values_fail(self):
        for value in (None, [], {}, "text"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                plan_resident(value)
        for key, value in (
            ("format", "other"),
            ("version", True),
            ("version", 2),
            ("code_bytes", True),
            ("code_bytes", 4097),
            ("control_bytes", -1),
            ("arena_bytes", ARENA_BYTES + 1),
            ("arena_bytes", 0),
            ("tensors", []),
            ("steps", []),
            ("outputs", []),
            ("name", "bad/name"),
            ("scratch_bytes", 128),
        ):
            spec = schedule()
            spec[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                plan_resident(spec)

    def test_invalid_shapes_types_strides_and_alignment_fail(self):
        for key, value in (
            ("shape", []),
            ("shape", [0]),
            ("shape", [True]),
            ("shape", [ARENA_BYTES, ARENA_BYTES]),
            ("shape", [1] * 9),
            ("dtype", "f64"),
            ("dtype", []),
            ("kind", "scratch"),
            ("strides", [-1]),
            ("strides", [True]),
            ("strides", [1, 1]),
            ("alignment", 64),
            ("alignment", 192),
            ("alignment", 8192),
            ("alignment", True),
            ("layout", ""),
            ("bytes", 128),
        ):
            spec = schedule()
            spec["tensors"][0][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                plan_resident(spec)
        for strides in ([2, 2], [7, 2]):
            spec = schedule()
            spec["tensors"][0].update(shape=[2, 4], dtype="f16", strides=strides)
            with self.subTest(strides=strides), self.assertRaises(ValueError):
                plan_resident(spec)

    def test_padded_strides_charge_full_address_span_and_cache_lines(self):
        spec = schedule()
        spec["tensors"][0].update(shape=[2, 3], dtype="f16", strides=[128, 2])
        plan = plan_resident(spec)
        self.assert_safe_layout(plan)
        record = allocations(plan)["x"]
        self.assertEqual(record["bytes"], 134)
        self.assertEqual(record["reserved_bytes"], 256)

    def test_exact_capacity_fits_and_overflow_includes_fixed_reservations(self):
        spec = schedule()
        spec["arena_bytes"] = 512
        plan = plan_resident(spec)
        self.assertEqual(plan["headroom_bytes"], 0)
        for change in ("arena", "scratch", "control", "code"):
            modified = deepcopy(spec)
            if change == "arena":
                modified["arena_bytes"] -= 1
            elif change == "scratch":
                modified["steps"][0]["scratch_bytes"] = 1
            elif change == "control":
                modified["control_bytes"] = 1
            else:
                modified["code_bytes"] += 1
            with (
                self.subTest(change=change),
                self.assertRaisesRegex(ValueError, "placement exceeds"),
            ):
                plan_resident(modified)

    def test_alias_count_limit_and_long_chains_are_bounded_without_recursion(self):
        spec = schedule()
        chain = [tensor(f"v{i}", alias_of=f"v{i + 1}" if i < 1018 else "x") for i in range(1019)]
        spec["tensors"] = chain + spec["tensors"]
        plan_resident(spec)
        spec["tensors"].append(tensor("extra", alias_of="x"))
        with self.assertRaisesRegex(ValueError, "1024"):
            plan_resident(spec)

    def test_plan_is_deterministic_and_does_not_mutate_the_schedule(self):
        spec = schedule()
        original = deepcopy(spec)
        self.assertEqual(plan_resident(spec), plan_resident(deepcopy(spec)))
        self.assertEqual(spec, original)

    def test_random_schedules_preserve_all_live_ranges_and_bounds(self):
        rng = random.Random(64827)
        for _ in range(80):
            spec = schedule()
            spec["tensors"] = [
                tensor("x", "input", rng.randint(1, 1000)),
                tensor("w", "weight", rng.randint(1, 1000)),
            ]
            spec["steps"] = []
            available = ["x"]
            for index in range(rng.randint(2, 20)):
                name = f"t{index}"
                inputs = rng.sample(available, rng.randint(1, min(3, len(available)))) + ["w"]
                spec["tensors"].append(
                    tensor(
                        name, size=rng.randint(1, 1000), alignment=rng.choice([128, 256, 512, 4096])
                    )
                )
                spec["steps"].append(
                    {
                        "name": f"s{index}",
                        "inputs": inputs,
                        "outputs": [name],
                        "scratch_bytes": rng.randint(0, 1000),
                    }
                )
                available.append(name)
            spec["outputs"] = [available[-1], available[-2]]
            plan = plan_resident(spec)
            self.assert_safe_layout(plan)
            records = allocations(plan)
            for step in plan["steps"]:
                for name in step["inputs"] + step["outputs"]:
                    self.assertLessEqual(records[name]["lifetime"][0], step["index"])
                    self.assertGreater(records[name]["lifetime"][1], step["index"])


class ResidentCLITests(unittest.TestCase):
    def test_loading_rejects_invalid_duplicate_deep_and_oversized_json(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "schedule.json"
            for raw in (
                b"{",
                b'{"version":1,"version":2}',
                b"[" * 2000 + b"]" * 2000,
                b" " * (2 * 1024 * 1024 + 1),
            ):
                path.write_bytes(raw)
                with self.subTest(length=len(raw)), self.assertRaises(ValueError):
                    load_resident_plan(path)

    def test_cli_plans_without_constructing_a_device(self):
        output = io.StringIO()
        with (
            patch("framenpu.cli.Device", side_effect=AssertionError("device opened")),
            contextlib.redirect_stdout(output),
        ):
            main(["plan-resident", str(ROOT / "examples/resident_residual.json")])
        self.assertEqual(json.loads(output.getvalue())["required_bytes"], 187392)

    def test_cli_from_unrelated_directory_and_malformed_input_error(self):
        env = dict(os.environ, PYTHONPATH=str(ROOT / "src"), PYTHONDONTWRITEBYTECODE="1")
        with tempfile.TemporaryDirectory() as directory:
            for path, expected in (
                (ROOT / "examples/resident_residual.json", 0),
                (Path(directory) / "missing.json", 2),
            ):
                result = subprocess.run(
                    [sys.executable, "-B", "-m", "framenpu", "plan-resident", str(path)],
                    cwd=directory,
                    env=env,
                    text=True,
                    capture_output=True,
                )
                self.assertEqual(result.returncode, expected, result.stderr)
                if expected:
                    self.assertIn("framenpu:", result.stderr)
                    self.assertNotIn("Traceback", result.stderr)
                else:
                    self.assertFalse(json.loads(result.stdout)["execution_supported"])


if __name__ == "__main__":
    unittest.main()
