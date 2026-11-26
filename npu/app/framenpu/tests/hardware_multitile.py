"""Opt-in qualification of two depth tiles per direct HMX job."""

import argparse
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Device, JobFailed
from framenpu.provenance import verify_kernel
from hardware_dispatch import percentiles
from hardware_hmx import PowerSnapshot, audited_device, health
from hardware_smoke import equal, reference


class MeasuredDevice(Device):
    def __init__(self):
        super().__init__(dispatch="direct")
        self.calls = []

    def submit(self, job, data=b"", **kwargs):
        result = super().submit(job, data, **kwargs)
        self.calls.append(
            {
                "opcode": kwargs.get("opcode", 0),
                "hmx": job.requires_hmx,
                "publish_ms": result.elapsed_ms,
                "kernel_ticks": result.kernel_reference_ticks,
            }
        )
        return result


def run(iterations, correctness_only=False):
    manifest = verify_kernel()
    rng = random.Random(0x4D54494C)
    audits = {"restored_jobs": 0, "restored_hmx_jobs": 0}
    with PowerSnapshot() as power:
        baseline = power.read()
        initial = health(power, baseline)
        AuditedDevice = audited_device(power, audits)
        with AuditedDevice(dispatch="direct") as device:
            for m, k, n in (
                (2, 33, 3),
                (32, 64, 32),
                (32, 128, 32),
                (31, 63, 33),
                (33, 65, 35),
                (1, 256, 1),
            ):
                a = [[rng.randint(-4, 4) / 2 for _ in range(k)] for _ in range(m)]
                b = [[rng.randint(-4, 4) / 2 for _ in range(n)] for _ in range(k)]
                bias = [rng.randint(-4, 4) / 4 for _ in range(n)]
                print(f"MULTITILE_STAGE shape_{m}x{k}x{n}", flush=True)
                got = device.matmul(a, b, bias=bias, activation="relu", tiles_per_job=2)
                equal(f"multi_{m}x{k}x{n}", got, reference(a, b, bias, relu=True))

            for name, k, values, expected in (
                ("sequential_rounding", 128, (65504, 2**-12, -65504, 2**-12), 2**-12),
                ("fp32_seed_range", 65, (65504, 65504, -65504), 65504.0),
                ("final_bias_relu", 65, (-4, -4, 16), 10.0),
            ):
                a = [[0.0] * k]
                b = [[1.0] for _ in range(k)]
                for index, value in enumerate(values):
                    a[0][index * 32] = value
                bias = [2] if name == "final_bias_relu" else [0]
                got = device.matmul(a, b, bias=bias, activation="relu", tiles_per_job=2)
                equal(name, got, [[expected]])
                if got != reference(a, b, bias, relu=True):
                    raise AssertionError("sequential FP32 reference differs")

            # Reject unsupported execution modes and malformed jobs on the DSP,
            # then prove that the same session still accepts valid work.
            scalar = struct.pack("<2ef", 1, 1, 0)
            probes = [
                (True, scalar, (1, 1, 1, 0, 0), False),
                (False, scalar, (1, 1, 1, 0, 0), True),
                (True, b"", (33, 1, 1, 0, 0), True),
                (True, b"", (1, 65, 1, 0, 0), True),
                (True, scalar, (1, 1, 1, 0, 2), True),
                (True, scalar, (1, 1, 1, 0, 1), True),
            ]
            for hmx, data, params, direct in probes:
                try:
                    device._submit(
                        device.builtin(hmx=hmx),
                        data,
                        output_size=4,
                        opcode=6,
                        params=params,
                        use_direct=direct,
                    )
                except JobFailed:
                    pass
                else:
                    raise AssertionError("invalid multi-tile request was accepted")
            equal("post_rejection_reuse", device.matmul([[2]], [[3]], tiles_per_job=2), [[6]])
            after = device.submit(device.builtin(hmx=False), opcode=4)
            ssr = [row[7] for row in after.workers]
            if ssr != [0x81840200, 0x89840200, 0x91840200]:
                raise AssertionError("worker SSR differs")
        checked_health = health(power, baseline)
        print(
            "MULTITILE_CORRECTNESS",
            json.dumps(
                {
                    "audits": audits,
                    "worker_ssr": [hex(x) for x in ssr],
                    "native_rejections": len(probes),
                    "health": checked_health,
                }
            ),
            flush=True,
        )

        timings = []
        if not correctness_only:
            m, k, n = 32, 64, 32
            a = [[rng.randint(-4, 4) / 2 for _ in range(k)] for _ in range(m)]
            b = [[rng.randint(-4, 4) / 2 for _ in range(n)] for _ in range(k)]
            bias = [rng.randint(-4, 4) / 4 for _ in range(n)]
            expected = reference(a, b, bias, relu=True)
            for budget in (1, 2):
                wall, maximum_job, calls, powers = [], [], [], []
                with MeasuredDevice() as device:
                    for index in range(iterations + 1):
                        start_call = len(device.calls)
                        started = time.monotonic_ns()
                        got = device.matmul(
                            a, b, bias=bias, activation="relu", tiles_per_job=budget
                        )
                        elapsed = (time.monotonic_ns() - started) / 1e6
                        if got != expected:
                            raise AssertionError("timed multi-tile product differs")
                        if index:
                            recent = device.calls[start_call:]
                            wall.append(elapsed)
                            maximum_job.append(max(item["publish_ms"] for item in recent))
                            calls.append(len(recent))
                            powers.append(sum(item["hmx"] for item in recent))
                if len(set(calls)) != 1 or len(set(powers)) != 1:
                    raise AssertionError("unstable submission count")
                item = {
                    "shape": [m, k, n],
                    "tiles_per_job": budget,
                    "iterations": iterations,
                    "wall_ms": percentiles(wall),
                    "max_job_publish_ms": max(maximum_job),
                    "submissions_per_matmul": calls[0],
                    "hmx_power_cycles_per_matmul": powers[0],
                }
                timings.append(item)
                print("MULTITILE_TIMING", json.dumps(item), flush=True)
                health(power, baseline)
        print(
            "MULTITILE_QUALIFIED",
            json.dumps(
                {
                    "runtime_bytes": manifest["bytes"],
                    "runtime_sha256": manifest["sha256"],
                    "audits": audits,
                    "timings": timings,
                    "initial_health": initial,
                    "final_health": health(power, baseline),
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
                        capture_output=True,
                        text=True,
                        check=True,
                    ).stdout,
                }
            ),
            flush=True,
        )


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--correctness-only", action="store_true")
    args = parser.parse_args(argv)
    if not args.run_on_device:
        parser.error("--run-on-device is required; this changes DSP state and HMX power")
    if os.geteuid() != 0:
        parser.error("run with sudo for /dev/mem access")
    if not 1 <= args.iterations <= 1000:
        parser.error("--iterations must be between 1 and 1000")
    run(args.iterations, args.correctness_only)


if __name__ == "__main__":
    main()
