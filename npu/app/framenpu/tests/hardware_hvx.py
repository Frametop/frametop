"""Opt-in raw packing/copy qualification and scalar/HVX runtime comparison."""

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
from hardware_smoke import equal, reference


def layout_case(device, probe, rows, cols, stride, column, prefix, start):
    values = [(start + i) & 65535 for i in range(rows * stride)]
    # Express the independent reference as alternating two row slices, then
    # pad missing rows/columns. Compare bits, including NaNs and subnormals.
    expected = []
    for row in range(0, 32, 2):
        for col in range(32):
            for lane in (row, row + 1):
                expected.append(
                    values[lane * stride + column + col] if lane < rows and col < cols else 0
                )
    result = device.submit(
        probe,
        bytes([0xA5]) * prefix + struct.pack(f"<{len(values)}H", *values),
        output_size=2048,
        params=(rows, cols, stride, column, prefix),
    )
    if result.output != struct.pack("<1024H", *expected):
        raise AssertionError(f"packing differs: {(rows, cols, stride, column, prefix, start)}")


def run(probe_path, baseline_path, iterations):
    manifest = verify_kernel()
    probe = Job.load(probe_path)
    audits = {"restored_jobs": 0, "restored_hmx_jobs": 0}
    with PowerSnapshot() as power:
        baseline_power = power.read()
        initial = health(power, baseline_power)
        AuditedDevice = audited_device(power, audits)
        layout_count = 0
        copy_count = 0
        with AuditedDevice(dispatch="direct") as device:
            for start in range(0, 65536, 1024):
                layout_case(device, probe, 32, 32, 32, 0, 0, start)
                layout_count += 1
            for start in range(0, 65536, 2048):
                for column in (0, 32):
                    layout_case(device, probe, 32, 32, 64, column, 0, start)
                    layout_count += 1
            for case in (
                (1, 1, 1, 0, 0),
                (31, 32, 32, 0, 0),
                (32, 31, 31, 0, 0),
                (31, 31, 63, 32, 0),
                (32, 32, 33, 0, 0),
                (32, 32, 64, 0, 4),
                (32, 32, 64, 32, 64),
                (32, 32, 32, 0, 124),
            ):
                layout_case(device, probe, *case, 0xF123)
                layout_count += 1
            lengths = (0, 1, 3, 4, 127, 128, 129, 255, 256, 257, 4095, 4096, 4097, 65536)
            for length in lengths:
                data = bytes((i * 73 + 19) & 255 for i in range(length))
                result = device.submit(
                    device.builtin(hmx=False), data, output_size=length, opcode=5
                )
                if result.output != data:
                    raise AssertionError(f"copy differs: {length}")
                copy_count += 1
                if length <= 4097:
                    for prefix in (4, 64, 124):
                        shifted = bytes([0xA5]) * prefix + data
                        result = device.submit(
                            probe, shifted, output_size=len(shifted), opcode=5, params=(prefix,)
                        )
                        if result.output != bytes(prefix) + data:
                            raise AssertionError(f"unaligned copy differs: {(length, prefix)}")
                        copy_count += 1
            rng = random.Random(0x485658)
            shapes = (
                (32, 32, 32),
                (32, 64, 32),
                (31, 64, 32),
                (32, 64, 31),
                (31, 32, 32),
                (32, 33, 32),
            )
            for m, k, n in shapes:
                a = [[rng.randint(-4, 4) / 2 for _ in range(k)] for _ in range(m)]
                b = [[rng.randint(-4, 4) / 2 for _ in range(n)] for _ in range(k)]
                bias = [rng.randint(-4, 4) / 4 for _ in range(n)]
                equal(
                    f"hvx_{m}x{k}x{n}",
                    device.matmul(a, b, bias=bias, activation="relu", tiles_per_job=2),
                    reference(a, b, bias, relu=True),
                )
            ssr = [row[7] for row in device.submit(device.builtin(hmx=False), opcode=4).workers]
            if ssr != [0x81840200, 0x89840200, 0x91840200]:
                raise AssertionError("worker SSR differs")
        print(
            "HVX_CORRECTNESS",
            json.dumps(
                {
                    "layout_cases": layout_count,
                    "copy_cases": copy_count,
                    "gemm_shapes": shapes,
                    "audits": audits,
                    "worker_ssr": [hex(x) for x in ssr],
                    "health": health(power, baseline_power),
                }
            ),
            flush=True,
        )

        timings = []
        if baseline_path:
            scalar = baseline_path.read_bytes()
            if (
                hashlib.sha256(scalar).hexdigest()
                != "6a7eaa2a42bb727f6bec3b4036a8d377edc0b77dfd936dea78c3e9245d690ef2"
            ):
                raise ValueError("expected the qualified 3380-byte pre-HVX runtime")
            vector = (ROOT / "src/framenpu/data/runtime.bin").read_bytes()
            # Compare the same wire requests under one clock/power policy.
            with Device(dispatch="direct") as device:
                for label, code in (("scalar", scalar), ("hvx", vector)):
                    plain = Job(code, label, supports_direct=True)
                    hmx = Job(code, label, requires_hmx=True, supports_direct=True)
                    data = bytes(range(256)) * 16
                    item = {
                        "runtime": label,
                        "sha256": hashlib.sha256(code).hexdigest(),
                        "copy_4k": measure(
                            device, plain, data, expected=data, opcode=5, iterations=iterations
                        ),
                    }
                    for k in (32, 64):
                        a = [float((r + 3) % k == c) for r in range(32) for c in range(k)]
                        b = [(r * 7 + c * 3) % 17 / 4 - 2 for r in range(k) for c in range(32)]
                        data = struct.pack(f"<{len(a) + len(b)}e", *(a + b)) + bytes(128)
                        expected = [b[((r + 3) % k) * 32 + c] for r in range(32) for c in range(32)]
                        item[f"gemm_32x{k}x32"] = measure(
                            device,
                            hmx,
                            data,
                            expected=struct.pack("<1024f", *expected),
                            opcode=1 if k == 32 else 6,
                            params=(32, k, 32, 0, 0),
                            iterations=iterations,
                        )
                    timings.append(item)
                    print("HVX_TIMING", json.dumps(item), flush=True)
        print(
            "HVX_QUALIFIED",
            json.dumps(
                {
                    "runtime_bytes": manifest["bytes"],
                    "runtime_sha256": manifest["sha256"],
                    "probe_sha256": hashlib.sha256(probe.code).hexdigest(),
                    "audits": audits,
                    "timings": timings,
                    "initial_health": initial,
                    "final_health": health(power, baseline_power),
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
        parser.error("run with sudo and --probe packing_probe.fjob")
    if not 1 <= args.iterations <= 1000:
        parser.error("--iterations must be between 1 and 1000")
    run(args.probe, args.baseline, args.iterations)


if __name__ == "__main__":
    main()
