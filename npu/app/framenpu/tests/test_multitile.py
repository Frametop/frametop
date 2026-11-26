"""Check chunk scheduling/serialization against an independent FP32 reference."""

import random
import struct
import unittest

from framenpu import Device, JobResult
from test_library import FakeTransport


def rounded(value, code):
    return struct.unpack("<" + code, struct.pack("<" + code, value))[0]


def reference(a, b, bias, relu=False):
    answer = []
    for row in a:
        result = []
        for column in range(len(b[0])):
            total = 0.0
            for start in range(0, len(b), 32):
                product = sum(
                    rounded(row[q], "e") * rounded(b[q][column], "e")
                    for q in range(start, min(start + 32, len(b)))
                )
                total = rounded(total + rounded(product, "e"), "f")
            total = rounded(total + rounded(bias[column], "f"), "f")
            result.append(max(0, total) if relu else total)
        answer.append(result)
    return answer


class ProtocolDevice(Device):
    """Emulate the external wire contract; never connect to hardware."""

    def __init__(self, dispatch="direct"):
        super().__init__(dispatch=dispatch)
        self.calls = []

    def submit(self, job, data, *, output_size, opcode, params):
        self.calls.append((opcode, params))
        if opcode == 3:
            count, broadcast, relu = params
            values = struct.unpack("<%df" % (count + broadcast), data)
            output = [rounded(values[i] + values[count + i % broadcast], "f") for i in range(count)]
            if relu:
                output = [max(0, value) for value in output]
        else:
            m, k, n, relu = params[:4]
            self.assert_budget(m, k, n, opcode)
            operands = struct.unpack_from("<%de" % (m * k + k * n), data)
            bias_offset = ((m * k + k * n) * 2 + 3) & ~3
            biases = struct.unpack_from("<%df" % n, data, bias_offset)
            seeded = params[4] if opcode == 6 else 0
            expected_size = bias_offset + n * 4 + seeded * m * n * 4
            if len(data) != expected_size or output_size != m * n * 4:
                raise AssertionError("wire length or alignment differs from the kernel ABI")
            output = (
                list(struct.unpack_from("<%df" % (m * n), data, bias_offset + n * 4))
                if seeded
                else [0.0] * (m * n)
            )
            self.last_seed = list(output) if seeded else None
            for r in range(m):
                for c in range(n):
                    index = r * n + c
                    for start in range(0, k, 32):
                        product = sum(
                            operands[r * k + q] * operands[m * k + q * n + c]
                            for q in range(start, min(start + 32, k))
                        )
                        output[index] = rounded(output[index] + rounded(product, "e"), "f")
                    output[index] = rounded(output[index] + biases[c], "f")
                    if relu:
                        output[index] = max(0, output[index])
        return JobResult(struct.pack("<%df" % len(output), *output), 0, ())

    @staticmethod
    def assert_budget(m, k, n, opcode):
        if not 1 <= m <= 32 or not 1 <= n <= 32 or not 1 <= k <= (64 if opcode == 6 else 32):
            raise AssertionError("chunk exceeds the execution budget")


class MultiTileTests(unittest.TestCase):
    def test_two_depth_tiles_use_one_submission_instead_of_four(self):
        a = [[1] * 64 for _ in range(32)]
        b = [[1] * 32 for _ in range(64)]
        batched, old = ProtocolDevice(), ProtocolDevice()
        expected = [[64.0] * 32 for _ in range(32)]
        self.assertEqual(batched.matmul(a, b, tiles_per_job=2), expected)
        self.assertEqual(old.matmul(a, b), expected)
        self.assertEqual(batched.calls, [(6, (32, 64, 32, 0, 0))])
        self.assertEqual([call[0] for call in old.calls], [1, 1, 3, 3])

    def test_padded_output_and_odd_depth_match_reference(self):
        rng = random.Random(0x4D54494C)
        a = [[rng.randint(-4, 4) / 2 for _ in range(65)] for _ in range(33)]
        b = [[rng.randint(-4, 4) / 2 for _ in range(35)] for _ in range(65)]
        bias = [rng.randint(-4, 4) / 4 for _ in range(35)]
        device = ProtocolDevice()
        self.assertEqual(device.matmul(a, b, bias=bias, tiles_per_job=2), reference(a, b, bias))
        self.assertEqual(len(device.calls), 8)
        self.assertTrue(all(opcode == 6 for opcode, params in device.calls))
        self.assertEqual(sum(params[4] for opcode, params in device.calls), 4)

    def test_seed_preserves_left_to_right_rounding_across_chunks(self):
        a, b = [[0.0] * 128], [[1.0] for _ in range(128)]
        a[0][0], a[0][32], a[0][64], a[0][96] = 65504, 2**-12, -65504, 2**-12
        device = ProtocolDevice()
        self.assertEqual(device.matmul(a, b, tiles_per_job=2), [[2**-12]])
        self.assertEqual(device.matmul(a, b, tiles_per_job=2), reference(a, b, [0]))

    def test_accumulator_is_fp32_and_may_exceed_fp16_range(self):
        a, b = [[0.0] * 65], [[1.0] for _ in range(65)]
        a[0][0], a[0][32], a[0][64] = 65504, 65504, -65504
        device = ProtocolDevice()
        self.assertEqual(device.matmul(a, b, tiles_per_job=2), [[65504.0]])
        self.assertEqual(device.last_seed, [131008.0])

    def test_bias_and_relu_apply_only_after_the_last_chunk(self):
        a, b = [[0.0] * 65], [[1.0] for _ in range(65)]
        a[0][0], a[0][32], a[0][64] = -4, -4, 16
        device = ProtocolDevice()
        self.assertEqual(
            device.matmul(a, b, bias=[2], activation="relu", tiles_per_job=2), [[10.0]]
        )
        self.assertEqual([params[3] for opcode, params in device.calls], [0, 1])

    def test_two_tile_path_also_accepts_a_single_padded_product(self):
        device = ProtocolDevice()
        self.assertEqual(device.matmul([[2]], [[3]], bias=[1], tiles_per_job=2), [[7.0]])
        self.assertEqual(device.calls, [(6, (1, 1, 1, 0, 0))])

    def test_invalid_tile_budget_or_backend_rejected_before_hardware(self):
        for value in (0, 3, True, 1.0):
            transport = FakeTransport()
            with self.subTest(tiles_per_job=value), self.assertRaises(ValueError):
                Device(dispatch="direct", _transport=transport).matmul(
                    [[1]], [[1]], tiles_per_job=value
                )
            self.assertEqual(transport.events, [])
        transport = FakeTransport()
        with self.assertRaisesRegex(ValueError, "dispatch='direct'"):
            Device(_transport=transport).matmul([[1]], [[1]], tiles_per_job=2)
        self.assertEqual(transport.events, [])


if __name__ == "__main__":
    unittest.main()
