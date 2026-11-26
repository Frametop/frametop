"""Opt-in exact HVX output conversion, bounded readback and timing qualification."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Device, Job
from framenpu.provenance import verify_kernel
from hardware_dispatch import measure
from hardware_hmx import PowerSnapshot, audited_device, health
from hardware_smoke import reference

BASELINE_SHA256 = "315a3eff8b851e3a8b31cf43477aeeace9a3f26dd4a57155491c2d3e5f33027b"


def half_bits_to_float_bits(h):
    """Independent field construction, preserving signaling NaN payload bits."""
    sign, exponent, mantissa = (h >> 15) << 31, (h >> 10) & 31, h & 1023
    if exponent:
        return sign | ((255 if exponent == 31 else exponent + 112) << 23) | (mantissa << 13)
    if not mantissa:
        return sign
    top = mantissa.bit_length() - 1
    return sign | ((top + 103) << 23) | ((mantissa - (1 << top)) << (23 - top))


def packed_tile(values):
    """Alternate matching columns from successive row pairs."""
    return struct.pack(
        "<1024H",
        *(
            values[row * 32 + col]
            for pair in range(16)
            for col in range(32)
            for row in (2 * pair, 2 * pair + 1)
        ),
    )


def accumulation_case(device, probe, values, seed, shape):
    rows, cols, stride, prefix = shape
    hmx_probe = Job(probe.code, "readback", requires_hmx=True, supports_direct=True)
    data = packed_tile(values) + struct.pack(f"<{len(seed)}I", *seed)
    kwargs = {"output_size": prefix + rows * stride * 4 + 128, "params": shape}
    actual = device.submit(hmx_probe, data, opcode=1, **kwargs).output
    oracle = device.submit(probe, data, opcode=2, **kwargs).output
    if actual != oracle:
        raise AssertionError(f"scalar/HVX accumulation differs: {shape}")
    guard = bytes([0xA5])
    if actual[:prefix] != guard * prefix or actual[-128:] != guard * 128:
        raise AssertionError("readback overwrote a guard")
    words = struct.unpack(f"<{len(seed)}I", actual[prefix:-128])
    for row in range(rows):
        if tuple(words[row * stride + cols : (row + 1) * stride]) != tuple(
            seed[row * stride + cols : (row + 1) * stride]
        ):
            raise AssertionError("readback overwrote row padding")
    # For finite values, also compare against an independent host FP32 sum.
    expected = list(seed)
    finite = True
    for row in range(rows):
        for col in range(cols):
            h = values[row * 32 + col]
            if h & 0x7C00 == 0x7C00 or seed[row * stride + col] & 0x7F800000 == 0x7F800000:
                finite = False
                continue
            value = struct.unpack("<f", struct.pack("<I", half_bits_to_float_bits(h)))[0]
            initial = struct.unpack("<f", struct.pack("<I", seed[row * stride + col]))[0]
            expected[row * stride + col] = struct.unpack("<I", struct.pack("<f", initial + value))[
                0
            ]
    if finite and tuple(expected) != words:
        raise AssertionError(f"host FP32 reference differs: {shape}")


def run(probe_path, baseline_path, iterations):
    manifest = verify_kernel()
    probe = Job.load(probe_path)
    baseline = baseline_path.read_bytes() if baseline_path else None
    if baseline is not None and hashlib.sha256(baseline).hexdigest() != BASELINE_SHA256:
        raise ValueError("expected the qualified 3520-byte runtime before HVX readback")
    audits = {"restored_jobs": 0, "restored_hmx_jobs": 0}
    conversion_cases, accumulation_cases = 0, 0
    seed_patterns = (
        0,
        0x80000000,
        1,
        0x80000001,
        0x007FFFFF,
        0x00800000,
        0x3F800000,
        0xBF800000,
        0x38000000,
        0x47800000,
        0x48000000,
        0x71800000,
    )
    with PowerSnapshot() as power:
        saved_power = power.read()
        initial = health(power, saved_power)
        AuditedDevice = audited_device(power, audits)
        with AuditedDevice(dispatch="direct") as device:
            for start in range(0, 65536, 1024):
                values = list(range(start, start + 1024))
                converted = device.submit(probe, packed_tile(values), output_size=4096).output
                expected = struct.pack("<1024I", *(half_bits_to_float_bits(h) for h in values))
                if converted != expected:
                    raise AssertionError(f"conversion bits differ: {start}")
                conversion_cases += 1
                seed = [
                    seed_patterns[(i + start // 1024) % len(seed_patterns)] for i in range(1024)
                ]
                accumulation_case(device, probe, values, seed, (32, 32, 32, 0))
                accumulation_cases += 1
            for shape in (
                (1, 1, 1, 0),
                (1, 32, 32, 4),
                (31, 32, 32, 64),
                (32, 31, 31, 124),
                (3, 17, 33, 4),
                (32, 32, 64, 124),
                (31, 1, 1, 0),
            ):
                values = [(i * 61) % 0x7C00 | ((i & 1) << 15) for i in range(1024)]
                seed = [seed_patterns[i % len(seed_patterns)] for i in range(shape[0] * shape[2])]
                accumulation_case(device, probe, values, seed, shape)
                accumulation_cases += 1
            # Mixed packing/readback edges, both tile budgets and FP32 seed carry.
            rng = random.Random(0x52454144)
            shapes = ((1, 32, 32), (31, 64, 32), (32, 64, 31), (32, 128, 32), (33, 65, 35))
            for m, k, n in shapes:
                a = [[rng.randint(-4, 4) / 2 for _ in range(k)] for _ in range(m)]
                b = [[rng.randint(-4, 4) / 2 for _ in range(n)] for _ in range(k)]
                bias = [rng.randint(-4, 4) / 4 for _ in range(n)]
                expected = reference(a, b, bias, relu=True)
                for budget in (1, 2):
                    if (
                        device.matmul(a, b, bias=bias, activation="relu", tiles_per_job=budget)
                        != expected
                    ):
                        raise AssertionError(f"GEMM differs: {(m, k, n, budget)}")
            ssr = [row[7] for row in device.submit(device.builtin(hmx=False), opcode=4).workers]
            if ssr != [0x81840200, 0x89840200, 0x91840200]:
                raise AssertionError("worker SSR differs")
        correctness = {
            "conversion_cases": conversion_cases,
            "conversion_patterns": 65536,
            "accumulation_cases": accumulation_cases,
            "gemm_shapes": shapes,
            "tile_budgets": [1, 2],
            "audits": audits,
            "worker_ssr": [hex(x) for x in ssr],
            "health": health(power, saved_power),
        }
        print("READBACK_CORRECTNESS", json.dumps(correctness), flush=True)
        timings = []
        if baseline is not None:
            current = (ROOT / "src/framenpu/data/runtime.bin").read_bytes()
            with Device(dispatch="direct") as device:
                # Install before timing either payload; first calls remain excluded.
                device.submit(device.builtin(hmx=False), opcode=4)
                for label, code in (("scalar_readback", baseline), ("hvx_readback", current)):
                    job = Job(code, label, requires_hmx=True, supports_direct=True)
                    item = {"runtime": label, "sha256": hashlib.sha256(code).hexdigest()}
                    for k in (32, 64):
                        a = [float((r + 3) % k == c) for r in range(32) for c in range(k)]
                        b = [(r * 7 + c * 3) % 17 / 4 - 2 for r in range(k) for c in range(32)]
                        data = struct.pack(f"<{len(a) + len(b)}e", *(a + b)) + bytes(128)
                        expected = [b[((r + 3) % k) * 32 + c] for r in range(32) for c in range(32)]
                        item[f"gemm_32x{k}x32"] = measure(
                            device,
                            job,
                            data,
                            expected=struct.pack("<1024f", *expected),
                            opcode=1 if k == 32 else 6,
                            params=(32, k, 32, 0, 0),
                            iterations=iterations,
                        )
                    timings.append(item)
                    print("READBACK_TIMING", json.dumps(item), flush=True)
        print(
            "READBACK_QUALIFIED",
            json.dumps(
                {
                    "runtime_bytes": manifest["bytes"],
                    "runtime_sha256": manifest["sha256"],
                    "probe_sha256": hashlib.sha256(probe.code).hexdigest(),
                    "correctness": correctness,
                    "timings": timings,
                    "initial_health": initial,
                    "final_health": health(power, saved_power),
                    "service": subprocess.run(
                        [
                            "systemctl",
                            "show",
                            "dsp_service",
                            "-p",
                            "MainPID",
                            "-p",
                            "NRestarts",
                            "-p",
                            "ActiveState",
                        ],
                        check=True,
                        capture_output=True,
                        text=True,
                    ).stdout,
                }
            ),
            flush=True,
        )


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--iterations", type=int, default=100)
    args = parser.parse_args(argv)
    if not args.run_on_device:
        parser.error("--run-on-device is required; this changes DSP state and HMX power")
    if os.geteuid() != 0 or args.probe is None:
        parser.error("run with sudo and --probe readback_probe.fjob")
    if not 1 <= args.iterations <= 1000:
        parser.error("--iterations must be between 1 and 1000")
    run(args.probe, args.baseline, args.iterations)


if __name__ == "__main__":
    main()
