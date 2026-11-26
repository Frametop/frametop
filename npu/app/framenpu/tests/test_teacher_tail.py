"""Offline checks for the FP16 teacher-tail run (no device). Needs the public
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
class TeacherTailTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tools")]
        from framenpu.convolution import load_landmark_graph
        from framenpu.landmark import specifications

        cls.graph = load_landmark_graph(MODEL / "hand.ncnn.param", MODEL / "hand.ncnn.bin")
        cls.specs = specifications(cls.graph)

    def test_kernel_models_match_math_model_on_every_tail_op(self):
        import numpy as np
        import teacher_tail_layout as T

        by = {s["name"]: s for s in self.specs}
        ops, _ = T.tail_ops(self.specs)
        sh = T.shapes(self.specs)
        where, stage = T.allocate(ops, sh, ["8", "10"])
        rng = np.random.default_rng(3)
        t = {
            "8": rng.uniform(0, 3, sh["8"]).astype(np.float16).astype(np.float64),
            "10": rng.uniform(-1, 2, sh["10"]).astype(np.float16).astype(np.float64),
        }
        arena = {}
        for b in ("8", "10"):
            arena[where[b][0]] = T.grid_of(sh[b]).to_bytes(t[b].astype(np.float16))
        for op in ops:
            s = by[op.name]
            gin, gout = T.grid_of(sh[op.src]), T.grid_of(sh[op.dst])
            arena[where[op.dst][0]] = bytes(gout.tensor_bytes(sh[op.dst][0]))
            if op.kind == "pw":
                tiles, bias, nk, no = T.pw_weights(self.graph, s)
                staged = [tiles, bias, gin.mask_bytes()[: gin.nj * 128]]
                desc = T.pw_descriptor(
                    s,
                    gin,
                    nk,
                    no,
                    where[op.src][0],
                    stage,
                    [len(staged[0]), len(staged[1])],
                    where[op.res][0] if op.res else 0,
                    where[op.dst][0],
                )
                model, ref = (
                    T.model_pw,
                    T.emulate_pw(self.graph, s, t[op.src], t[op.res] if op.res else None),
                )
            else:
                coef, bias = T.dw_weights(self.graph, s)
                staged = [coef, bias, gin.mask_bytes()]
                desc = T.dw_descriptor(
                    s, gin, gout, where[op.src][0], stage, [len(coef), len(bias)], where[op.dst][0]
                )
                model, ref = T.model_dw, T.emulate_dw(self.graph, s, t[op.src])
            for key in [k for k in arena if k >= stage]:
                del arena[key]  # staging is rewritten for every op
            off = stage
            for blob in staged:
                arena[off] = blob
                off += len(blob)

            def mem(o, n):
                for base, data in arena.items():
                    if base <= o and o + n <= base + len(data):
                        return data[o - base : o - base + n]
                raise KeyError(hex(o))

            written = model(desc, mem)
            raw = written[where[op.dst][0]]
            got, stray = gout.from_bytes(raw, sh[op.dst][0])
            with self.subTest(op.name):
                self.assertEqual(stray, 0)
                np.testing.assert_allclose(
                    got, ref.astype(np.float16).astype(np.float64), atol=2e-3, rtol=2e-3
                )
            arena[where[op.dst][0]] = raw
            t[op.dst] = got

    def test_plan_fits_window_and_vtcm(self):
        import teacher_tail_layout as T

        ops, _ = T.tail_ops(self.specs)
        sh = T.shapes(self.specs)
        where, stage = T.allocate(ops, sh, ["8", "10"])
        by = {s["name"]: s for s in self.specs}
        biggest = 0
        for op in ops:
            if op.kind == "pw":
                tiles, bias, _, _ = T.pw_weights(self.graph, by[op.name])
                biggest = max(biggest, len(tiles) + len(bias) + T.grid_of(sh[op.src]).nj4 * 128)
        self.assertLessEqual(stage + biggest, T.WIN_HI)
        al = lambda v: (v + 31) & ~31  # noqa: E731  (mirrors pw_get / td_get)
        for op in ops:
            s, gi, go = by[op.name], T.grid_of(sh[op.src]), T.grid_of(sh[op.dst])
            if op.kind == "pw":
                cin, cout = s["input_shape"][0], s["output_shape"][0]
                nk, no = -(-cin // 32), -(-cout // 32)
                bias = al(al(no * nk * 16) + nk * 16)
                end = (
                    bias
                    + (no + 1) // 2
                    + gi.nj
                    + cout // 2 * gi.nj
                    + cin // 2 * gi.nbin
                    + (cout // 2 * gi.nbin if op.res else 0)
                )
            else:
                cp, kk = s["input_shape"][0] // 2, s["kernel"][0] ** 2
                end = (
                    cp * gi.nj4
                    + cp * kk
                    + (cp * kk + 31) // 32
                    + (cp + 31) // 32
                    + gi.nj4
                    + (cp * go.nbin if s["stride"] == (2, 2) else 0)
                    + cp * gi.nbin
                )
            with self.subTest(op.name):
                self.assertLessEqual(end, T.VTCM_SPAN_VECTORS)


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
                [sys.executable, str(ROOT / "tools/build_teacher_tail.py"), "--output", tmp],
                check=True,
                capture_output=True,
            )
            for k in ("pw", "dw"):
                self.assertLessEqual(
                    json.loads((Path(tmp) / k / "build.json").read_text())["bytes"], 4096
                )


if __name__ == "__main__":
    unittest.main()
