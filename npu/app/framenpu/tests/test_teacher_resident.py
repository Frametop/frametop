"""Offline checks for the resident teacher tail (no device). Needs the public
v20a model files; set FNPU_TEACHER_MODEL to their directory."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
MODEL = Path(os.environ.get("FNPU_TEACHER_MODEL", "/nonexistent"))
NUMPY = importlib.util.find_spec("numpy") is not None
HAVE_MODEL = (MODEL / "hand.ncnn.param").exists()


@unittest.skipUnless(NUMPY and HAVE_MODEL, "numpy and FNPU_TEACHER_MODEL required")
class ResidentPlanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tools")]
        from framenpu.convolution import load_landmark_graph
        from framenpu.landmark import specifications
        import teacher_resident_layout as RL

        cls.graph = load_landmark_graph(MODEL / "hand.ncnn.param", MODEL / "hand.ncnn.bin")
        cls.specs = specifications(cls.graph)
        cls.plans = {
            m: RL.build_plan(cls.graph, cls.specs, m)
            for m in ("resident", "qualify", "merged", "merged-observe")
        }
        cls.plans["merged-split"] = RL.build_plan(
            cls.graph, cls.specs, "merged", split_after="convdwclip_5"
        )

    def test_plans_pass_the_native_checks_and_fit(self):
        import numpy as np
        import teacher_resident_layout as RL
        import teacher_tail_layout as T

        for mode, plan in self.plans.items():
            rows = np.frombuffer(plan.table, "<u4").reshape(-1, 32)
            with self.subTest(mode):
                self.assertTrue(all(RL.native_ok(r) for r in rows))
                self.assertLessEqual(plan.end, T.WIN_HI)
                self.assertTrue(all(j.end - j.first <= 160 for j in plan.jobs))
                self.assertLessEqual(max(j.vtcm_peak for j in plan.jobs), RL.SPAN)
        self.assertEqual(
            [j.kind for j in self.plans["resident"].jobs],
            ["rs", "s2", "rs", "s2", "rs", "s2", "rs"],
        )
        self.assertEqual(len(self.plans["merged"].jobs), 1)
        self.assertEqual(len(self.plans["merged"].table) // 128, 45)
        self.assertEqual(set(self.plans["merged"].where), {"8", "10", RL.FINAL})
        self.assertEqual(len(self.plans["merged-split"].jobs), 2)
        bad = rows[[i for i, r in enumerate(rows) if r[0] == 1][0]].copy()
        bad[9] = T.WIN_HI  # the hardware rejection control
        self.assertFalse(RL.native_ok(bad))
        bad = rows[[i for i, r in enumerate(rows) if r[0] == 1][0]].copy()
        bad[14] += 1  # misaligned HMX tiles
        self.assertFalse(RL.native_ok(bad))

    def test_simulated_kernel_matches_contract_model_and_modes_agree(self):
        import numpy as np
        import teacher_resident_layout as RL
        import teacher_tail_layout as T

        by = {s["name"]: s for s in self.specs}
        sh = T.shapes(self.specs)
        rng = np.random.default_rng(5)
        t = {"8": rng.uniform(0, 3, sh["8"]), "10": rng.uniform(-1, 2, sh["10"])}
        t = {k: v.astype(np.float16).astype(np.float64) for k, v in t.items()}
        for op in T.tail_ops(self.specs)[0]:
            s = by[op.name]
            t[op.dst] = (
                T.emulate_pw(self.graph, s, t[op.src], t[op.res] if op.res else None)
                if op.kind == "pw"
                else T.emulate_dw(self.graph, s, t[op.src])
            )
        finals = {}
        for mode, plan in self.plans.items():
            win = bytearray(T.WIN_HI - T.WIN_LO)
            for off, data in RL.setup_writes(plan).items():
                win[off - T.WIN_LO : off - T.WIN_LO + len(data)] = data
            for b in RL.INPUTS:
                data = T.grid_of(sh[b]).to_bytes(t[b].astype(np.float16))
                win[plan.where[b] - T.WIN_LO : plan.where[b] - T.WIN_LO + len(data)] = data
            RL.simulate(plan, win)
            for b in {b for job in plan.jobs for b in job.stores}:
                g = T.grid_of(sh[b])
                at = plan.where[b] - T.WIN_LO
                data, padding = g.from_bytes(
                    bytes(win[at : at + g.tensor_bytes(sh[b][0])]), sh[b][0]
                )
                # Stored slots in the per-op reference are reused later.
                if mode.startswith("merged"):
                    with self.subTest(mode=mode, tensor=b):
                        self.assertEqual(padding, 0)
                        np.testing.assert_array_equal(data, t[b])
            g = T.grid_of(sh[RL.FINAL])
            o = plan.where[RL.FINAL] - T.WIN_LO
            got, stray = g.from_bytes(
                bytes(win[o : o + g.tensor_bytes(sh[RL.FINAL][0])]), sh[RL.FINAL][0]
            )
            with self.subTest(mode):
                self.assertEqual(stray, 0)
                self.assertFalse(np.isnan(got).any())
                np.testing.assert_array_equal(got, t[RL.FINAL])
            finals[mode] = got
        np.testing.assert_array_equal(finals["resident"], finals["qualify"])

    def test_invalid_splits_are_rejected(self):
        import teacher_resident_layout as RL

        for mode, name in (
            ("resident", "convdwclip_5"),
            ("merged", "missing"),
            ("merged", "convdwclip_15"),
        ):
            with self.subTest(mode=mode, name=name), self.assertRaises(ValueError):
                RL.build_plan(self.graph, self.specs, mode, split_after=name)

    def test_link_harness_switches_shared_window_plans(self):
        from dataclasses import replace
        import numpy as np
        import hardware_teacher_link as H
        import teacher_resident_layout as R
        import teacher_tail_layout as T

        class SimulatedRunner(H.LinkRunner):
            def submit(self, plan, job, **kwargs):
                win = bytearray(self.mem[T.WIN_LO : T.WIN_HI])
                R.simulate(replace(plan, jobs=[job]), win)
                self.mem[T.WIN_LO : T.WIN_HI] = win
                return {"kind": job.kind, "wall_ms": 1, "wrapper_ms": 1, "body_ms": 1}

        rng = np.random.default_rng(71)
        inputs = {b: rng.uniform(-1, 2, self.plans["merged"].shape_of[b]) for b in R.INPUTS}
        for split in (None, "convdwclip_5"):
            runner = SimulatedRunner(
                None,
                bytearray(T.WIN_HI),
                {},
                self.graph,
                self.specs,
                None,
                b"",
                stride2=True,
                split_after=split,
            )
            _, _, control, _ = runner.frame(inputs.__getitem__, observe=False)
            _, checkpoints, observed, _ = runner.frame(
                inputs.__getitem__, observe=True, linked=True, checkpoints=True
            )
            self.assertEqual(set(checkpoints), {"19", "28", "58", R.FINAL})
            self.assertEqual(observed, control)
            for linked in (True, False, True):
                _, _, final, _ = runner.frame(inputs.__getitem__, observe=False, linked=linked)
                self.assertEqual(final, control)

    def test_simulator_reads_the_table_from_the_window(self):
        import teacher_resident_layout as RL
        import teacher_tail_layout as T

        plan = self.plans["qualify"]
        win = bytearray(T.WIN_HI - T.WIN_LO)  # no setup writes: every descriptor reads as kind 0
        import numpy as np

        rows = np.frombuffer(bytes(win[: len(plan.table)]), "<u4").reshape(-1, 32)
        self.assertFalse(any(RL.native_ok(r) for r in rows))


class BuildTests(unittest.TestCase):
    def test_kernels_build_within_4_kib(self):
        try:
            sys.path.insert(0, str(ROOT / "src"))
            from framenpu.compiler import Toolchain

            Toolchain.discover()
        except Exception as exc:  # pragma: no cover
            self.skipTest(f"no Hexagon toolchain: {exc}")
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(
                [sys.executable, str(ROOT / "tools/build_teacher_resident.py"), "--output", tmp],
                check=True,
                capture_output=True,
            )
            for k in ("rs", "dw"):
                self.assertLessEqual(
                    json.loads((Path(tmp) / k / "build.json").read_text())["bytes"], 4096
                )


if __name__ == "__main__":
    unittest.main()
