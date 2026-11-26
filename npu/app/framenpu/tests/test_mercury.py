"""Mercury keypoint support: ONNX wire reader, lowering semantics and numerics.

The pinned model is not redistributed. Tests that need it read
FNPU_MERCURY_MODEL (path to grayscale_keypoint_jan18.onnx) and skip otherwise.
"""

import importlib.util
import os
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / n) for n in ("src", "tools")]
from framenpu import mercury, onnx_graph
from framenpu.errors import InvalidModel, UnsupportedOperator

HAVE_NUMPY = importlib.util.find_spec("numpy") is not None
MODEL = os.environ.get("FNPU_MERCURY_MODEL")


# ---------------------------------------------------------------- tiny protobuf writer


def varint(n):
    n &= (1 << 64) - 1
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def fld(num, value):
    if isinstance(value, int):
        return varint(num << 3) + varint(value)
    return varint(num << 3 | 2) + varint(len(value)) + value


def tensor(name, dims, dtype=1, raw=None, floats=None):
    body = b"".join(fld(1, d) for d in dims) + fld(2, dtype) + fld(8, name.encode())
    if raw is not None:
        body += fld(9, raw)
    if floats is not None:
        body += fld(4, struct.pack(f"<{len(floats)}f", *floats))
    return body


def attr_ints(name, values):
    packed = b"".join(varint(v) for v in values)
    return fld(1, name.encode()) + fld(20, 7) + fld(8, packed)


def node(op, inputs, outputs, attrs=(), name="n", domain=None):
    body = b"".join(fld(1, i.encode()) for i in inputs)
    body += b"".join(fld(2, o.encode()) for o in outputs)
    body += fld(3, name.encode()) + fld(4, op.encode())
    body += b"".join(fld(5, a) for a in attrs)
    if domain is not None:
        body += fld(7, domain.encode())
    return body


def value_info(name, dims):
    shape = b"".join(fld(1, fld(1, d)) for d in dims)
    return fld(1, name.encode()) + fld(2, fld(1, fld(1, 1) + fld(2, shape)))


def model(nodes, inits=(), inputs=(), outputs=(), opset=13):
    graph = b"".join(fld(1, n) for n in nodes)
    graph += b"".join(fld(5, t) for t in inits)
    graph += b"".join(fld(11, v) for v in inputs) + b"".join(fld(12, v) for v in outputs)
    return fld(1, 7) + fld(2, b"test") + fld(7, graph) + fld(8, fld(1, b"") + fld(2, opset))


class OnnxReaderTests(unittest.TestCase):
    def sample(self, **kw):
        w = struct.pack("<4f", 1.0, -2.0, 0.5, 3.0)
        return model(
            [node("Conv", ["x", "w"], ["y"], [attr_ints("kernel_shape", [1, 1])], **kw)],
            [tensor("w", [2, 2, 1, 1], raw=w), tensor("b", [2], floats=[0.25, -1.0])],
            [value_info("x", [1, 2, 3, 3]), value_info("w", [2, 2, 1, 1])],
            [value_info("y", [1, 2, 3, 3])],
        )

    def test_reads_nodes_attributes_tensors_and_signature(self):
        g = onnx_graph.parse(self.sample())
        self.assertEqual(g.opset, 13)
        self.assertEqual(g.producer, "test")
        self.assertEqual([(v.name, v.shape) for v in g.inputs], [("x", (1, 2, 3, 3))])
        self.assertEqual(g.outputs[0].shape, (1, 2, 3, 3))
        (n,) = g.nodes
        self.assertEqual((n.op_type, n.inputs, n.outputs), ("Conv", ("x", "w"), ("y",)))
        self.assertEqual(n.attrs, {"kernel_shape": (1, 1)})
        self.assertEqual(g.tensor("w").dims, (2, 2, 1, 1))
        self.assertEqual(g.tensor("b").data, struct.pack("<2f", 0.25, -1.0))
        self.assertEqual(len(g.sha256), 64)

    def test_negative_varints_and_unpacked_ints(self):
        a = fld(1, b"pads") + fld(20, 7) + fld(8, -1) + fld(8, 2)
        g = onnx_graph.parse(model([node("Pad", ["x"], ["y"], [a])]))
        self.assertEqual(g.nodes[0].attrs["pads"], (-1, 2))

    def test_rejects_unsupported_and_malformed_models(self):
        cases = {
            "domain": self.sample(domain="com.microsoft"),
            "truncated": self.sample()[:-3],
            "no graph": fld(1, 7) + fld(8, fld(2, 13)),
            "wire type": varint(1 << 3 | 3),
            "size": model([], [tensor("t", [3], raw=b"\0" * 8)]),
            "external": model([], [tensor("t", [1], raw=b"\0" * 4) + fld(14, 1)]),
            "dtype": model([], [tensor("t", [1], dtype=8, raw=b"\0")]),
        }
        for label, data in cases.items():
            with self.subTest(label), self.assertRaises(InvalidModel):
                onnx_graph.parse(data)

    def test_float16_typed_storage_and_negative_dims(self):
        t = tensor("h", [2], dtype=10) + fld(5, varint(0x3C00) + varint(0xC000))
        g = onnx_graph.parse(model([], [t]))
        self.assertEqual(g.tensor("h").data, struct.pack("<2H", 0x3C00, 0xC000))
        with self.assertRaises(InvalidModel):
            onnx_graph.parse(model([], [tensor("n", [-1], raw=b"")]))

    def test_unknown_constant_tensor_name_is_refused(self):
        with self.assertRaises(InvalidModel):
            onnx_graph.parse(self.sample()).tensor("missing")


@unittest.skipUnless(HAVE_NUMPY, "NumPy required")
class MercuryNumericsTests(unittest.TestCase):
    def conv(self, **params):
        import numpy as np

        w = params.pop("weight")
        defaults = dict(bias=np.zeros(w.shape[0], np.float32), group=1, act=None)
        defaults.update(params)
        defaults["weight"] = w
        return defaults

    def test_depthwise_stride2_symmetric_padding_keeps_even_phase(self):
        import numpy as np

        rng = np.random.default_rng(3)
        x = rng.standard_normal((2, 8, 8)).astype(np.float32)
        w = rng.standard_normal((2, 1, 3, 3)).astype(np.float32)
        op = mercury.Op(
            "dw",
            "conv",
            ("x",),
            "y",
            self.conv(weight=w, group=2, pads=(1, 1, 1, 1), stride=(2, 2)),
            (2, 4, 4),
        )
        got = mercury._conv(op, x, None, "fp32")
        pad = np.pad(x, ((0, 0), (1, 1), (1, 1)))
        want = np.zeros((2, 4, 4))
        for c in range(2):
            for i in range(4):
                for j in range(4):
                    want[c, i, j] = (pad[c, 2 * i : 2 * i + 3, 2 * j : 2 * j + 3] * w[c, 0]).sum()
        np.testing.assert_allclose(got, want, rtol=1e-5, atol=1e-5)
        # Stride-1 result sampled at even positions: phase 0, not the teacher's phase 1.
        full = mercury._conv(
            mercury.Op(
                "d1",
                "conv",
                ("x",),
                "y",
                self.conv(weight=w, group=2, pads=(1, 1, 1, 1), stride=(1, 1)),
                (2, 8, 8),
            ),
            x,
            None,
            "fp32",
        )
        np.testing.assert_allclose(got, full[:, ::2, ::2], rtol=1e-5, atol=1e-5)

    def test_deconv2x2_matches_scatter_definition(self):
        import numpy as np

        rng = np.random.default_rng(4)
        x = rng.standard_normal((3, 2, 3)).astype(np.float32)
        w = rng.standard_normal((3, 2, 2, 2)).astype(np.float32)
        b = rng.standard_normal(2).astype(np.float32)
        op = mercury.Op("t", "deconv2x2", ("x",), "y", {"weight": w, "bias": b}, (2, 4, 6))
        got = mercury._deconv(op, x, "fp32")
        want = np.tile(b[:, None, None], (1, 4, 6)).astype(np.float64)
        for ci in range(3):
            for i in range(2):
                for j in range(3):
                    want[:, 2 * i : 2 * i + 2, 2 * j : 2 * j + 2] += x[ci, i, j] * w[ci]
        np.testing.assert_allclose(got, want, rtol=1e-5, atol=1e-5)
        fp16 = mercury._deconv(op, x, "fp16")
        self.assertTrue(np.array_equal(fp16, fp16.astype(np.float16).astype(np.float32)))

    def test_dense_conv_fp16_contract_rounds_storage(self):
        import numpy as np

        rng = np.random.default_rng(5)
        x = mercury._f16(rng.standard_normal((4, 5, 5)))
        w = rng.standard_normal((3, 4, 3, 3)).astype(np.float32)
        op = mercury.Op(
            "c",
            "conv",
            ("x",),
            "y",
            self.conv(weight=w, pads=(2, 2, 2, 2), stride=(1, 1), act="relu6"),
            (3, 7, 7),
        )
        exact = mercury._conv(op, x, None, "fp32")
        dev = mercury._conv(op, x, None, "fp16")
        self.assertEqual(exact.shape, (3, 7, 7))
        self.assertTrue(np.array_equal(dev, mercury._f16(dev)))
        self.assertLessEqual(float(dev.max()), 6.0)
        self.assertLess(float(np.abs(dev - exact).max()), 0.05)

    def test_decode_hint_round_trip_and_normalize(self):
        import numpy as np

        hm = np.zeros((1, 21, 22, 22), np.float32)
        for j in range(21):
            hm[0, j, j % 22, (3 * j) % 22] = 1.0
        out = {
            "heatmap_xy": hm,
            "heatmap_depth": np.zeros((1, 21, 22), np.float32),
            "scalar_extras": np.zeros((1, 8), np.float32),
            "curls": np.arange(10, dtype=np.float32)[None],
        }
        d = mercury.decode(out)
        np.testing.assert_allclose(d["grid"][:, 0], [(3 * j) % 22 + 0.5 for j in range(21)])
        np.testing.assert_allclose(d["crop_px"], d["grid"] * 128 / 22)
        self.assertEqual(d["is_hand"], 0.5)
        np.testing.assert_allclose(d["curl_var"], np.arange(5, 10) + 0.01)
        hint = mercury.hint_from_grid(d["grid"]).reshape(21, 2)
        np.testing.assert_allclose((hint[:, 0] + 1) * 11, d["grid"][:, 0], rtol=1e-6)
        np.testing.assert_allclose((1 - hint[:, 1]) * 11, d["grid"][:, 1], rtol=1e-6)
        n = mercury.normalize(np.arange(256, dtype=np.uint8).reshape(16, 16))
        self.assertAlmostEqual(float(n.mean()), 0.5, places=5)
        self.assertAlmostEqual(float(n.std()), 0.25, places=5)
        self.assertIsNone(mercury.normalize(np.full((4, 4), 7, np.uint8)))

    def test_run_rejects_non_finite_inputs(self):
        import numpy as np

        feeds = {
            "inputImg": np.full((128, 128), np.nan, np.float32),
            "lastKeypoints": np.zeros(42, np.float32),
            "useLastKeypoints": np.zeros(1, np.float32),
        }
        with self.assertRaises(ValueError):
            mercury.run(mercury.KeypointModel([], "", False, 0), feeds)


@unittest.skipUnless(HAVE_NUMPY and MODEL, "NumPy and FNPU_MERCURY_MODEL required")
class PinnedMercuryModelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.graph = onnx_graph.load(MODEL)
        cls.model = mercury.lower(cls.graph)

    def test_pinned_structure_and_inventory(self):
        from collections import Counter

        self.assertEqual(self.graph.sha256, mercury.KEYPOINT_SHA256)
        self.assertEqual(self.model.params, 986168)
        kinds = Counter((mercury.unit(op), op.conv_class or op.kind) for op in self.model.ops)
        self.assertEqual(kinds[("hmx", "pointwise")], 23)
        self.assertEqual(kinds[("hvx", "depthwise")], 18)
        self.assertEqual(kinds[("hmx", "dense")], 3)
        self.assertEqual(kinds[("hmx", "deconv2x2")], 1)
        self.assertEqual(kinds[("fp32", "dense")], 5)
        fused = sum(1 for op in self.model.ops if len(op.inputs) > 1 and op.kind == "conv")
        self.assertEqual(fused, sum(1 for n in self.graph.nodes if n.op_type == "Add"))
        self.assertEqual(fused, 12)
        inv = mercury.inventory(self.model)
        conv_macs = sum(r["macs"] for r in inv if r["kind"] in ("conv", "deconv2x2"))
        self.assertEqual(conv_macs, 121865736 + 63 * 42 * 4 * 10 * 10)  # Conv + ConvTranspose
        pools = [op.params["kernel"] for op in self.model.ops if op.kind == "avgpool"]
        self.assertEqual(sorted(pools), [(3, 3), (3, 3), (6, 6)])

    def test_unpinned_model_is_refused_unless_explicit(self):
        import dataclasses

        other = dataclasses.replace(self.graph, sha256="0" * 64)
        with self.assertRaises(InvalidModel):
            mercury.lower(other)
        self.assertFalse(mercury.lower(other, require_pin=False).pinned)

    def test_unsupported_patterns_are_refused(self):
        import dataclasses

        nodes = list(self.graph.nodes)
        i = next(k for k, n in enumerate(nodes) if n.op_type == "AveragePool")
        bad = dataclasses.replace(nodes[i], attrs=dict(nodes[i].attrs, ceil_mode=1))
        g = dataclasses.replace(self.graph, nodes=nodes[:i] + [bad] + nodes[i + 1 :])
        with self.assertRaises(UnsupportedOperator):
            mercury.lower(g)
        j = next(k for k, n in enumerate(nodes) if n.op_type == "Concat")
        bad = dataclasses.replace(nodes[j], op_type="Sum")
        g = dataclasses.replace(self.graph, nodes=nodes[:j] + [bad] + nodes[j + 1 :])
        with self.assertRaises(UnsupportedOperator):
            mercury.lower(g)

    def test_fp16_contract_stays_close_to_fp32_on_a_structured_input(self):
        import numpy as np

        yy, xx = np.mgrid[0:128, 0:128]
        img = mercury.normalize(
            ((np.sin(xx / 7.0) + np.cos(yy / 11.0)) * 60 + 128).astype(np.uint8)
        )
        feeds = {
            "inputImg": img,
            "lastKeypoints": np.zeros(42, np.float32),
            "useLastKeypoints": np.zeros(1, np.float32),
        }
        exact = mercury.run(self.model, feeds, "fp32")
        dev = mercury.run(self.model, feeds, "fp16")
        for k, shape in mercury.OUTPUTS.items():
            self.assertEqual(exact[k].shape, shape)
            self.assertTrue(np.isfinite(dev[k]).all())
        self.assertLess(
            abs(mercury.decode(dev)["is_hand"] - mercury.decode(exact)["is_hand"]), 0.01
        )


if __name__ == "__main__":
    unittest.main()
