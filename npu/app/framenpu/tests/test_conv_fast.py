"""Build checks for the opt-in fused 1x1 convolution path."""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TILED = "43791f45ace2c35abd0be61501121f7ab2f1f7a217de2d638927153de8feceb3"


def build(out, *flags):
    for tool, arguments in (
        ("build_resident_landmark.py", ["--linked", *flags, "--output", str(out)]),
        ("check_resident_link.py", ["--jobs", str(out), "--output", str(out / "verify.json")]),
    ):
        subprocess.run(
            [sys.executable, str(ROOT / "tools" / tool), *arguments],
            check=True,
            capture_output=True,
            cwd=ROOT,
        )
    return json.loads((out / "link.json").read_text())


@unittest.skipUnless(shutil.which("clang") and shutil.which("ld.lld"), "LLVM is not installed")
class ConvFastBuildTests(unittest.TestCase):
    def test_fused_image_fits_holds_no_vector_on_the_stack_and_is_opt_in(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            fast = build(folder / "fast", "--dw-reuse", "6", "--dw-row-tile", "--conv-fast")
            tiled = build(folder / "tile", "--dw-reuse", "6", "--dw-row-tile")
            self.assertTrue(fast["conv_fast"])
            self.assertFalse(tiled["conv_fast"])
            self.assertEqual(tiled["linked"]["sha256"], TILED)
            self.assertEqual(fast["loader"], tiled["loader"])
            self.assertEqual(fast["code_reservation_bytes"], 16384)
            self.assertLessEqual(fast["linked"]["bytes"], 16384)
            sys.path.insert(0, str(ROOT / "src"))
            from framenpu.resident_landmark import load_linked

            self.assertTrue(load_linked(folder / "fast").fused)
            self.assertFalse(load_linked(folder / "tile").fused)
            text = (folder / "fast/linked/disassembly.txt").read_text()
            self.assertNotRegex(text, r"vmemu?\((r29|r30)")
            for function in ("rg_pack_unit", "rg_stem_unit", "hmx_rows"):
                self.assertIn(f"<{function}>:", text)
            # The separate bias operator is unreachable in a fused image.
            self.assertNotIn("<rg_op_post>:", text)
            self.assertIn("<rg_op_post>:", (folder / "tile/linked/disassembly.txt").read_text())

    def test_fused_path_requires_the_wide_reservation(self):
        with tempfile.TemporaryDirectory() as folder:
            result = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools/build_resident_landmark.py"),
                    "--linked",
                    "--conv-fast",
                    "--output",
                    folder,
                ],
                capture_output=True,
                cwd=ROOT,
            )
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
