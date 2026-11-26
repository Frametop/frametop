"""Graph alias lifetimes, chunk/tail coverage and failure-before-reclamation."""

from dataclasses import replace
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tools")]
from framenpu import Job, RecoveryRequired, UnsupportedOperator
from framenpu.job import JobResult
from framenpu.convolution import load_landmark_graph
from framenpu.landmark import PARAM_SHA, WEIGHTS_SHA, StreamingLandmark, tensor_lifetimes
from landmark_reference import frozen_limits


def fp32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


class Emulator:
    def __init__(self, **kwargs):
        self.closed = False
        self.commands = []
        self.corrupt = False

    def __enter__(self):
        return self

    def close(self):
        self.closed = True

    def _submit(self, job, data, *, opcode, params, output_size, _completion_validator, **kwargs):
        self.commands.append((opcode, params, len(data), output_size))
        if opcode == 25:
            source = struct.unpack("<32f", data)
            values = [fp32(1 / (1 + math.exp(-v))) for v in source]
        elif opcode == 20:
            depth = params[0]
            k = depth * 32
            a = struct.unpack_from(f"<{k * 32}e", data)
            b = struct.unpack_from(f"<{k * 32}e", data, k * 64)
            bias = struct.unpack_from("<32f", data, k * 128)
            values = []
            for r in range(32):
                for c in range(32):
                    value = sum(
                        a[(t // 32) * 1024 + r * 32 + t % 32] * b[t * 32 + c] for t in range(k)
                    )
                    value = fp32(struct.unpack("<e", struct.pack("<e", value))[0] + bias[c])
                    values.append(min(6, max(0, value)) if params[1] else value)
        else:
            vectors, taps, clip = params
            size = vectors * 32
            inputs = struct.unpack(f"<{len(data) // 4}f", data)
            values = []
            for i in range(size):
                if opcode == 22:
                    value = fp32(inputs[i] + inputs[size + i])
                else:
                    value = inputs[2 * taps * size + i] if opcode == 21 else 0.0
                    for t in range(taps):
                        v = inputs[t * size + i]
                        if opcode == 21:
                            v = fp32(v * inputs[(taps + t) * size + i])
                        value = (
                            max(value, v)
                            if opcode == 23 and t
                            else v
                            if opcode == 23
                            else fp32(value + v)
                        )
                    if opcode == 24:
                        value = fp32(value * inputs[taps * size + i])
                values.append(min(6, max(0, value)) if clip else value)
        raw = struct.pack(f"<{len(values)}f", *values) + bytes(128)
        if self.corrupt:
            raw = raw[:-128] + b"\1" + bytes(127)
        rows = []
        for i, ssr in enumerate((0x81840200, 0x89840200, 0x91840200)):
            row = [0] * 16
            row[0] = 123
            row[1] = i
            row[7] = ssr
            if i == 0:
                row[8] = 0x87000000
                row[10] = 1
                row[12] = 101
            rows.append(tuple(row))
        result = JobResult(raw, 1.0, tuple(rows))
        _completion_validator(result)
        return result


class LandmarkTests(unittest.TestCase):
    def test_full_depth_and_two_output_tiles_reach_all_four_heads(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "m.param"
            b = Path(d) / "m.bin"
            lines = [
                "Input input 0 1 in0",
                "Squeeze squeeze 1 1 in0 vector -23303=2,1,2",
                "Split heads 1 4 vector a b c d",
            ]
            raw = b""
            k = 704
            expected = {}
            source = [(t % 11 - 5) / 16 for t in range(k)]
            for index, (bottom, top, n, activation) in enumerate(
                (
                    ("a", "out0", 33, 0),
                    ("b", "out1", 1, 4),
                    ("c", "out2", 1, 4),
                    ("d", "out3", 33, 0),
                )
            ):
                lines.append(
                    f"InnerProduct head{index} 1 1 {bottom} {top} 0={n} 1=1 2={k * n} 9={activation}"
                )
                weights = [
                    float(t == (c * 37 + index * 11 + 5) % k) for c in range(n) for t in range(k)
                ]
                raw += (
                    struct.pack("<I", 0x01306B47)
                    + struct.pack(f"<{k * n}e", *weights)
                    + struct.pack(f"<{n}f", *([0.25] * n))
                )
                bvalues = [source[(c * 37 + index * 11 + 5) % k] + 0.25 for c in range(n)]
                expected[top] = (
                    [fp32(1 / (1 + math.exp(-v))) for v in bvalues] if activation else bvalues
                )
            p.write_text("7767517\n7 10\n" + "\n".join(lines) + "\n")
            b.write_bytes(raw)
            g = replace(
                load_landmark_graph(p, b, input_shape=(k, 1, 1)),
                param_sha256=PARAM_SHA,
                weights_sha256=WEIGHTS_SHA,
            )
            device = Emulator()
            with StreamingLandmark(g, self.jobs(), _device_factory=lambda **kw: device) as owner:
                output = owner.run(source)
            self.assertEqual({key: list(value) for key, value in output.items()}, expected)
            self.assertEqual(sum(c[0] == 20 for c in device.commands), 6)
            self.assertEqual(sum(c[0] == 25 for c in device.commands), 2)
            self.assertTrue(
                all(c[1][0] == 22 and c[2] == 90240 for c in device.commands if c[0] == 20)
            )

    def graph(self, directory):
        p = Path(directory) / "m.param"
        b = Path(directory) / "m.bin"
        # One non-tile-aligned spatial tensor, residual and four aliases to it.
        p.write_text(
            "7767517\n8 12\nInput input 0 1 in0\n"
            "Convolution conv 1 1 in0 x 0=1 1=1 5=1 6=1\n"
            "Split branch 1 2 x a b\n"
            "ConvolutionDepthWise dw 1 1 b c 0=1 1=3 4=1 5=1 6=9 7=1\n"
            "BinaryOp add 2 1 c a d 0=0\n"
            "Pooling pool 1 1 d e 0=1 4=1\n"
            "Reshape reshape 1 1 e f 0=1 1=1 2=-1\n"
            "Split outputs 1 4 f out0 out1 out2 out3\n"
        )
        weights = struct.pack("<Ie2xf", 0x01306B47, 2.0, 0.25)
        weights += struct.pack("<I9e2xf", 0x01306B47, *([0.125] * 9), -0.5)
        b.write_bytes(weights)
        # Pins are synthesized only inside this fixture; public execution rejects
        # all unpinned files before touching a device.
        g = load_landmark_graph(p, b, input_shape=(1, 5, 7))
        return replace(g, param_sha256=PARAM_SHA, weights_sha256=WEIGHTS_SHA)

    def jobs(self):
        return {k: Job(b"\0" * 4, k, True, True) for k in ("hmx", "hvx", "sigmoid")}

    def test_unpinned_models_and_bad_jobs_are_rejected_offline(self):
        with tempfile.TemporaryDirectory() as d:
            g = self.graph(d)
            with self.assertRaises(UnsupportedOperator):
                StreamingLandmark(replace(g, weights_sha256="0" * 64), self.jobs())
            with self.assertRaises(ValueError):
                StreamingLandmark(g, {})

    def test_graph_aliases_extend_lifetimes_and_run_without_copy(self):
        with tempfile.TemporaryDirectory() as d:
            graph = self.graph(d)
            plan = tensor_lifetimes(graph)
            self.assertEqual(plan["roots"]["a"], "x")
            self.assertEqual(plan["allocations"]["x"]["last"], 4)
            self.assertEqual(plan["allocations"]["e"]["last"], 8)
            device = Emulator()
            checks = []

            def validator(s, inputs):
                def check(indices, values, final, phase):
                    checks.append((s["name"], len(values), final))

                return check

            with StreamingLandmark(
                graph, self.jobs(), _device_factory=lambda **kw: device
            ) as owner:
                output = owner.run([1.0] * 35, validator_factory=validator)
                self.assertEqual(set(output), {"out0", "out1", "out2", "out3"})
                self.assertIs(output["out0"], output["out3"])
                # Valid interior: 9*2.25/8-.5+2.25. Borders use fewer taps.
                expected = (
                    sum(
                        (min(y + 1, 4) - max(y - 1, 0) + 1)
                        * (min(x + 1, 6) - max(x - 1, 0) + 1)
                        * 2.25
                        / 8
                        + 1.75
                        for y in range(5)
                        for x in range(7)
                    )
                    / 35
                )
                self.assertAlmostEqual(output["out0"][0], expected, places=6)
                self.assertEqual([c[0] for c in device.commands], [20, 20, 21, 22, 24])
                self.assertEqual(sum(c[2] for c in checks), 8)
                self.assertTrue(owner.metrics[-1]["complete"])
            self.assertTrue(device.closed)

    def test_nonfinite_marker_or_checker_exception_invalidates_graph(self):
        with tempfile.TemporaryDirectory() as d:
            for corruption in (True, False):
                device = Emulator()
                device.corrupt = corruption
                owner = StreamingLandmark(
                    self.graph(d), self.jobs(), _device_factory=lambda **kw: device
                )

                def factory(s, inputs):
                    def check(*args):
                        if s["operator"] == "Convolution":
                            raise AssertionError("checker")

                    return check

                with self.assertRaises(RecoveryRequired):
                    owner.run([1.0] * 35, validator_factory=None if corruption else factory)
                self.assertTrue(owner.invalid)
                with self.assertRaises(RuntimeError):
                    owner.run([1.0] * 35)

    def test_preflight_bad_input_never_opens_device(self):
        with tempfile.TemporaryDirectory() as d:
            factory = Mock()
            owner = StreamingLandmark(self.graph(d), self.jobs(), _device_factory=factory)
            for x in ([], [math.inf] * 35):
                with self.assertRaises(ValueError):
                    owner.run(x)
            factory.assert_not_called()

    def test_heldout_cannot_fit_graph_limits(self):
        rows = [
            {
                "split": "calibration",
                "layer": "x",
                "error": {"max_abs": 0.01, "rms": 0.001},
                "reference_max_abs": 1.0,
                "reference_rms": 0.5,
            }
        ]
        before = frozen_limits(rows)
        rows.append({**rows[0], "split": "heldout", "error": {"max_abs": 100.0, "rms": 100.0}})
        self.assertEqual(before, frozen_limits(rows))

    def test_hardware_runner_requires_opt_in(self):
        result = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tests/hardware_landmark.py"),
                "--jobs",
                "unused",
                "--model",
                "unused",
                "--corpus",
                "unused",
                "--output",
                "unused",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("--run-on-device is required", result.stderr)


if __name__ == "__main__":
    unittest.main()
