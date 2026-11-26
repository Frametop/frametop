"""Opt-in, numerically checked batched QFloat dependency-chain timing slopes."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Job, RecoveryRequired
from framenpu.resident_gemm import SSR, _lease
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health
from hardware_landmark import service
from hmx_multi_run import failure_snapshot

KINDS = ("empty", "multiply_fp32", "add_fp32", "add_with_zero_identity", "full_tap")


def operands(seed):
    a = [(0.5, -0.5, 1.0, -1.0, 2.0, -2.0, 4.0, -4.0)[(i + seed) % 8] for i in range(32)]
    b = [(1.0, -1.0, 0.0)[(i + seed) % 3] for i in range(32)]
    return a, b


def expected(kind, n, a, b):
    if kind == 0:
        return a
    if kind == 1:
        return [x * y**n for x, y in zip(a, b)]
    if kind in (2, 3):
        return [x + n * y for x, y in zip(a, b)]
    return [x + (x * y if n % 2 else 0) for x, y in zip(a, b)]


def error_bounds(kind, n, a, b):
    """Conservative propagated rounding bounds for these finite dyadic fixtures.

    QFloat conversion need not match exact FP32 identities. This diagnostic
    uses the graph's 2^-22 arithmetic scale, propagated over repeated steps;
    it does not change the frozen model qualification policy.
    """
    eps = 2**-22
    growth = math.exp(n * math.log1p(eps))
    result = []
    for x, y in zip(a, b):
        if kind == 0 or (kind == 1 and y == 0) or (kind == 3 and y == 0):
            result.append(0.0)
        elif kind == 1:
            result.append(abs(x) * math.expm1(n * math.log1p(eps)))
        elif kind in (2, 3):
            result.append(eps * (n * abs(x) + abs(y) * n * (n + 1) / 2) * growth)
        else:
            result.append(eps * n * (abs(x) + 3 * abs(x * y)) * growth * growth)
    return result


def check_values(kind, n, a, b, actual):
    reference = expected(kind, n, a, b)
    bounds = error_bounds(kind, n, a, b)
    if len(actual) != 32 or any(
        not math.isfinite(v) or abs(v - e) > bound for v, e, bound in zip(actual, reference, bounds)
    ):
        raise ValueError("QFloat fixture exceeds propagated arithmetic error bound")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.run_on_device or os.geteuid() != 0:
        parser.error("--run-on-device and Frame root are required")
    job = Job.load(args.probe)
    report = {
        "format": "framenpu-qfloat-slopes",
        "job_sha256": hashlib.sha256(job.code).hexdigest(),
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
        "samples": [],
        "service_before": service(),
    }
    failed = False
    lease = _lease()
    try:
        with PowerSnapshot() as power:
            baseline = power.read()
            report["initial_health"] = settled_health(power, baseline)
            try:
                with audited_device(power, report["audits"])(dispatch="direct") as device:

                    def run(kind, n, seed):
                        a, b = operands(seed)
                        captured = []

                        def validate(result):
                            try:
                                for worker, row in enumerate(result.workers):
                                    if (
                                        not row[0]
                                        or row[0] != result.workers[0][0]
                                        or row[1] != worker
                                        or row[2]
                                        or any(row[3:6])
                                        or row[7] != SSR[worker]
                                        or tuple(row[14:16]) != ((1, 1) if worker == 0 else (0, 0))
                                    ):
                                        raise ValueError("worker, fault or cleanup mismatch")
                                wrapper = result.kernel_reference_ticks
                                meta = struct.unpack("<32I", result.output[:128])
                                body = (meta[6] | meta[7] << 32) - (meta[4] | meta[5] << 32)
                                actual = struct.unpack("<32f", result.output[128:])
                                if (
                                    wrapper is None
                                    or not 0 < body <= wrapper < 768000
                                    or result.workers[0][8] != 0x87000000
                                    or meta[0] != 0x51465331
                                    or not 0x87A92000 <= meta[1] < 0x87A92000 + len(job.code)
                                    or meta[2:4] != (kind, n)
                                    or meta[8] != n
                                    or any(meta[9:])
                                    or any(not math.isfinite(x) for x in actual)
                                ):
                                    raise ValueError("numeric, timing or metadata mismatch")
                                check_values(kind, n, a, b, actual)
                                captured.append(
                                    {
                                        "kind": kind,
                                        "n": n,
                                        "seed": seed,
                                        "body_ticks": body,
                                        "wrapper_ms": wrapper / 19200,
                                    }
                                )
                            except Exception as exc:
                                raise RecoveryRequired(
                                    "QFloat probe invalid; retain resources"
                                ) from exc

                        device._submit(
                            job,
                            struct.pack("<64f", *(a + b)),
                            opcode=96,
                            params=(1, kind, n),
                            output_size=256,
                            use_direct=True,
                            _completion_validator=validate,
                        )
                        return captured[0]

                    for kind in range(5):
                        for n in (63, 64, 1024, 16384):
                            run(kind, n, 1)
                    for round_ in range(10):
                        for kind in range(5) if round_ % 2 else reversed(range(5)):
                            pair = [
                                run(kind, n, round_ % 3)
                                for n in ((1024, 16384) if round_ % 2 else (16384, 1024))
                            ]
                            for sample in pair:
                                sample["round"] = round_
                            report["samples"].extend(pair)
                report["final_health"] = settled_health(power, baseline)
                report["service_after"] = service()
                assert report["service_before"] == report["service_after"]
                table = []
                for kind, name in enumerate(KINDS):
                    slopes = []
                    for round_ in range(10):
                        pair = sorted(
                            [
                                s
                                for s in report["samples"]
                                if s["kind"] == kind and s["round"] == round_
                            ],
                            key=lambda s: s["n"],
                        )
                        slopes.append(
                            (pair[1]["body_ticks"] - pair[0]["body_ticks"]) / (16384 - 1024) / 19.2
                        )
                    table.append(
                        {
                            "kind": kind,
                            "name": name,
                            "us_per_iteration": statistics.median(slopes),
                            "min_us": min(slopes),
                            "max_us": max(slopes),
                            "slopes_us": slopes,
                        }
                    )
                report["table"] = table
                report["all_checks_passed"] = True
            except Exception as exc:
                failed = True
                report["error"] = f"{type(exc).__name__}: {exc}"
                report["causes"] = []
                cause = exc.__cause__
                while cause:
                    report["causes"].append(f"{type(cause).__name__}: {cause}")
                    cause = cause.__cause__
                report["after_failure"] = failure_snapshot(power)
    finally:
        lease.close()
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps({k: report.get(k) for k in ("all_checks_passed", "audits", "error", "table")}),
        flush=True,
    )
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
