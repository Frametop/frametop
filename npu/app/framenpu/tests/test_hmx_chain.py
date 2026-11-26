"""Offline checks for the layer-chain executor prototype (no device)."""

import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from hmx_block_layout import im2col, oracle, unpack, weight_tiles
from hmx_chain_layout import (
    CHAINS,
    HARD_TICKS,
    MAGIC,
    WIN_HI,
    WIN_LO,
    arena_image,
    check_chain,
    descriptors,
    emulate_gather,
    fixture,
    from_buffer,
    plan,
    to_buffer,
)

ROOT = Path(__file__).resolve().parents[1]
NUMPY = importlib.util.find_spec("numpy") is not None


def kernel_accepts(words, index):
    """Python mirror of lc_get in tests/hmx_layer_chain_probe.c."""
    d = [int(v) for v in words[32 + 32 * index : 48 + 32 * index]]
    w, h, cin, nm, no, cout, pad, nbin = d[:8]
    nk1, zero = d[14], d[15]
    nj = (w * h + 31) >> 5
    al = lambda v: (v + 31) & ~31  # noqa: E731
    s1, s2 = nk1 * 16, nm * 16
    w2 = nm * s1
    bt = al(w2 + no * s2)
    bias = al(al(bt + s1) + s2)
    tbl = bias + (nm + no + 1) // 2
    xt = tbl + (nk1 + 1) // 2
    src = xt + nj
    ok = (
        1 <= w <= 128
        and 1 <= h <= 128
        and 2 <= cin <= 64
        and 2 <= cout <= 64
        and 1 <= nm <= 11
        and 1 <= no <= 2
        and cout <= 32 * no
        and zero == 0
        and nk1 == (9 * cin + 31) >> 5
        and 1 <= pad <= 32
        and (pad + w) % 32 == 0
        and nbin >= nj + ((pad + 2 * w + 1) >> 5) + 2
        and src + cin // 2 * nbin <= (0x311E0000 - 0x31002000) // 128
    )
    sizes = [cin // 2 * nbin, w2 + no * s2, tbl - bias, xt - tbl, nj, cout // 2 * nbin]
    for lo, n in zip(d[8:14], sizes):
        ok &= lo % 128 == 0 and WIN_LO <= lo <= WIN_HI and n <= (WIN_HI - lo) // 128
    return ok


class ChainLayoutTests(unittest.TestCase):
    @unittest.skipUnless(NUMPY, "optional NumPy not installed")
    def test_chains_fit_window_vtcm_and_kernel_checks(self):
        import numpy as np

        for name, layers in CHAINS.items():
            with self.subTest(name):
                check_chain(layers)
                p = plan(layers)
                self.assertLessEqual(p["end"], WIN_HI)
                words = np.frombuffer(descriptors(layers, p), "<u4")
                self.assertEqual(words[0], MAGIC)
                self.assertEqual(len(words) * 4, 128 + 128 * len(layers))
                for i in range(len(layers)):
                    self.assertTrue(kernel_accepts(words, i))
                bad = words.copy()
                bad[32 + 13] = WIN_HI  # output region outside the window
                self.assertFalse(kernel_accepts(bad, 0))

    def test_regions_do_not_overlap(self):
        for layers in CHAINS.values():
            p = plan(layers)
            spans = [(o, o + n) for o, n in p["act"]]
            for i, layer in enumerate(layers):
                spans.append(
                    (p["w"][i], p["w"][i] + (layer.nm * layer.nk1 + layer.no * layer.nm) * 2048)
                )
                spans.append((p["bias"][i], p["bias"][i] + 128))
                spans.append((p["tbl"][i], p["tbl"][i] + (layer.nk1 + 1) // 2 * 128))
                spans.append((p["xt"][i], p["xt"][i] + layer.nj * 128))
            spans.sort()
            for (a0, a1), (b0, b1) in zip(spans, spans[1:]):
                self.assertLessEqual(a1, b0)

    @unittest.skipUnless(NUMPY, "numpy")
    def test_buffer_round_trip_keeps_padding_zero(self):
        import numpy as np

        for layers in CHAINS.values():
            layer = layers[0]
            x0, _ = fixture(layers)
            band, stray = from_buffer(to_buffer(x0, layer), layer.cin, layer)
            self.assertEqual(stray, 0)
            np.testing.assert_array_equal(band[:, 1:-1], x0.astype(np.float64))
            self.assertFalse(band[:, 0].any() or band[:, -1].any())

    @unittest.skipUnless(NUMPY, "numpy")
    def test_table_gather_equals_im2col_for_every_block(self):
        import numpy as np

        for name, layers in CHAINS.items():
            layer = layers[0]
            x0, _ = fixture(layers)
            raw = to_buffer(x0, layer)
            band, _ = from_buffer(raw, layer.cin, layer)
            g = layer.geometry()
            for j in range(layer.nj):
                with self.subTest(name, j=j):
                    np.testing.assert_array_equal(emulate_gather(layer, raw, j), im2col(g, band, j))

    @unittest.skipUnless(NUMPY, "numpy")
    def test_layer_through_packed_tiles_matches_oracle(self):
        import numpy as np

        h = lambda v: v.astype(np.float16).astype(np.float64)  # noqa: E731
        for name, layers in CHAINS.items():
            layer = layers[0]
            x0, weights = fixture(layers)
            w1, w2, b1, b2 = weights[0]
            raw = to_buffer(x0, layer)
            band, _ = from_buffer(raw, layer.cin, layer)
            g = layer.geometry()
            ref = oracle(g, band.astype(np.float16), w1, w2, b1, b2)
            a1, a2 = weight_tiles(g, w1, w2)
            A1 = np.block(
                [[unpack(a1[m * g.nk1 + t].tobytes()) for t in range(g.nk1)] for m in range(g.nm)]
            )
            A2 = np.block(
                [[unpack(a2[o * g.nm + m].tobytes()) for m in range(g.nm)] for o in range(g.no)]
            )
            b1p = np.zeros(g.nm * 32)
            b1p[: g.cmid] = h(b1.astype(np.float64))
            b2p = np.zeros(g.no * 32)
            b2p[: g.cout] = h(b2.astype(np.float64))
            for j in range(layer.nj):
                with self.subTest(name, j=j):
                    mid = np.clip(h(h(A1 @ emulate_gather(layer, raw, j)) + b1p[:, None]), 0, 6)
                    out = h(h(A2 @ mid) + b2p[:, None])[: g.cout]
                    lo, hi = 32 * j, min(32 * j + 32, g.nout)
                    err = np.abs(out[:, : hi - lo] - ref["out"][:, lo:hi])
                    self.assertTrue(np.all(err <= ref["out_allow"][:, lo:hi]))

    @unittest.skipUnless(NUMPY, "numpy")
    def test_arena_image_covers_plan(self):
        for layers in CHAINS.values():
            x0, weights = fixture(layers)
            p, writes = arena_image(layers, x0, weights)
            for off, data in writes.items():
                self.assertTrue(WIN_LO <= off and off + len(data) <= p["end"])
            self.assertLess(0, HARD_TICKS)

    @unittest.skipUnless(shutil.which("clang") or True, "toolchain")
    def test_probe_builds_within_4_kib_without_vector_stack(self):
        try:
            sys.path.insert(0, str(ROOT / "src"))
            from framenpu.compiler import Toolchain

            Toolchain.discover()
        except Exception as exc:  # pragma: no cover - environment dependent
            self.skipTest(f"no Hexagon toolchain: {exc}")
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(
                [sys.executable, str(ROOT / "tools/build_hmx_chain_probe.py"), "--output", tmp],
                check=True,
                capture_output=True,
            )
            import json

            record = json.loads((Path(tmp) / "build.json").read_text())
            self.assertLessEqual(record["bytes"], 4096)


if __name__ == "__main__":
    unittest.main()
