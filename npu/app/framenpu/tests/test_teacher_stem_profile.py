"""Native profile bounds, sampling coverage and non-overlapping work accounting."""

import ctypes
import importlib.util
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / n) for n in ("src", "tests", "tools")]
import teacher_stem_profile as P
import teacher_stem_layout as S
import teacher_tail_layout as T


@unittest.skipUnless(shutil.which("cc"), "host C compiler required")
class ProfileNativeTests(unittest.TestCase):
    def test_native_profile_bounds_match_mirror_and_forbid_aliases(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / "check.c").write_text(
                '#include <stdint.h>\ntypedef uint32_t fnpu_u32;\n#define RS_STEM_PROFILE\n#include "teacher_stem_check.h"\nint accepted(const fnpu_u32 *r) {return !ts_bad(r);}\n'
            )
            subprocess.run(
                [
                    "cc",
                    "-O2",
                    "-shared",
                    "-fPIC",
                    "-I",
                    str(ROOT / "tests"),
                    str(folder / "check.c"),
                    "-o",
                    str(folder / "check.so"),
                ],
                check=True,
                capture_output=True,
            )
            lib = ctypes.CDLL(str(folder / "check.so"))
            lib.accepted.argtypes = [ctypes.POINTER(ctypes.c_uint32)]

            def accepted(row):
                return bool(lib.accepted((ctypes.c_uint32 * 32)(*row)))

            row = [
                7,
                0,
                7,
                8,
                20,
                0x1A40000,
                0x1A89800,
                S.PARAMS,
                S.BLOCK,
                0x1B1C800,
                0x1B8D800,
                4,
                2356,
                5972,
                6880,
            ] + [0] * 17
            row[18:21] = [0x1C00000, 1, 0]
            self.assertTrue(accepted(row))
            rng = random.Random(0x54535031)
            for _ in range(4000):
                r = row.copy()
                i = rng.randrange(32)
                r[i] = rng.choice(
                    [0, 1, 2, 7, 8, 0x4000, 0xFFFFFFFF, (r[i] - 1) & 0xFFFFFFFF, r[i] + 1]
                )
                self.assertEqual(accepted(r), P.native_ok(r))
            for i in (5, 6, 9, 10):
                r = row.copy()
                r[18] = r[i]
                self.assertFalse(accepted(r))
            r = row.copy()
            r[19] = 2
            self.assertFalse(accepted(r))
            r[2] = 1
            self.assertTrue(accepted(r))
            r[19] = 3
            self.assertTrue(accepted(r))
            r[20] = 1
            self.assertFalse(accepted(r))
            r[20] = 8
            self.assertFalse(accepted(r))


class ProfileGeometryTests(unittest.TestCase):
    def test_halo_census_counts_actual_duplicates_and_stride2_waste(self):
        census = P.census()
        for name in S.NAMES[:4]:
            c = census[name]
            self.assertEqual(c["executed_blocks"], 504)
            self.assertEqual(c["executed_position_slots"], 16128)
            self.assertEqual(c["unique_valid_positions"], 112 * 112)
            self.assertEqual(c["repeated_valid_positions"], 24 * 112)
            self.assertEqual(c["invalid_padding_slots"], 896)
            self.assertEqual(
                c["valid_position_evaluations"] + c["invalid_padding_slots"],
                c["executed_position_slots"],
            )
            self.assertEqual(c["block_classes"], dict(core=392, halo=98, mixed=14))
        c = census[S.NAMES[4]]
        self.assertEqual(c["executed_blocks"], 252)
        self.assertEqual(c["required_output_positions"], 56 * 56)
        self.assertAlmostEqual(c["required_lane_fraction"], 7 / 18)
        self.assertEqual(c["executed_position_slots"], 8064)

    def test_log_vtcm_and_phase_coverage(self):
        self.assertGreaterEqual(P.VTCM, 13009)
        self.assertLessEqual(P.VTCM + P.VECTORS, T.VTCM_SPAN_VECTORS)
        for stage in range(5):
            coverage = [0] * 72
            for phase in range(8):
                for j in range(72):
                    if P.kept(stage, j) and j % 8 == phase:
                        coverage[j] += 1
            self.assertEqual(coverage, [int(P.kept(stage, j)) for j in range(72)])


@unittest.skipUnless(importlib.util.find_spec("numpy"), "NumPy required")
class ProfileLogTests(unittest.TestCase):
    def test_decode_requires_exact_coverage_and_accounting(self):
        import numpy as np

        row = [0] * 32
        row[1:3] = [0, 1]
        row[19:21] = [2, 0]
        log = np.zeros(4096, "<u4")
        log[:6] = [P.MAGIC, 2, 2, 0, 0, 1]
        log[6] = 10000
        log[7:11] = 10
        log[12:16] = [256, 256, 1, 1]
        b = log[64 : 64 + 448]
        for stage in range(5):
            p = b[80 * stage : 80 * stage + 80]
            p[:3] = [1, 1, 1000]
            for j in range(0, 72, 8):
                if P.kept(stage, j):
                    p[8 + j] = 10
                    p[7] += 10
        b[400:403] = [10, 10, 6000]
        P.decode(log.tobytes(), row, 1.0)
        bad = log.copy()
        bad[64 + 8 + 1] = 10
        with self.assertRaises(ValueError):
            P.decode(bad.tobytes(), row, 1.0)
        bad = log.copy()
        bad[64 + 7] += 1
        with self.assertRaises(ValueError):
            P.decode(bad.tobytes(), row, 1.0)
        bad = log.copy()
        bad[-1] = 1
        with self.assertRaises(ValueError):
            P.decode(bad.tobytes(), row, 1.0)


class ProfileBuildTests(unittest.TestCase):
    def test_profile_fits_and_uninstrumented_code_is_unchanged(self):
        try:
            from framenpu.compiler import Toolchain

            Toolchain.discover()
        except Exception as exc:
            self.skipTest(str(exc))
        import build_teacher_link as B

        with tempfile.TemporaryDirectory() as tmp:
            normal = B.build(Path(tmp) / "normal", stride2=True, heads=True, stem=True)
            prof = B.build(
                Path(tmp) / "profile", stride2=True, heads=True, stem=True, stem_profile=True
            )
            self.assertEqual(
                normal["linked"]["sha256"],
                "8c36451dd0e91b8122d867d66cbb6cdd419b312fe1125ab40fc9608c5cf327b9",
            )
            self.assertEqual(normal["loader"], prof["loader"])
            self.assertEqual(normal["reference"], prof["reference"])
            self.assertLessEqual(prof["linked"]["bytes"], 0x3F80)


if __name__ == "__main__":
    unittest.main()
