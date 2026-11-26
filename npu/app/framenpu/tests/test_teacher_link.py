"""Offline checks of the teacher linked loader and operator image."""

import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tests"), str(ROOT / "tools")]

try:
    from framenpu.compiler import Toolchain

    Toolchain.discover()
    HAVE_TOOLS = True
except Exception:  # noqa: BLE001
    HAVE_TOOLS = False


@unittest.skipUnless(HAVE_TOOLS, "Hexagon toolchain unavailable")
class TeacherLinkBuild(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import build_teacher_link as BL
        import build_teacher_resident as BR

        cls.BL = BL
        cls.tmp = tempfile.TemporaryDirectory()
        cls.out = Path(cls.tmp.name)
        cls.record = BL.build(cls.out / "link")
        cls.stride2 = BL.build(cls.out / "stride2", stride2=True)
        cls.dw4 = BL.build(cls.out / "dw4", stride2=True, dw_blocks=4)
        cls.reference = BR.build(cls.out / "ref")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_sizes_and_reservation(self):
        self.assertLessEqual(self.record["loader"]["bytes"], 4096)
        self.assertLessEqual(self.record["linked"]["bytes"], 0x3F80)
        image = (self.out / "link/teacher_link.bin").read_bytes()
        self.assertEqual(hashlib.sha256(image).hexdigest(), self.record["linked"]["sha256"])

    def test_reference_kernels_unchanged_by_link_build(self):
        for k in ("rs", "dw"):
            self.assertEqual(self.record["reference"][k]["sha256"], self.reference[k]["sha256"])
            self.assertEqual(self.stride2["reference"][k]["sha256"], self.reference[k]["sha256"])
            self.assertEqual(self.dw4["reference"][k]["sha256"], self.reference[k]["sha256"])

    def test_stride2_build_preserves_qualified_images(self):
        self.assertLessEqual(self.stride2["linked"]["bytes"], 0x3F80)
        self.assertTrue(self.stride2["stride2"])
        self.assertFalse(self.record["stride2"])
        self.assertEqual(self.record["loader"], self.stride2["loader"])
        self.assertEqual(self.record["loader"], self.dw4["loader"])
        self.assertEqual(self.dw4["dw_blocks"], 4)
        self.assertLessEqual(self.dw4["linked"]["bytes"], 0x3F80)
        self.assertEqual(self.stride2["dw_blocks"], 2)
        self.assertEqual(
            self.stride2["linked"]["sha256"],
            "ef5721f82e2f6fe20934032b0b9692ec422cae24aec863fe9c66c37aeae5a244",
        )
        # Frozen hashes from teacher-link-fp16.json: the per-layer oracle and
        # seven-request control must remain the qualified machine code.
        self.assertEqual(
            self.record["linked"]["sha256"],
            "30d64e47b5caf56dc730ef847645b7e5f594bc74ccbb9d11d8a360f09874a90a",
        )
        self.assertEqual(
            self.reference["rs"]["sha256"],
            "8eef3ad03b232fa54addd6713e390b49ccee3c4db679e89ce87b63ed408448ae",
        )
        self.assertEqual(
            self.reference["dw"]["sha256"],
            "3d11cee3f26c441490f094735956bcd6102dfc2ad59e01225e8c54d32c97aa8e",
        )

    def test_mercury_images_frozen(self):
        # Device-qualified Mercury images (evidence/mercury-*.json): even-phase
        # stride 2, direct stride 2, direct stride 2 with the fast 1x1 post, and that plus
        # the fast depthwise, 1x1 groups of 8 column blocks, and crop replay.
        frozen = {
            (): "d0427fb0137f4d34d4a1046c1a4bced7af582e618c279b8468c171cfb6b39e0b",
            ("s2_direct",): "58c5ec58ac5df03982d416e1f08640c929fcc36c517ffdb848005b1e0e987916",
            ("s2_direct", "pw_fast"): (
                "e7b064418102b2da41a34b9ef76aa3df89762fbcc196afa8ca68171e0e14d04f"
            ),
            ("s2_direct", "pw_fast", "dw_fast"): (
                "89511c1854be82a553e0bb5f8dc2b402a655bc711c6e678a339cab9844b00c67"
            ),
            ("s2_direct", "pw_fast", "dw_fast", ("pw_group", 8)): (
                "a2377f9cf86850138368181d44142225da491059679723dde24e3316a44c0a9d"
            ),
            ("s2_direct", "pw_fast", "dw_fast", ("pw_group", 8), "replay"): (
                "d7a78a81469c10acaf193b0d99cc0415f92d1de845a7a9aa292a7a1ebe691819"
            ),
            ("s2_direct", "pw_fast", "dw_fast", ("pw_group", 8), "replay", "mheads"): (
                "582d79ce821304ab90b48d18c5e9d7c5e4396614b9c6eb70bf2e7b6febf114d3"
            ),
        }
        for i, (flags, digest) in enumerate(frozen.items()):
            kw = dict(f if isinstance(f, tuple) else (f, True) for f in flags)
            r = self.BL.build(self.out / f"merc{i}", stride2=True, s2_even=True, **kw)
            self.assertEqual(r["linked"]["sha256"], digest, flags)
            self.assertLessEqual(r["linked"]["bytes"], 0x3F80)
            self.assertEqual(r["loader"], self.record["loader"])

    def test_pw_options_need_stride2(self):
        for flag in ("pw_fast", "pw_profile", "dw_fast", "replay", "mheads"):
            with self.assertRaises(ValueError):
                self.BL.build(self.out / f"bad-{flag}", **{flag: True})
        for group, fast in ((8, False), (3, True), (16, True)):
            with self.assertRaises(ValueError):
                self.BL.build(
                    self.out / f"bad-g{group}", stride2=True, pw_fast=fast, pw_group=group
                )

    def test_loader_checks_reject_a_wrong_seal_constant(self):
        text = (self.out / "link/loader/disassembly.txt").read_text()
        from check_resident_link import _symbols

        symbols = _symbols(self.out / "link/loader/kernel.elf")
        self.BL.verify_loader(text, symbols)
        with self.assertRaises(ValueError):
            self.BL.verify_loader(text.replace("0x544c4e4b", "0x544c4e4c"), symbols)

    def test_image_contains_no_slot_or_self_address(self):
        text = (self.out / "link/linked/disassembly.txt").read_text()
        self.assertNotIn("0x87a92000", text)
        self.assertNotIn("0xf0a92000", text)

    def test_admin_descriptor_layout(self):
        import hardware_teacher_link as HL

        data = struct.pack("<4I", HL.ADMIN_MAGIC, 7, HL.CODE, HL.CODE_BYTES)
        self.assertEqual(struct.unpack("<4I", data), (0x544C4144, 7, 0x1FF0000, 0x4000))
        self.assertEqual(HL.SEAL_AT, HL.CODE + HL.CODE_BYTES - 128)


if __name__ == "__main__":
    unittest.main()
