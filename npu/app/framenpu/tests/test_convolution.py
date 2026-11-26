"""Strict binary/DAG preflight, ncnn layout and bounded patch-lowering checks."""

from pathlib import Path
import math
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tools"))
from framenpu import InvalidModel, Job, UnsupportedOperator, RecoveryRequired
from framenpu.convolution import load_landmark_graph, ResidentConvolution
from analyze_conv_reference import full_reference_limits
from test_resident_gemm import Emulated, session


def fixture(
    directory,
    *,
    attributes="",
    shape=(3, 4, 4),
    n=2,
    kh=3,
    kw=3,
    weights=None,
    bias=None,
    dtype="f16",
):
    k = shape[0] * kh * kw
    if weights is None:
        weights = [(i - 10) / 32 for i in range(n * k)]
    if bias is None:
        bias = [0.25] * n
    param = Path(directory) / "model.param"
    binary = Path(directory) / "model.bin"
    param.write_text(
        f"7767517\n2 2\nInput input 0 1 input\nConvolution conv 1 1 input output 0={n} 1={kw} 11={kh} 3=2 13=2 15=1 16=1 5=1 6={n * k} {attributes}\n"
    )
    raw = struct.pack("<I", 0x01306B47 if dtype == "f16" else 0) + struct.pack(
        f"<{len(weights)}{'e' if dtype == 'f16' else 'f'}", *weights
    )
    raw = raw.ljust((len(raw) + 3) & ~3, b"\0") + struct.pack(f"<{n}f", *bias)
    binary.write_bytes(raw)
    return param, binary


class ConvolutionPreflightTests(unittest.TestCase):
    def test_checker_exceptions_retain_payload_and_power_before_reclamation(self):
        for error in (TypeError("checker bug"), AssertionError("mismatch"), KeyboardInterrupt()):
            transport = Emulated()
            owner = session(transport).__enter__()
            before = transport.events.count("power_off")

            def fail(output):
                raise error

            with self.subTest(error=type(error).__name__), self.assertRaises(RecoveryRequired):
                owner.run([[0.0] * 32 for _ in range(32)], _check=fail)
            owner.close()
            self.assertTrue(transport.poisoned)
            self.assertEqual(transport.events.count("power_off"), before)
            self.assertIn("abandon", transport.events)

    def test_heldout_errors_cannot_widen_frozen_full_reference_limits(self):
        rows = [
            {
                "layer": "conv",
                "split": "calibration",
                "input_quantization_vs_full": {"max_abs": 0.002, "rms": 0.0002},
                "ideal_fp16_store_vs_quantized_ncnn": {"max_abs": 0.001, "rms": 0.0001},
            }
        ]
        baseline = full_reference_limits(rows, "conv")
        rows.append(
            {
                "layer": "conv",
                "split": "heldout",
                "input_quantization_vs_full": {"max_abs": 1000, "rms": 1000},
                "ideal_fp16_store_vs_quantized_ncnn": {"max_abs": 1000, "rms": 1000},
            }
        )
        self.assertEqual(full_reference_limits(rows, "conv"), baseline)
        self.assertAlmostEqual(baseline["full_max_abs"], 0.0038)
        self.assertAlmostEqual(baseline["full_rms"], 0.00038)

    def test_hardware_runner_requires_explicit_opt_in_before_loading_inputs(self):
        result = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tests/hardware_convolution.py"),
                "--probe",
                "unused",
                "--model",
                "unused",
                "--corpus",
                "unused",
                "--output",
                "unused",
            ],
            cwd="/tmp",
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("--run-on-device is required", result.stderr)

    def test_weight_transposition_bias_and_asymmetric_padding(self):
        with tempfile.TemporaryDirectory() as d:
            p, b = fixture(d)
            graph = load_landmark_graph(p, b, input_shape=(3, 4, 4))
            layer = graph.convolution("conv")
            self.assertEqual(layer.output_shape, (2, 2, 2))
            self.assertEqual(layer.padding, (0, 1, 0, 1))
            self.assertEqual(layer.weights(graph.raw)[1], [-9 / 32, 18 / 32])
            self.assertEqual(layer.bias(graph.raw), (0.25, 0.25))
            self.assertEqual(layer.segment, (0, len(graph.raw)))

    def test_patch_order_and_right_bottom_zero_padding(self):
        with tempfile.TemporaryDirectory() as d:
            p, b = fixture(d)
            layer = load_landmark_graph(p, b, input_shape=(3, 4, 4)).convolution("conv")
            patches = layer.patches(list(range(48)), 0)
            self.assertEqual(len(patches), 4)
            self.assertEqual(len(patches[0]), 27)
            self.assertEqual(patches[0][:9], [0, 1, 2, 4, 5, 6, 8, 9, 10])
            self.assertEqual(patches[-1][:9], [10, 11, 0, 14, 15, 0, 0, 0, 0])
            self.assertEqual(patches[0][9], 16)

    def test_fp16_odd_count_alignment_and_fp32_tag(self):
        for dtype in ("f16", "f32"):
            with tempfile.TemporaryDirectory() as d:
                p, b = fixture(
                    d, shape=(1, 1, 1), n=1, kh=1, kw=1, weights=[2], bias=[3], dtype=dtype
                )
                layer = load_landmark_graph(p, b, input_shape=(1, 1, 1)).convolution("conv")
                self.assertEqual(layer.weight_dtype, dtype)
                self.assertEqual(layer.weight_offset, 4)
                self.assertEqual(layer.bias_offset, 8)

    def test_truncation_trailing_data_bad_tag_and_nonfinite_values_fail(self):
        with tempfile.TemporaryDirectory() as d:
            p, b = fixture(d)
            raw = b.read_bytes()
            mutations = [
                raw[:-1],
                raw + b"\0",
                struct.pack("<I", 0x000D4B38) + raw[4:],
                raw[:4] + struct.pack("<H", 0x7C00) + raw[6:],
                raw[:-4] + struct.pack("<f", math.inf),
            ]
            for value in mutations:
                b.write_bytes(value)
                with self.subTest(length=len(value)), self.assertRaises(InvalidModel):
                    load_landmark_graph(p, b, input_shape=(3, 4, 4))

    def test_invalid_connectivity_counts_and_unsupported_semantics_fail(self):
        with tempfile.TemporaryDirectory() as d:
            p, b = fixture(d)
            text = p.read_text()
            for value in (
                text.replace("2 2\n", "3 2\n"),
                text.replace("2 2\n", "2 3\n"),
                text.replace("input output", "missing output"),
                text.replace("6=54", "6=53"),
                text.replace("Convolution conv", "Custom conv"),
                text.replace("15=1", "4=-233"),
                text.replace("5=1", "5=2"),
                text + "Split duplicate 1 1 output output\n",
            ):
                p.write_text(value)
                with self.subTest(text=value), self.assertRaises(InvalidModel):
                    load_landmark_graph(p, b, input_shape=(3, 4, 4))

    def test_activation_arrays_unknown_attributes_and_dynamic_weights_fail(self):
        for attributes in (
            "9=3 -23310=2,0,6",
            "9=3 -23310=1,0,6",
            "9=3 -23310=2,0,7",
            "9=2",
            "19=1",
            "99=1",
            "0=2",
            "9=3 -23310=2,0,nan",
        ):
            with tempfile.TemporaryDirectory() as d:
                p, b = fixture(d, attributes=attributes)
                if attributes == "9=3 -23310=2,0,6":
                    self.assertEqual(
                        load_landmark_graph(p, b, input_shape=(3, 4, 4))
                        .convolution("conv")
                        .activation,
                        "clip",
                    )
                else:
                    with self.subTest(attributes=attributes), self.assertRaises(InvalidModel):
                        load_landmark_graph(p, b, input_shape=(3, 4, 4))

    def test_bad_patch_input_range_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            p, b = fixture(d)
            layer = load_landmark_graph(p, b, input_shape=(3, 4, 4)).convolution("conv")
            for data, start, rows in (
                ([], 0, 32),
                ([0] * 48, -1, 32),
                ([0] * 48, 4, 32),
                ([0] * 48, 0, 33),
                ([0] * 48, True, 1),
            ):
                with self.assertRaises(InvalidModel):
                    layer.patches(data, start, rows)

    def test_invalid_input_is_rejected_before_a_device_is_opened(self):
        with tempfile.TemporaryDirectory() as d:
            p, b = fixture(d)
            graph = load_landmark_graph(p, b, input_shape=(3, 4, 4))
            factory = Mock(side_effect=AssertionError("device opened"))
            with ResidentConvolution(
                Job(bytes(2048), "proof", True, True), graph, "conv", _device_factory=factory
            ) as conv:
                for values in ([], [math.nan] * 48, [70000] * 48):
                    with self.assertRaises(InvalidModel):
                        conv.run(values)
            factory.assert_not_called()

    def test_layer_width_depth_or_activation_outside_proof_is_rejected(self):
        for n, shape, attr in ((33, (3, 4, 4), ""), (2, (129, 4, 4), ""), (2, (3, 4, 4), "9=4")):
            with tempfile.TemporaryDirectory() as d:
                p, b = fixture(d, n=n, shape=shape, attributes=attr)
                graph = load_landmark_graph(p, b, input_shape=shape)
                with self.assertRaises(UnsupportedOperator):
                    ResidentConvolution(Job(bytes(2048), "proof", True, True), graph, "conv")

    def test_partial_spatial_tiles_keep_bias_on_unused_rows_and_zero_channel_tails(self):
        class Tile:
            def __init__(self, job, weights, **options):
                self.k = len(weights)
                self.weights = weights
                self.bias = options["_bias"]
                self.clip = options["_clip"]
                self.opens = 0

            def __enter__(self):
                self.opens += 1
                return self

            def close(self):
                pass

            def run(self, inputs, **kwargs):
                output = [
                    [
                        sum(a * b for a, b in zip(row, column)) + bias
                        for column, bias in zip(zip(*self.weights), self.bias)
                    ]
                    for row in inputs
                ]
                if self.clip:
                    output = [[min(6, max(0, v)) for v in row] for row in output]
                kwargs["_check"](struct.pack("<1024f", *(v for row in output for v in row)))
                return output

        for clip in (False, True):
            with tempfile.TemporaryDirectory() as d:
                p, b = fixture(
                    d,
                    shape=(1, 1, 5),
                    n=2,
                    kh=1,
                    kw=1,
                    weights=[-1, 2],
                    bias=[0.5, -1],
                    attributes="9=3 -23310=2,0,6" if clip else "",
                )
                graph = load_landmark_graph(p, b, input_shape=(1, 1, 5))
                with patch("framenpu.convolution.ResidentGemm", Tile):
                    with ResidentConvolution(
                        Job(bytes(2048), "proof", True, True), graph, "conv"
                    ) as conv:
                        actual = list(conv.run([1, 2, 3, 4, 5]))
                        expected = [-0.5, -2.5, -4.5, 1, 5, 9]
                        if clip:
                            expected = [min(6, max(0, v)) for v in expected]
                        self.assertEqual(actual, expected)
                        self.assertEqual(conv.gemm.opens, 1)


if __name__ == "__main__":
    unittest.main()
