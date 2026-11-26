"""Independent layout, diagonal-mapping and nonlinear-fusion controls."""

import importlib.util
from pathlib import Path
import sys
import unittest

from hardware_hmx_model import pack, unpack

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from analyze_hmx_model import group_width, layer_cost, narrow_channels


class HmxModelCostTests(unittest.TestCase):
    def test_tail_channels_cannot_form_invalid_uniform_groups(self):
        self.assertEqual(group_width(24, 32), 8)
        self.assertEqual(group_width(144, 32), 16)
        self.assertEqual(group_width(672, 32), 32)

    def test_padding_cost_of_depthwise_mapping(self):
        # C24 x17 positions x9 taps: nine full hardware tiles despite sparse
        # coefficients; this is precisely the cost hidden by logical MACs.
        row = layer_cost("test", 24, 24, 17, 9, "group")
        self.assertEqual(row["logical_macs"], 3672)
        self.assertEqual(row["physical_macs"], 294912)
        self.assertEqual(row["load_pairs"], 9)
        self.assertEqual(row["transactions"], 1)

    def test_width_rounding_retains_small_channel_bottlenecks(self):
        self.assertEqual(narrow_channels(24, 0.5), 16)
        self.assertEqual(narrow_channels(112, 0.5), 56)


@unittest.skipUnless(importlib.util.find_spec("numpy"), "optional NumPy not installed")
class HmxModelMappingTests(unittest.TestCase):
    def test_pack_roundtrip_preserves_every_half_lane(self):
        import numpy as np

        values = (np.arange(1024, dtype=np.float32) / 128 - 4).astype(np.float16).reshape(32, 32)
        np.testing.assert_array_equal(unpack(pack(values)), values)

    def test_diagonal_group_is_depthwise_including_padding(self):
        import numpy as np

        depth, channels, positions = 9, 24, 17
        coeff = (
            np.arange(depth * channels, dtype=np.float32).reshape(depth, channels) % 11 - 5
        ) / 16
        inp = (
            np.arange(depth * channels * positions, dtype=np.float32).reshape(
                depth, channels, positions
            )
            % 13
            - 6
        ) / 16
        a = np.zeros((depth, 32, 32), dtype=np.float16)
        b = np.zeros_like(a)
        for t in range(depth):
            a[t, np.arange(channels), np.arange(channels)] = coeff[t]
            b[t, :channels, :positions] = inp[t]
        matrix = sum(unpack(pack(aa)) @ unpack(pack(bb)) for aa, bb in zip(a, b))
        expected = np.sum(coeff[:, :, None] * inp, axis=0)
        np.testing.assert_array_equal(matrix[:channels, :positions], expected)
        self.assertTrue(np.all(matrix[channels:] == 0))
        self.assertTrue(np.all(matrix[:, positions:] == 0))

    def test_clipping_prevents_analytical_expansion_spatial_folding(self):
        # expand by2, clip6, spatial coefficient1/2, clip6
        old = min(6, max(0, min(6, max(0, 4 * 2)) * 0.5))
        folded = min(6, max(0, 4 * (2 * 0.5)))
        self.assertEqual(old, 3)
        self.assertEqual(folded, 4)


if __name__ == "__main__":
    unittest.main()
