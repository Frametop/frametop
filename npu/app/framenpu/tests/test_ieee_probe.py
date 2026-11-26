"""Host oracle/lane-layout checks for IEEE arithmetic experiments."""

import math
import struct
import unittest

from hardware_ieee_probe import decode_values, expected, payload, rounded


class IeeeProbeOracleTests(unittest.TestCase):
    def test_mixed_accumulation_preserves_product_below_half_range(self):
        x, w = [2**-24] * 64, [2**-24] * 64
        self.assertEqual(expected(x, w, 0.0, 1, 2), [2**-48] * 64)
        self.assertEqual(expected(x, w, 0.0, 1, 3), [0.0] * 64)

    def test_half_accumulation_rounds_each_step(self):
        # 2048 + 1 is an exact half-way case and ties to even every step.
        self.assertEqual(expected([1.0] * 64, [1.0] * 64, 2048.0, 2, 3), [2048.0] * 64)
        self.assertEqual(expected([1.0] * 64, [1.0] * 64, 2048.0, 2, 2), [2050.0] * 64)

    def test_pair_lane_order_has_no_permutation_ambiguity(self):
        values = list(range(64))
        ordered = values[::2] + values[1::2]
        raw = bytes(256) + struct.pack("<64f", *ordered)
        self.assertEqual(decode_values(raw, 2), values)
        self.assertNotEqual(decode_values(raw, 2, "contiguous"), values)

    def test_payload_is_bounded_and_rounds_inputs_to_half(self):
        raw, hx, hw = payload([0.1] * 64, [0.2] * 64)
        self.assertEqual(len(raw), 1152)
        self.assertEqual(hx, [rounded(0.1, True)] * 64)
        self.assertEqual(hw, [rounded(0.2, True)] * 64)
        self.assertNotEqual(hx[0], 0.1)

    def test_overflow_is_represented_without_host_exception(self):
        self.assertEqual(rounded(65504 * 2, True), math.inf)
        self.assertEqual(rounded(-1e40), -math.inf)


# NumPy remains optional for the package; it is required for recorded-layer pilots.
import importlib.util
from hardware_native_depthwise import prepare, unpack
from types import SimpleNamespace


@unittest.skipUnless(importlib.util.find_spec("numpy"), "optional NumPy not installed")
class NativeDepthwiseLayoutTests(unittest.TestCase):
    def test_panel_geometry_and_every_vector_read_fit_saved_vtcm(self):
        import numpy as np

        for width in (1, 7, 14, 28, 56, 112):
            for k in (3, 5):
                for stride in (1, 2):
                    ow = (width + stride - 1) // stride
                    total = max((ow - 1) * stride + k - width, 0)
                    pad, tail = total // 2, total - total // 2
                    spec = {
                        "input_shape": (9, width, width),
                        "output_shape": (9, ow, ow),
                        "kernel": (k, k),
                        "stride": (stride, stride),
                        "padding": (pad, tail, pad, tail),
                        "dilation": (1, 1),
                        "activation": "clip",
                    }
                    source = (
                        np.arange(9 * width * width, dtype=np.float32).reshape(9, width, width) % 13
                    )
                    weights = np.ones((9, k * k), dtype=np.float16)
                    bias = np.zeros(9, dtype=np.float32)
                    for mode in range(3):
                        batches, _ = prepare(spec, source, weights, bias, mode)
                        self.assertEqual(sum(b["count"] for b in batches), 9)
                        for batch in batches:
                            self.assertLessEqual(len(batch["data"]) + batch["output_bytes"], 131072)
                            d = struct.unpack_from("<32I", batch["data"])
                            self.assertEqual(d[14], tail)
                            self.assertEqual(d[11], (width + pad + tail) * d[10])
                        lanes = 64 if mode == 2 else 32
                        for x in range(0, ow, lanes):
                            # Native loads full vectors, including unused tail lanes.
                            self.assertLessEqual(x * stride + k - 1 + lanes * stride, 256)

    def test_mixed_pair_unpacks_each_spatial_tile_independently(self):
        import numpy as np

        shape = (2, 3, 112)
        logical = np.arange(2 * 3 * 128, dtype=np.float32).reshape(2, 3, 128)
        pairs = logical.reshape(2, 3, 2, 32, 2).transpose(0, 1, 2, 4, 3).copy()
        result = SimpleNamespace(output=bytes(256) + pairs.tobytes())
        actual = unpack(result, {"count": 2, "row_bytes": 512}, {"output_shape": shape}, 2)
        np.testing.assert_array_equal(actual, logical[:, :, :112])

    def test_padded_input_contains_exact_original_coordinates(self):
        import numpy as np

        spec = {
            "input_shape": (1, 7, 7),
            "output_shape": (1, 4, 4),
            "kernel": (3, 3),
            "stride": (2, 2),
            "padding": (1, 1, 1, 1),
            "dilation": (1, 1),
            "activation": "clip",
        }
        source = np.arange(49, dtype=np.float32).reshape(1, 7, 7)
        for mode in range(3):
            batches, _ = prepare(
                spec, source, np.ones((1, 9), dtype=np.float16), np.zeros(1, dtype=np.float32), mode
            )
            raw = batches[0]["data"]
            panel = np.frombuffer(raw, offset=256, dtype="<f2" if mode == 2 else "<f4").reshape(
                9, -1
            )
            np.testing.assert_array_equal(panel[1:8, 1:8], source[0])
            self.assertTrue(np.all(panel[0] == 0))
            self.assertTrue(np.all(panel[-1] == 0))
            self.assertTrue(np.all(panel[:, 0] == 0))
            self.assertTrue(np.all(panel[:, 8:] == 0))


if __name__ == "__main__":
    unittest.main()
