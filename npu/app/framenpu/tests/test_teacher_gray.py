"""Single-plane input: exact tile packing, isolated descriptors and RGB controls."""

import ctypes
import importlib.util
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / n) for n in ("src", "tests", "tools")]
import teacher_stem_layout as S

MODEL = Path(os.environ.get("FNPU_TEACHER_MODEL", "/nonexistent"))


@unittest.skipUnless(shutil.which("cc"), "host C compiler required")
class GrayNativeTests(unittest.TestCase):
    def test_gray_native_bounds_and_format_match_mirror(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            path = folder / "check.c"
            path.write_text(
                '#include <stdint.h>\ntypedef uint32_t fnpu_u32;\n#define RS_STEM_GRAY\n#include "teacher_stem_check.h"\nint accepted(const fnpu_u32 *r) { return !ts_bad(r); }\n'
            )
            subprocess.run(
                [
                    "cc",
                    "-shared",
                    "-fPIC",
                    "-O2",
                    "-I",
                    str(ROOT / "tests"),
                    str(path),
                    "-o",
                    str(folder / "check.so"),
                ],
                check=True,
                capture_output=True,
            )
            lib = ctypes.CDLL(str(folder / "check.so"))
            lib.accepted.argtypes = [ctypes.POINTER(ctypes.c_uint32)]

            def accepted(r):
                return bool(lib.accepted((ctypes.c_uint32 * 32)(*r)))

            row = [
                8,
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
            self.assertTrue(accepted(row))
            self.assertFalse(S.native_ok(row))
            bad = row.copy()
            bad[0] = 7
            self.assertFalse(accepted(bad))
            rng = random.Random(0x47524159)
            for _ in range(4000):
                r = row.copy()
                i = rng.randrange(32)
                r[i] = rng.choice([0, 1, 7, 8, 9, 0xFFFFFFFF, (r[i] - 1) & 0xFFFFFFFF, r[i] + 1])
                self.assertEqual(accepted(r), S.native_ok(r, grayscale=True))
            for at in (0x1F00000 - 784 * 128, 0x1F00000 - 784 * 128 + 128):
                r = row.copy()
                r[5] = at
                self.assertEqual(accepted(r), S.native_ok(r, grayscale=True))
            r = row.copy()
            r[6] = r[5] + 784 * 128
            self.assertTrue(accepted(r))  # ends exactly at next owned span
            r[6] -= 128
            self.assertFalse(accepted(r))

    def test_gray_actual_packer_matches_replicated_rgb_every_tile(self):
        source = (ROOT / "tests/teacher_stem_impl.h").read_text()
        packer = source[
            source.index("typedef long ts_uv2") : source.index(
                "static __attribute__((noinline)) void ts_stem"
            )
        ]
        code = (
            '#include "teacher_stem_pack_host.h"\n#define RS_STEM_GRAY\n'
            + packer
            + r"""
int main(void) {
    fnpu_u16 *gray=(fnpu_u16 *)FR(TS_RGB);
    /* All finite and nonfinite half encodings may be moved without rounding.
     * Poison the removed planes: no live packed feature may use them. */
    memset(vtcm,0xa5,sizeof(vtcm));
    for (unsigned i=0;i<50176;i++) gray[i]=(i*31u+i/37u)&65535u;
    rs_op d={.scr=FR(12688)};
    for (unsigned band=0;band<7;band++) for (unsigned j=0;j<72;j++) {
        ts_rgb_tile(&d,band,j);
        const fnpu_u16 *b=(const fnpu_u16 *)d.scr;
        for (unsigned k=0;k<32;k++) for (unsigned lane=0;lane<32;lane++) {
            unsigned pos=32*j+lane;
            int y=16*(int)band-1+pos/114,x=(int)(pos%114)-2;
            unsigned want=0;
            if (k<27 && y>=0 && y<112) {
                int iy=2*y+(k%9)/3,ix=2*x+k%3;
                if (iy<224 && ix>=0 && ix<224) want=gray[iy*224+ix];
            }
            if (b[(k/2)*64+2*lane+k%2]!=want) return 1;
        }
    }
    return 0;
}
"""
        )
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            path = folder / "pack.c"
            path.write_text(code)
            subprocess.run(
                ["cc", "-O2", "-I", str(ROOT / "tests"), str(path), "-o", str(folder / "pack")],
                check=True,
                capture_output=True,
            )
            subprocess.run([str(folder / "pack")], check=True, capture_output=True)


class GrayPlanTests(unittest.TestCase):
    @unittest.skipUnless((MODEL / "hand.ncnn.bin").exists(), "public teacher required")
    def test_gray_planner_preserves_all_parameters_and_tail(self):
        from framenpu.convolution import load_landmark_graph
        from framenpu.landmark import specifications
        from hardware_teacher_gray import weight_analysis

        g = load_landmark_graph(MODEL / "hand.ncnn.param", MODEL / "hand.ncnn.bin")
        specs = specifications(g)
        rgb, tail = S.build_plan(g, specs)
        gray, gray_tail = S.build_plan(g, specs, grayscale=True)
        self.assertEqual(tail, gray_tail)
        self.assertEqual(rgb.fixed, gray.fixed)
        a = list(struct.unpack("<32I", rgb.table))
        b = list(struct.unpack("<32I", gray.table))
        a[0] = 8
        self.assertEqual(a, b)
        self.assertTrue(S.native_ok(b, grayscale=True))
        stats = weight_analysis(g, specs)
        self.assertEqual(stats["coefficients_changed_by_fp16_rounding"], 124)
        self.assertEqual(stats["padded_hmx_k_rgb"], stats["padded_hmx_k_folded"])

    @unittest.skipUnless(importlib.util.find_spec("numpy"), "NumPy required")
    def test_gray_stager_uploads_one_plane_and_rejects_rgb(self):
        import numpy as np
        from hardware_teacher_gray import GrayRunner
        from types import SimpleNamespace

        runner = object.__new__(GrayRunner)
        runner.prefix = SimpleNamespace(where={"in0": 0x1A40000})
        writes = []
        runner.put = lambda at, data: writes.append((at, data))
        gray = np.arange(224 * 224, dtype=np.float32).reshape(224, 224) / 50176
        runner.stage_gray(gray)
        self.assertEqual(len(writes[0][1]), 100352)
        self.assertEqual(writes[0][1], gray.astype("<f2").tobytes())
        with self.assertRaises(ValueError):
            runner.stage_gray(np.repeat(gray[None], 3, axis=0))

    def test_gray_build_preserves_rgb_images_and_reference_kernels(self):
        from framenpu.compiler import Toolchain

        try:
            Toolchain.discover()
        except Exception as exc:
            self.skipTest(str(exc))
        import build_teacher_link as B

        with tempfile.TemporaryDirectory() as tmp:
            rgb = B.build(Path(tmp) / "rgb", stride2=True, heads=True, stem=True, stem_vector=True)
            gray = B.build(
                Path(tmp) / "gray",
                stride2=True,
                heads=True,
                stem=True,
                stem_vector=True,
                stem_gray=True,
            )
            self.assertEqual(
                rgb["linked"]["sha256"],
                "ee1d8256c89cdd40caeb51c67b4c668d664a6c7219bb2e73fdd79c3d1523a214",
            )
            self.assertEqual(rgb["loader"], gray["loader"])
            self.assertEqual(rgb["reference"], gray["reference"])
            self.assertLessEqual(gray["linked"]["bytes"], 0x3F80)
            with self.assertRaisesRegex(ValueError, "without profiling"):
                B.build(
                    Path(tmp) / "bad",
                    stride2=True,
                    heads=True,
                    stem=True,
                    stem_gray=True,
                    stem_profile=True,
                )
