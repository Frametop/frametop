"""Offline checks for the HVX FP16 depthwise probes and issue-rate probe (no device)."""

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from hvx_dw_layout import (
    TEACHER_STRIDE1,
    VTCM_SPAN_VECTORS,
    WIN_HI,
    WIN_LO,
    arena_image,
    fixture,
    oracle,
    emulate,
    plan,
    v2_arena_image,
    v2_decode,
    v2_emulate,
    v2_plan,
    v2_shape,
    v2_vtcm_vectors,
)

ROOT = Path(__file__).resolve().parents[1]
NUMPY = importlib.util.find_spec("numpy") is not None


class DepthwiseLayoutTests(unittest.TestCase):
    def test_every_teacher_layer_fits_window_and_vtcm(self):
        for layer in TEACHER_STRIDE1:
            with self.subTest(layer.name):
                self.assertLessEqual(plan(layer)["end"], WIN_HI)
                self.assertLessEqual(v2_plan(layer)["end"], WIN_HI)
                self.assertLessEqual(layer.vtcm_vectors(), VTCM_SPAN_VECTORS)
                self.assertLessEqual(v2_vtcm_vectors(layer), VTCM_SPAN_VECTORS)
                self.assertEqual(layer.c % 8, 0)
                ws, nj, nbin = v2_shape(layer)
                self.assertEqual(nj % 4, 0)
                self.assertGreater(nbin * 32, nj * 32 + (layer.k - 1) * (ws + 1) + 31)

    @unittest.skipUnless(NUMPY, "numpy")
    def test_kernel_addressing_models_match_oracle(self):
        import numpy as np

        for layer in TEACHER_STRIDE1:
            with self.subTest(layer.name):
                x, wt, b = fixture(layer)
                ref, allow = oracle(layer, x, wt, b)
                self.assertTrue(np.all(allow > 0))
                np.testing.assert_allclose(emulate(layer, x, wt, b), ref, atol=1e-9)
                np.testing.assert_allclose(v2_emulate(layer, x, wt, b), ref, atol=1e-9)

    @unittest.skipUnless(NUMPY, "numpy")
    def test_arena_images_cover_plan_and_masked_lanes_decode_as_zero(self):
        for layer in TEACHER_STRIDE1[:3]:
            x, wt, b = fixture(layer)
            for image in (arena_image, v2_arena_image):
                p, writes = image(layer, x, wt, b)
                for off, data in writes.items():
                    self.assertTrue(WIN_LO <= off and off + len(data) <= p["end"])
            p, writes = v2_arena_image(layer, x, wt, b)
            out, stray = v2_decode(layer, writes[p["out"][0]])
            self.assertEqual(stray, 0)
            self.assertEqual(out.shape, (layer.c, layer.h, layer.w))

    def test_probes_build_within_4_kib_without_vector_stack(self):
        try:
            sys.path.insert(0, str(ROOT / "src"))
            from framenpu.compiler import Toolchain

            Toolchain.discover()
        except Exception as exc:  # pragma: no cover - environment dependent
            self.skipTest(f"no Hexagon toolchain: {exc}")
        for tool in ("build_hvx_dw_probe.py", "build_hvx_dw2_probe.py", "build_hvx_rate_probe.py"):
            with self.subTest(tool), tempfile.TemporaryDirectory() as tmp:
                subprocess.run(
                    [sys.executable, str(ROOT / "tools" / tool), "--output", tmp],
                    check=True,
                    capture_output=True,
                )
                self.assertLessEqual(
                    json.loads((Path(tmp) / "build.json").read_text())["bytes"], 4096
                )


if __name__ == "__main__":
    unittest.main()
