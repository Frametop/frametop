"""Banded prefix: native bounds, finite half pooling, seams and owned memory."""

import ctypes
import importlib.util
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / n) for n in ("src", "tests", "tools")]
import teacher_stem_layout as S
import teacher_tail_layout as T

MODEL = Path(os.environ.get("FNPU_TEACHER_MODEL", "/nonexistent"))


@unittest.skipUnless(shutil.which("cc"), "host C compiler required")
class StemNativeChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        folder = Path(cls.tmp.name)
        source = (ROOT / "tests/teacher_stem_impl.h").read_text()
        scalar = source[
            source.index("static inline fnpu_u32 ts_hmax") : source.index(
                "static __attribute__((noinline)) void ts_skip"
            )
        ]
        code = '#include <stdint.h>\ntypedef uint32_t fnpu_u32;\n#include "teacher_stem_check.h"\n'
        code += scalar + "\nint accepted(const fnpu_u32 *r) { return !ts_bad(r); }\n"
        code += "fnpu_u32 maxpair(fnpu_u32 a, fnpu_u32 b) { return ts_maxpair(a,b); }\n"
        code += "fnpu_u32 needed(fnpu_u32 j) { return ts_needed_block(j); }\n"
        (folder / "check.c").write_text(code)
        subprocess.run(
            [
                shutil.which("cc"),
                "-shared",
                "-fPIC",
                "-O2",
                "-I",
                str(ROOT / "tests"),
                str(folder / "check.c"),
                "-o",
                str(folder / "check.so"),
            ],
            check=True,
            capture_output=True,
        )
        cls.lib = ctypes.CDLL(str(folder / "check.so"))
        cls.lib.accepted.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
        cls.lib.accepted.restype = ctypes.c_int
        cls.lib.maxpair.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
        cls.lib.maxpair.restype = ctypes.c_uint32
        cls.row = [
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

    def test_actual_vector_packer_matches_scalar_gather_for_every_tile(self):
        folder = Path(self.tmp.name)
        source = (ROOT / "tests/teacher_stem_impl.h").read_text()
        packer = source[
            source.index("typedef long ts_uv2") : source.index(
                "static __attribute__((noinline)) void ts_stem"
            )
        ]
        code = (
            '#include "teacher_stem_pack_host.h"\n'
            + packer
            + r"""
int main(void) {
    fnpu_u16 *rgb=(fnpu_u16 *)FR(TS_RGB);
    for (unsigned i=0;i<150528;i++) rgb[i]=(i*31u+i/37u)&65535u;
    rs_op d={.scr=FR(12688)};
    for (unsigned band=0;band<7;band++) for (unsigned j=0;j<72;j++) {
        ts_rgb_tile(&d,band,j);
        const fnpu_u16 *b=(const fnpu_u16 *)d.scr;
        for (unsigned k=0;k<32;k++) for (unsigned lane=0;lane<32;lane++) {
            unsigned pos=32*j+lane;
            int y=16*(int)band-1+pos/114, x=(int)(pos%114)-2;
            unsigned want=0;
            if (k<27 && y>=0 && y<112) {
                int iy=2*y+(k%9)/3, ix=2*x+k%3;
                if (iy<224 && ix>=0 && ix<224) want=rgb[(k/9)*50176+iy*224+ix];
            }
            if (b[(k/2)*64+2*lane+k%2]!=want) return 1;
        }
    }
    return 0;
}
"""
        )
        (folder / "pack.c").write_text(code)
        subprocess.run(
            [
                shutil.which("cc"),
                "-O2",
                "-I",
                str(ROOT / "tests"),
                str(folder / "pack.c"),
                "-o",
                str(folder / "pack"),
            ],
            check=True,
            capture_output=True,
        )
        subprocess.run([str(folder / "pack")], check=True, capture_output=True)

    def test_sampled_dw1_blocks_cover_every_output(self):
        kept = {j for j in range(72) if self.lib.needed(j)}
        self.assertEqual(len(kept), 36)
        for y in range(2, 18, 2):
            for x in range(1, 112, 2):
                self.assertIn((y * 114 + x + 2) // 32, kept)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def accepted(self, row):
        return bool(self.lib.accepted((ctypes.c_uint32 * 32)(*row)))

    def test_native_bounds_match_mirror(self):
        self.assertTrue(self.accepted(self.row))
        rng = random.Random(0x5354454D)
        for i in range(4000):
            row = self.row.copy()
            field = rng.randrange(32)
            row[field] = rng.choice(
                [
                    0,
                    1,
                    2,
                    7,
                    8,
                    20,
                    0x4000,
                    0xFFFFFFFF,
                    (row[field] - 1) & 0xFFFFFFFF,
                    row[field] + 1,
                ]
            )
            with self.subTest(case=i, field=field):
                self.assertEqual(self.accepted(row), S.native_ok(row))
        for a in (5, 6, 9, 10):
            for b in (5, 6, 9, 10):
                if a == b:
                    continue
                row = self.row.copy()
                row[a] = row[b]
                self.assertFalse(self.accepted(row))
        for stage in range(1, 6):
            row = self.row.copy()
            row[16:18] = [stage, 0x1C00000]
            self.assertTrue(self.accepted(row))
            row[17] = row[5]
            self.assertFalse(self.accepted(row))

    @unittest.skipUnless(importlib.util.find_spec("numpy"), "NumPy required")
    def test_maximum_all_finite_halves_and_signed_zero(self):
        import numpy as np

        bits = np.arange(65536, dtype="<u2")
        bits = bits[(bits & 0x7C00) != 0x7C00]
        rng = np.random.default_rng(44)
        other = rng.permutation(bits)
        for a, b in zip(bits, other):
            expected = b if bits.dtype.type(a).view("<f2") <= bits.dtype.type(b).view("<f2") else a
            got = self.lib.maxpair(int(a) | (int(b) << 16), int(b) | (int(a) << 16))
            self.assertEqual(got & 65535, int(expected))
        self.assertEqual(self.lib.maxpair(0, 0x80008000), 0x80008000)
        self.assertEqual(self.lib.maxpair(0x80008000, 0), 0)


@unittest.skipUnless(importlib.util.find_spec("numpy"), "NumPy required")
class StemGeometryTests(unittest.TestCase):
    def test_vector_rgb_loads_stay_in_owned_vtcm(self):
        # Halfword bounds include both source rows of a seam tile, every
        # channel/tap, and the unselected halves read by VMEMU.
        lo, hi = S.RGB * 128, (S.RGB + 2352) * 128
        for band in range(7):
            for j in range(72):
                y, col = divmod(32 * j, 114)
                y += 16 * band - 1
                cases = [(y, col - 2)]
                if 114 - col < 32:
                    cases.append((y + 1, -2))
                for row, x in cases:
                    for k in range(27):
                        iy = 2 * row + (k % 9) // 3
                        if not 0 <= row < 112 or iy >= 224:
                            continue
                        at = lo + 2 * ((k // 9) * 50176 + iy * 224 + 2 * x + k % 3)
                        self.assertGreaterEqual(at, lo - 8)
                        self.assertLessEqual(at + 128, hi + 64)
        self.assertGreaterEqual(lo - 8, 0)
        self.assertLessEqual(hi + 64, T.VTCM_SPAN_VECTORS * 128)

    def test_vtcm_liveness_alignment_and_band_halos(self):
        self.assertEqual((S.BAND.nbin, S.BAND.blk0, S.BAND.nj4), (89, 8, 72))
        spans = [
            (S.RGB, 2352),
            (S.OUT8, 3616),
            (S.OUT10, 904),
            (S.A, 2848),
            (S.B, 2848),
            (S.PAR, 433),
        ]
        for i, (at, n) in enumerate(spans):
            self.assertLessEqual(at + n, T.VTCM_SPAN_VECTORS)
            for b, m in spans[:i]:
                self.assertTrue(at >= b + m or b >= at + n)
        for off, n in zip(S.OFFSETS, S.LENGTHS):
            if off in (0, 224, 336):
                self.assertEqual((S.PAR + off) % 16, 0)
                self.assertEqual((S.PAR + n) % 16, 0)
        # A tail pixel samples DW1 centers 2,4,..16. Its radius-one expanded
        # inputs need DW0 radius-one stem rows 0..18, all within the 20-row band.
        self.assertEqual(min(range(2, 18, 2)) - 2, 0)
        self.assertEqual(max(range(2, 18, 2)) + 2, 18)

    def test_band_stitch_matches_full_contract_with_nonzero_biases(self):
        import numpy as np
        from unittest.mock import patch

        rng = np.random.default_rng(91)
        specs, weights = [], {}
        for i, name in enumerate(S.NAMES):
            cin, cout = [(3, 24), (24, 24), (24, 16), (16, 64), (64, 64)][i]
            dw = i in (1, 4)
            count = cout * 9 if dw else cout * (27 if i == 0 else cin)
            weights[name] = (rng.normal(0, 0.1, count), rng.normal(0, 0.1, cout))
            specs.append(
                dict(
                    name=name,
                    input_shape=(cin, 112, 112),
                    output_shape=(cout, 112, 112),
                    kernel=(3, 3) if dw else (1, 1),
                    stride=(2, 2) if i == 4 else (1, 1),
                    activation="clip" if i != 2 else "none",
                )
            )
        rgb = rng.uniform(-1, 1, (3, 224, 224))
        with patch(
            "framenpu.landmark.weights_and_bias", side_effect=lambda g, s: weights[s["name"]]
        ):
            _, full = S.emulate(None, specs, rgb)
            for band in range(7):
                _, out = S.emulate(None, specs, rgb, band=band)
                for name in ("8", "10"):
                    np.testing.assert_array_equal(out[name], full[name][:, 8 * band : 8 * band + 8])

    @unittest.skipUnless((MODEL / "hand.ncnn.bin").exists(), "public teacher required")
    def test_pinned_parameters_masks_and_composite_ddr_layout(self):
        import numpy as np
        from framenpu.convolution import load_landmark_graph
        from framenpu.landmark import specifications

        graph = load_landmark_graph(MODEL / "hand.ncnn.param", MODEL / "hand.ncnn.bin")
        prefix, tail = S.build_plan(graph, specifications(graph))
        self.assertEqual(prefix.where["in0"], tail.where["79"])
        self.assertLessEqual(prefix.end, T.WIN_HI)
        self.assertTrue(S.native_ok(np.frombuffer(prefix.table, "<u4")))
        spans = (
            list(tail.fixed.items())
            + list(prefix.fixed.items())
            + [(tail.table_at, tail.table), (prefix.table_at, prefix.table)]
        )
        for i, (a, data) in enumerate(spans):
            for b, other in spans[:i]:
                self.assertTrue(a >= b + len(other) or b >= a + len(data))
        par = next(iter(prefix.fixed.values()))
        for band in range(7):
            mask = np.frombuffer(
                par[(band * S.BLOCK + 40) * 128 : (band * S.BLOCK + 112) * 128], "<u4"
            )
            y = 16 * band - 1 + np.arange(len(mask)) // 114
            self.assertFalse(mask[(y < 0) | (y >= 112)].any())


class StemBuildTests(unittest.TestCase):
    def test_optional_prefix_fits_and_preserves_qualified_images(self):
        try:
            from framenpu.compiler import Toolchain

            Toolchain.discover()
        except Exception as exc:
            self.skipTest(str(exc))
        import build_teacher_link as B

        with tempfile.TemporaryDirectory() as tmp:
            heads = B.build(Path(tmp) / "heads", stride2=True, heads=True)
            stem = B.build(Path(tmp) / "stem", stride2=True, heads=True, stem=True)
            self.assertEqual(
                heads["linked"]["sha256"],
                "3ff18f84960175a1d744165b6bdebc9aded51a4aa005fe02c36691d79f4ec21b",
            )
            self.assertEqual(heads["reference"], stem["reference"])
            self.assertEqual(heads["loader"], stem["loader"])
            self.assertLessEqual(stem["linked"]["bytes"], 0x3F80)
            self.assertTrue(stem["stem"])


if __name__ == "__main__":
    unittest.main()
