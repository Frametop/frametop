"""Independent coefficient overwrite/bounds and IEEE mask equivalence proofs."""

from pathlib import Path
import random
import struct
import subprocess
import sys
import unittest

from hardware_dw_reuse import classification, cases


class DepthwiseReuseTests(unittest.TestCase):
    def test_descending_half_expansion_preserves_unread_coefficients(self):
        # All finite normal/zero binary16 patterns accepted by the DW loader.
        values = [h for h in range(65536) if h & 0x7C00 not in (0, 0x7C00) or h & 0x7FFF == 0]
        for n in (1, 3, 9, 15, 25):
            for start in range(0, len(values), n):
                half = (values[start : start + n] + [0] * n)[:n]
                page = bytearray(struct.pack(f"<{n}H", *half).ljust(128, b"\xa5"))
                expected = [
                    struct.pack("<f", struct.unpack("<e", struct.pack("<H", h))[0]) for h in half
                ]
                for i in reversed(range(n)):
                    h = struct.unpack_from("<H", page, i * 2)[0]
                    sign, mag = (h & 0x8000) << 16, h & 0x7FFF
                    struct.pack_into(
                        "<I", page, i * 4, sign | ((mag << 13) + 0x38000000 if mag else 0)
                    )
                self.assertEqual(bytes(page[: n * 4]), b"".join(expected))
                self.assertEqual(
                    bytes(page[max(n * 4, n * 2) :]), b"\xa5" * (128 - max(n * 4, n * 2))
                )

    def test_magnitude_comparison_matches_exponent_and_zero_rules(self):
        rng = random.Random(19)
        words = [rng.getrandbits(32) for _ in range(10000)]
        words += [
            sign | exp << 23 | mant
            for sign in (0, 0x80000000)
            for exp in range(256)
            for mant in (0, 1, 0x7FFFFF)
        ]
        for x in words:
            self.assertEqual((x & 0x7FFFFFFF) > 0x7F7FFFFF, (x & 0x7F800000) == 0x7F800000)
            self.assertEqual((x & 0x7FFFFFFF) == 0, (x << 1) & 0xFFFFFFFF == 0)
        for a, b, raw in cases():
            nf, zero = classification(raw)
            self.assertEqual(len(nf), 32)
            self.assertEqual(len(zero), 32)
            self.assertTrue(all(x & 0x7F800000 != 0x7F800000 for x in a + b))

    def test_saved_vtcm_regions_do_not_overlap(self):
        regions = [
            (0, 0x1000),
            (0x1A00, 0x1B00),
            (0x1B00, 0x1B80),
            (0x1C00, 0x1C80),
            (0x1C80, 0x1D00),
            (0x1D80, 0x1E00),
            (0x1E00, 0x1E80),
            (0x1E80, 0x1F00),
            (0x1F00, 0x1F1C),
            (0x1F20, 0x1F2C),
            (0x1F30, 0x1F34),
            (0x1F80, 0x2000),
        ]
        for i, (lo, hi) in enumerate(regions):
            self.assertLessEqual(hi, 8192)
            for x, y in regions[i + 1 :]:
                self.assertTrue(hi <= x or y <= lo)
        self.assertLessEqual(25 * 4, 128)

    def test_broadcast_page_fits_below_load_scratch(self):
        rows_end = 5 * 512
        page_begin, page_end = 0xC00, 0xC00 + 25 * 128
        self.assertLessEqual(rows_end, page_begin)
        self.assertLessEqual(page_end, 0x1A00)
        # Avg pooling may overlap this page at a different operator; the
        # cache must be invalidated at that transition and on every request.
        self.assertLess(page_begin, 0x1000)

    def test_hardware_controls_require_explicit_opt_in(self):
        result = subprocess.run(
            [
                sys.executable,
                str(Path(__file__).with_name("hardware_dw_reuse.py")),
                "--probe",
                "unused",
                "--output",
                "unused",
            ],
            cwd="/tmp",
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("--run-on-device and Frame root are required", result.stderr)


if __name__ == "__main__":
    unittest.main()
