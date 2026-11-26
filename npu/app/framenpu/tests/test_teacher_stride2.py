"""Bounds checks for the linked teacher stride-2 extension; opens no device."""

import ctypes
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tests"), str(ROOT / "tools")]
MODEL = Path(os.environ.get("FNPU_TEACHER_MODEL", "/nonexistent"))
HAVE_MODEL = (MODEL / "hand.ncnn.param").exists()
NUMPY = importlib.util.find_spec("numpy") is not None


@unittest.skipUnless(
    NUMPY and HAVE_MODEL and shutil.which("cc"), "numpy, model and host C compiler required"
)
class NativeStride2Checks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import numpy as np
        from framenpu.convolution import load_landmark_graph
        from framenpu.landmark import specifications
        import teacher_resident_layout as R

        graph = load_landmark_graph(MODEL / "hand.ncnn.param", MODEL / "hand.ncnn.bin")
        specs = specifications(graph)
        cls.plans = [
            R.build_plan(graph, specs, m, split_after=split)
            for m in ("merged", "merged-observe")
            for split in (None, "convdwclip_5")
        ]
        cls.rows = np.concatenate(
            [np.frombuffer(p.table, "<u4").reshape(-1, 32) for p in cls.plans]
        )
        cls.R = R
        # Compile the actual descriptor checker on the host. Only the vector
        # copy and address macros are stubbed; its validation code is verbatim.
        source = (ROOT / "tests/teacher_resident_probe.c").read_text()
        struct = source[
            source.index("typedef struct {") : source.index("} rs_op;") + len("} rs_op;")
        ]
        check = source[source.index("/* Fetch descriptor") : source.index("/* Shared post")]
        scratch = source[source.index("#ifdef RS_PW_GROUP") : source.index("typedef long rs_uv")]
        prefix = """
#include <stdint.h>
#include <string.h>
typedef uint32_t fnpu_u32;
typedef long HVX_Vector __attribute__((vector_size(128)));
#define RS_STRIDE2
#define RS_MAX_KIND 5u
#define SPAN ((0x311e0000u - 0x31002000u) / 128u)
#define RS_DDR 0xf0000000u
#define RS_WIN_LO 0x1a40000u
#define RS_WIN_HI 0x1f00000u
#define FR(i) (0x31002000u + 128u * (i))
static HVX_Vector desc, input;
#define RS_DESC (&desc)
static void rs_copy(volatile HVX_Vector *to, const volatile HVX_Vector *from, fnpu_u32 n) {
    (void)from; (void)n;
    memcpy((void *)to, &input, 128);
}
"""
        suffix = """
int accepted(const uint32_t *row) {
    memcpy(&input, row, 128);
    rs_op d;
#ifdef RS_REPLAY
    return rs_get(RS_WIN_LO, 0, &d, 0) == 0;
#else
    return rs_get(RS_WIN_LO, 0, &d) == 0;
#endif
}
#ifdef RS_REPLAY
int accepted_replay(const uint32_t *row, uint32_t off) {
    memcpy(&input, row, 128);
    rs_op d;
    return rs_get(RS_WIN_LO, 0, &d, off) == 0;
}
#endif
"""
        cls.tmp = tempfile.TemporaryDirectory()
        folder = Path(cls.tmp.name)
        (folder / "check.c").write_text(prefix + scratch + struct + check + suffix)
        cls.libs = {}
        for blocks, replay in ((2, False), (4, False), (2, True)):
            libpath = folder / f"check{blocks}-{replay}.so"
            subprocess.run(
                [
                    shutil.which("cc"),
                    "-shared",
                    "-fPIC",
                    "-O2",
                    f"-DRS_DW_BLOCKS={blocks}",
                    *(["-DRS_REPLAY"] if replay else []),
                    str(folder / "check.c"),
                    "-o",
                    str(libpath),
                ],
                check=True,
                capture_output=True,
            )
            lib = ctypes.CDLL(str(libpath))
            lib.accepted.argtypes = [ctypes.POINTER(ctypes.c_uint32)]
            lib.accepted.restype = ctypes.c_int
            if replay:
                lib.accepted_replay.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32]
                lib.accepted_replay.restype = ctypes.c_int
                cls.replay_lib = lib
            else:
                cls.libs[blocks] = lib

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def accepted(self, row, *, dw_blocks=2):
        import numpy as np

        row = np.asarray(row, dtype="<u4")
        return bool(
            self.libs[dw_blocks].accepted(row.ctypes.data_as(ctypes.POINTER(ctypes.c_uint32)))
        )

    def test_all_plan_descriptors_pass_actual_native_checker(self):
        for i, row in enumerate(self.rows):
            with self.subTest(row=i):
                self.assertTrue(self.accepted(row))
                self.assertTrue(self.accepted(row, dw_blocks=4))

    def test_replay_offsets_match_actual_native_checker(self):
        import numpy as np
        import teacher_tail_layout as T

        for i, row in enumerate(self.rows):
            row = np.asarray(row, dtype="<u4")
            for offset in (0, 4096, (T.WIN_HI - T.WIN_LO) // 2, T.WIN_HI - T.WIN_LO):
                with self.subTest(row=i, offset=offset):
                    got = self.replay_lib.accepted_replay(
                        row.ctypes.data_as(ctypes.POINTER(ctypes.c_uint32)), offset
                    )
                    want = self.R.native_ok(self.R.replay_row(row, 1, offset))
                    self.assertEqual(bool(got), want)

    def test_mutations_match_python_mirror(self):
        import numpy as np

        rng = np.random.default_rng(19)
        for i in range(4000):
            row = self.rows[int(rng.integers(len(self.rows)))].copy()
            field = int(rng.integers(21))
            values = (
                0,
                1,
                0x3FFF,
                0x4000,
                0xFFFFFFFF,
                self.R.SPAN,
                (int(row[field]) - 1) & 0xFFFFFFFF,
                (int(row[field]) + 1) & 0xFFFFFFFF,
            )
            row[field] = values[int(rng.integers(len(values)))]
            with self.subTest(case=i, field=field):
                self.assertEqual(self.accepted(row), bool(self.R.native_ok(row)))
                self.assertEqual(
                    self.accepted(row, dw_blocks=4), bool(self.R.native_ok(row, dw_blocks=4))
                )

    def test_four_block_loop_rejects_a_two_block_remainder(self):
        row = next(r for r in self.rows if r[0] == 2).copy()
        row[7] -= 2
        self.assertTrue(self.accepted(row))
        self.assertTrue(self.R.native_ok(row))
        self.assertFalse(self.accepted(row, dw_blocks=4))
        self.assertFalse(self.R.native_ok(row, dw_blocks=4))

    def test_stride2_rejects_invalid_geometry_and_buffer_bounds(self):
        s2 = [r for r in self.rows if r[0] == 5]
        for base in s2:
            for field, value in (
                (15, 0),
                (15, self.R.SPAN),
                (16, self.R.SPAN),
                (17, 0),
                (18, 0),
                (18, 255),
                (19, 0),
                (19, 255),
                (20, 0),
                (20, self.R.SPAN),
                (12, self.R.SPAN),
                (11, self.R.SPAN),
                (9, 0x1A3FF80),
                (9, 0x1F00000),
                (13, 1),
                (4, 0),
            ):
                row = base.copy()
                row[field] = value
                with self.subTest(field=field, value=value):
                    self.assertFalse(self.accepted(row))
                    self.assertFalse(self.R.native_ok(row))


if __name__ == "__main__":
    unittest.main()
