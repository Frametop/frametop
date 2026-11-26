"""Offline checks for the fused HMX spatial-block prototype (no device)."""

import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from hmx_block_layout import (
    ARENA_BYTES,
    GEOMETRIES,
    TAPS,
    band,
    emulate_gather,
    im2col,
    oracle,
    payload,
    planar,
    synthetic_fixture,
    unpack,
    unplanar,
    weight_tiles,
)

ROOT = Path(__file__).resolve().parents[1]
NUMPY = importlib.util.find_spec("numpy") is not None


class GeometryTests(unittest.TestCase):
    def test_every_job_fits_the_arena_and_the_guards_cover_every_tap(self):
        for g in GEOMETRIES:
            with self.subTest(g.name):
                self.assertLessEqual(((g.input_bytes + 127) & ~127) + g.output_bytes, ARENA_BYTES)
                # Highest block any tap of the last output block touches.
                last = g.nj - 1 + (2 * g.width + 33) // 32 + 1
                self.assertLess(last, g.nbin)
                self.assertGreaterEqual(g.nbin * 32, 32 + (g.rows + 2) * g.width)

    def test_teacher_control_matches_public_layer_shapes(self):
        t = next(g for g in GEOMETRIES if g.name == "teacher_112")
        self.assertEqual((t.cin, t.cmid, t.cout, t.nk1, t.nm), (24, 24, 16, 7, 1))

    def test_counts_used_for_forecasts(self):
        g = next(g for g in GEOMETRIES if g.name == "w050_14")
        self.assertEqual((g.nk1, g.nm, g.no, g.nj), (7, 5, 1, 7))
        self.assertEqual(g.k_tiles_per_block, 5 * 7 + 5)


@unittest.skipUnless(NUMPY, "optional NumPy not installed")
class LayoutTests(unittest.TestCase):
    def test_device_gather_emulation_equals_im2col_for_every_tile(self):
        import numpy as np

        for g in GEOMETRIES:
            with self.subTest(g.name):
                x = synthetic_fixture(g)[0]
                src = planar(x.reshape(g.cin, -1), g.nbin, 1)
                for j in range(g.nj):
                    ref = im2col(g, x.astype(np.float64), j)
                    for t in range(g.nk1):
                        tile = emulate_gather(g, src, j, t)
                        got = unpack(tile.tobytes())
                        np.testing.assert_array_equal(got, ref[32 * t : 32 * t + 32])

    def test_resident_table_pack_equals_im2col_for_every_tile(self):
        """Model of hmx_resident_block_probe.c fr_table/fr_pack (VTCM band)."""
        import numpy as np

        for g in GEOMETRIES:
            with self.subTest(g.name):
                x = synthetic_fixture(g)[0]
                flat = planar(x.reshape(g.cin, -1), g.nbin, 1).view("<u4").reshape(-1)
                table = []
                for r in range(g.nk1 * 16):
                    tap, ch = divmod(2 * r, g.cin)
                    if tap >= TAPS:
                        table.append(3 << 24)
                        continue
                    s = (tap // 3) * g.width + tap % 3 + 31
                    table.append((ch // 2) * g.nbin + s // 32 | (s % 32) * 4 << 16 | tap % 3 << 24)
                for j in range(g.nj):
                    xs = (32 * j + np.arange(32)) % g.width
                    bt = np.zeros((g.nk1 * 16, 32), "<u4")
                    for r, word in enumerate(table):
                        kind, off, shift = word >> 24, word & 0xFFFF, (word >> 16) & 0xFF
                        if kind == 3:
                            continue
                        self.assertLess(off + j + 1, flat.size // 32)
                        v = flat[(off + j) * 32 + shift // 4 :][:32].copy()
                        if kind == 0:
                            v[xs == 0] = 0
                        elif kind == 2:
                            v[xs == g.width - 1] = 0
                        bt[r] = v
                    ref = im2col(g, x.astype(np.float64), j)
                    for t in range(g.nk1):
                        got = unpack(bt[16 * t : 16 * t + 16].tobytes())
                        np.testing.assert_array_equal(got, ref[32 * t : 32 * t + 32])

    def test_top_band_reads_zero_padding_rows(self):
        import numpy as np

        g = next(g for g in GEOMETRIES if g.name == "teacher_112")
        image = np.ones((g.cin, 112, 112), dtype=np.float16)
        top = band(image, 0, g.rows)
        self.assertTrue(np.all(top[:, 0] == 0) and np.all(top[:, 1:] == 1))
        bottom = band(image, 112 - g.rows, g.rows)
        self.assertTrue(np.all(bottom[:, -1] == 0))

    def test_full_block_through_packed_tiles_matches_oracle(self):
        """Emulate the whole data path (gather, tile order, HMX product, half
        stores, QFloat-free bias/clip) and check the oracle bounds hold."""
        import numpy as np

        h = lambda v: v.astype(np.float16).astype(np.float64)  # noqa: E731
        for g in GEOMETRIES:
            with self.subTest(g.name):
                x, w1, w2, b1, b2 = synthetic_fixture(g)
                src = planar(x.reshape(g.cin, -1), g.nbin, 1)
                a1, a2 = weight_tiles(g, w1, w2)
                b1p = np.zeros(g.nm * 32)
                b1p[: g.cmid] = h(np.asarray(b1, np.float64))
                b2p = np.zeros(g.no * 32)
                b2p[: g.cout] = h(np.asarray(b2, np.float64))
                out = np.zeros((g.cout, g.nj * 32))
                mids = np.zeros((g.nm * 32, g.nj * 32))
                for j in range(g.nj):
                    for m in range(g.nm):
                        acc = sum(
                            unpack(a1[m * g.nk1 + t].tobytes())
                            @ unpack(emulate_gather(g, src, j, t).tobytes())
                            for t in range(g.nk1)
                        )
                        mid = np.clip(h(h(acc) + b1p[32 * m : 32 * m + 32, None]), 0, 6)
                        mids[32 * m : 32 * m + 32, 32 * j : 32 * j + 32] = mid
                    for o in range(g.no):
                        acc = sum(
                            unpack(a2[o * g.nm + m].tobytes())
                            @ mids[32 * m : 32 * m + 32, 32 * j : 32 * j + 32]
                            for m in range(g.nm)
                        )
                        rows = min(32, g.cout - 32 * o)
                        res = h(h(acc) + b2p[32 * o : 32 * o + 32, None])
                        out[32 * o : 32 * o + rows, 32 * j : 32 * j + 32] = res[:rows]
                ref = oracle(g, x, w1, w2, b1, b2)
                err = np.abs(out[:, : g.nout] - ref["out"])
                self.assertTrue(np.all(err <= ref["out_allow"]))
                merr = np.abs(mids[: g.cmid, : g.nout] - ref["mid"])
                self.assertTrue(np.all(merr <= ref["mid_allow"]))

    def test_payload_layout_and_planar_roundtrip(self):
        import numpy as np

        for g in GEOMETRIES:
            with self.subTest(g.name):
                fx = synthetic_fixture(g)
                raw = payload(g, *fx)
                self.assertEqual(len(raw), g.input_bytes)
                words = np.frombuffer(raw[:128], "<u4")
                self.assertEqual(words[1], g.gid)
                xt = np.frombuffer(raw[g.xt_offset : g.src_offset], "<u4")
                self.assertTrue(np.all(xt & 0xFFFF == (xt >> 16) % g.width))
                src = raw[g.src_offset : g.w1_offset]
                back = unplanar(src, g.cin, g.nbin)[:, 32 : 32 + (g.rows + 2) * g.width]
                np.testing.assert_array_equal(back, fx[0].reshape(g.cin, -1).astype(np.float64))

    def test_tap_padding_rows_are_zero_in_weight_tiles(self):
        import numpy as np

        g = next(g for g in GEOMETRIES if g.name == "w050_14")  # 216 of 224 K rows
        _, w1, w2, _, _ = synthetic_fixture(g)
        a1, _ = weight_tiles(g, w1, w2)
        last = unpack(a1[g.nk1 - 1].tobytes())
        self.assertTrue(np.all(last[:, TAPS * g.cin - 32 * (g.nk1 - 1) :] == 0))


def fabricated_report():
    """Measurement-shaped report with known per-unit costs (ticks)."""
    cases = []
    for g in GEOMETRIES:
        samples = []
        variants = ["main", "copy1", "notime"] + (["noreuse"] if g.nm * g.no == 1 else [])
        for v in variants:
            for reps in (1, 4):
                blocks = reps * g.nj
                phases = {
                    "weights": 19.2
                    * 2
                    * blocks
                    * (g.nm * g.nk1 + (0 if g.nm * g.no == 1 else g.nm * g.no)),
                    "stage": 19.2 * 1 * blocks * g.nm * g.nk1,
                    "pack": 19.2 * 0.25 * blocks * g.nm * g.nk1,
                    "hmx": 19.2 * 0.5 * blocks * g.k_tiles_per_block,
                    "post": 19.2 * blocks * g.nm,
                    "spill": 19.2 * 2 * blocks * (g.nm + g.nm * g.no) if g.nm > 1 else 0,
                    "store": 19.2 * blocks * g.no,
                    "setup": 19.2 * 0.1 * blocks,
                }
                if v == "notime":
                    phases = dict.fromkeys(phases, 0)
                body = 10 + sum(phases.values()) if v != "notime" else 10 + 0.98 * blocks * 1000
                for round_ in range(2):
                    samples.append(
                        {
                            "variant": v,
                            "reps": reps,
                            "round": round_,
                            "body_ticks": body,
                            "phase_ticks": phases,
                        }
                    )
        cases.append(
            {
                "geometry": g.name,
                "fixture": "synthetic",
                "repeat_counts": [1, 4],
                "samples": samples,
            }
        )
    return {"all_checks_passed": True, "cases": cases}


class AnalyzerTests(unittest.TestCase):
    def test_unit_costs_are_recovered_from_phase_partition(self):
        sys.path.insert(0, str(ROOT / "tools"))
        from analyze_hmx_block import summarize

        summary = summarize(fabricated_report())
        for key, item in summary.items():
            u = item["units"]
            self.assertAlmostEqual(u["weight_tile_us"], 2)
            self.assertAlmostEqual(u["stage_k_tile_us"], 1)
            self.assertAlmostEqual(u["pack_k_tile_us"], 0.25)
            self.assertAlmostEqual(u["hmx_per_load_us"], 0.5)
            if key.startswith("w050_14"):
                self.assertAlmostEqual(u["spill_tile_us"], 2)
            self.assertAlmostEqual(sum(item["phase_share"].values()), 1)

    def test_failed_measurement_is_refused(self):
        sys.path.insert(0, str(ROOT / "tools"))
        from analyze_hmx_block import summarize

        with self.assertRaises(ValueError):
            summarize({"all_checks_passed": False, "cases": []})


@unittest.skipUnless(shutil.which("clang") and shutil.which("ld.lld"), "LLVM is not installed")
class BuildTests(unittest.TestCase):
    def test_all_variants_fit_payload_without_vector_stack_use(self):
        with tempfile.TemporaryDirectory() as folder:
            subprocess.run(
                [sys.executable, str(ROOT / "tools/build_hmx_block_probe.py"), "--output", folder],
                check=True,
                capture_output=True,
                cwd=ROOT,
            )
            records = [json.loads(p.read_text()) for p in Path(folder).glob("*/*/build.json")]
            self.assertEqual(len(records), 7 * len(GEOMETRIES) + 2)
            for r in records:
                self.assertLessEqual(r["bytes"], 4096)
                self.assertEqual(r["vector_stack_accesses"], 0)
                g = next(g for g in GEOMETRIES if g.name == r["geometry"])
                self.assertEqual(r["input_bytes"], g.input_bytes)
                self.assertEqual(r["macros"]["FB_NBIN"], g.nbin)


if __name__ == "__main__":
    unittest.main()
