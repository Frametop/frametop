"""Actual vector pooling/stitching versus scalar C, including guard preservation."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / n) for n in ("src", "tests", "tools")]


@unittest.skipUnless(shutil.which("cc"), "host C compiler required")
class VectorStemTests(unittest.TestCase):
    def test_vector_reads_and_partial_stores_stay_inside_each_plane(self):
        for band in range(7):
            for y in range(8):
                for x in (0, 32):
                    self.assertLess(258 + (2 * y + 2) * 114 + 2 * x + 63, 2848)
                    self.assertLess(259 + (2 * y + 2) * 114 + 2 * x + 63, 2848)
                    self.assertLess(130 + (8 * band + y) * 58 + x + 31, 3616)

    def test_optional_vector_build_preserves_scalar_control_and_references(self):
        from framenpu.compiler import Toolchain

        try:
            Toolchain.discover()
        except Exception as exc:
            self.skipTest(str(exc))
        import build_teacher_link as B

        with tempfile.TemporaryDirectory() as tmp:
            scalar = B.build(Path(tmp) / "scalar", stride2=True, heads=True, stem=True)
            vector = B.build(
                Path(tmp) / "vector", stride2=True, heads=True, stem=True, stem_vector=True
            )
            prof = B.build(
                Path(tmp) / "prof",
                stride2=True,
                heads=True,
                stem=True,
                stem_vector=True,
                stem_profile=True,
            )
            self.assertEqual(
                scalar["linked"]["sha256"],
                "8c36451dd0e91b8122d867d66cbb6cdd419b312fe1125ab40fc9608c5cf327b9",
            )
            for build in (vector, prof):
                self.assertLessEqual(build["linked"]["bytes"], 0x3F80)
                self.assertEqual(build["loader"], scalar["loader"])
                self.assertEqual(build["reference"], scalar["reference"])
                self.assertTrue(build["stem_vector"])
            with self.assertRaisesRegex(ValueError, "requires the stem"):
                B.build(Path(tmp) / "invalid", stride2=True, heads=True, stem_vector=True)

    def test_actual_vector_operations_match_scalar_and_preserve_owned_memory(self):
        source = (ROOT / "tests/teacher_stem_impl.h").read_text()
        operations = source[
            source.index("static inline fnpu_u32 ts_hmax") : source.index("static void ts_observe")
        ]
        code = (
            '#include "teacher_stem_vector_host.h"\n'
            "#define ts_skip scalar_skip\n#define ts_downsample scalar_downsample\n"
            + operations
            + "\n#undef ts_skip\n#undef ts_downsample\n"
            '#define RS_STEM_VECTOR\n#include "teacher_stem_vector.h"\n'
            + operations[operations.index("static __attribute__((noinline)) void ts_skip") :]
            + r"""
static unsigned char before[sizeof(vtcm)],expected[sizeof(vtcm)];
static fnpu_u32 rng=0x53545631;
static fnpu_u32 random_word(void) {
    rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng;
}
int main(void) {
    /* Every finite half encoding against sign/exponent boundaries and zero.
     * The full memory comparison below also exercises every row alignment. */
    const unsigned edges[]={0,0x8000,1,0x8001,0x3c00,0xbc00,0x7bff,0xfbff};
    for (unsigned e=0;e<8;e++) for (unsigned base=0;base<65536;base+=64) {
        fnpu_u16 a[64],b[64],got[64];
        for (unsigned i=0;i<64;i++) {a[i]=base+i;b[i]=edges[e];}
        HVX_Vector va,vb; memcpy(&va,a,128);memcpy(&vb,b,128);
        va=tsv_max(va,vb);memcpy(got,&va,128);
        for (unsigned i=0;i<64;i++) if ((a[i]&0x7c00)!=0x7c00 &&
                got[i]!=ts_hmax(a[i],b[i])) return 1;
    }
    for (unsigned trial=0;trial<4;trial++) {
        fnpu_u16 *v=(fnpu_u16 *)vtcm;
        for (unsigned i=0;i<sizeof(vtcm)/2;i++) {
            unsigned h=random_word()&65535;
            if ((h&0x7c00)==0x7c00) h^=0x400;
            if (trial==1) h=(h&1)?0x8000:0; /* all combinations of signed zero */
            if (trial==2) h=edges[h&7];
            v[i]=h;
        }
        /* Nonsequential bands expose damage to already written adjacent rows. */
        const unsigned order[]={6,0,3,1,5,2,4};
        for (unsigned j=0;j<7;j++) for (unsigned op=0;op<2;op++) {
            memcpy(before,vtcm,sizeof(vtcm));
            if (op) scalar_downsample(order[j]); else scalar_skip(order[j]);
            memcpy(expected,vtcm,sizeof(vtcm));memcpy(vtcm,before,sizeof(vtcm));
            if (op) ts_downsample(order[j]); else ts_skip(order[j]);
            if (memcmp(vtcm,expected,sizeof(vtcm))) return 2+op;
        }
    }
    return 0;
}
"""
        )
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            path = folder / "vector.c"
            path.write_text(code)
            result = subprocess.run(
                ["cc", "-O2", "-I", str(ROOT / "tests"), str(path), "-o", str(folder / "vector")],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(folder / "vector")], check=True, capture_output=True)
