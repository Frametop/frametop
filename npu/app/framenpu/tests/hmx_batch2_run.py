"""Opt-in two-depth-block HMX batching A/B probe, without clock changes.

Both input blocks have different deterministic values. Activation and weight
bases deliberately alias to stay inside the verified 8 KiB VTCM scratch: each
transaction computes groups*(A0@A0 + A1@A1). Mode 0 issues two existing load
pairs per group; mode 1 issues one paired instruction with range=4095. Every
FP16 output tile and the final FP32 conversion are checked exactly.

This diagnostic ratio is not distinct-weight model-throughput proof. Group
timing includes the diagnostic mode check/helper overhead, which is shared by
both paths; batching also amortizes ordinary SDK helper calls. Fresh direct
sessions, default power policy, phase timings and no-recovery behavior match
the correct baseline. Defaults to the control path; select --modes 0 1 for A/B.

  sudo python3 tests/hmx_batch2_run.py --run-on-device --probe hmx_batch2.fjob --modes 0 1 --output /tmp/hmx-batch2.json
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Device, Job
from framenpu.errors import FrameNPUError
from framenpu.job import JobResult
import hmx_baseline_run as baseline
from hardware_hmx import PowerSnapshot, health
from hmx_multi_run import failure_snapshot

CASES = ("signed_dense", "identity_and_dense")
MODES = {0: "control", 1: "batch2"}


def operands(case):
    rng = random.Random(0x42415432 + CASES.index(case))
    first = (
        [8 * int(r == c) for r in range(32) for c in range(32)]
        if case == "identity_and_dense"
        else [rng.randint(-4, 4) for _ in range(1024)]
    )
    second = [rng.randint(-4, 4) for _ in range(1024)]
    data = struct.pack("<2048e", *[x / 8 for x in first + second])
    product = [
        sum(
            first[r * 32 + k] * first[k * 32 + c] + second[r * 32 + k] * second[k * 32 + c]
            for k in range(32)
        )
        for r in range(32)
        for c in range(32)
    ]
    return data, product


def reference(product, groups):
    return baseline.reference(product, groups)


def decode(result, groups, transactions, mode, expected):
    if mode not in MODES or not 1 <= groups <= 256 or not 1 <= transactions <= 4:
        raise ValueError("invalid batch probe parameters")
    if len(result.output) < 256:
        raise ValueError("incomplete batch probe result")
    tag = struct.unpack_from("<I", result.output, 28)[0]
    if tag != mode + 1:
        raise ValueError("batch probe mode tag mismatch")
    normalized = bytearray(result.output)
    struct.pack_into("<I", normalized, 28, 0)
    sample = baseline.decode(
        JobResult(bytes(normalized), result.elapsed_ms, result.workers),
        groups,
        transactions,
        expected,
    )
    sample.update(groups=groups, depth=groups * 2, mode=mode)
    sample["macs"] *= 2
    return sample


def one(probe, data, groups, transactions, mode, expected):
    session_start = time.monotonic_ns()
    transport = baseline.TimedTransport()
    with Device(
        dispatch="direct",
        _transport=transport,
        _power_factory=lambda: baseline.TimedPower(transport.current),
    ) as device:
        started = time.monotonic_ns()
        result = device.submit(
            probe,
            data,
            output_size=256 + transactions * 2048 + 4096,
            opcode=9,
            params=(groups, transactions, mode),
        )
        submit_ms = (time.monotonic_ns() - started) / 1e6
    session_ms = (time.monotonic_ns() - session_start) / 1e6
    sample = decode(result, groups, transactions, mode, expected)
    if len(transport.benchmarks) != 1:
        raise ValueError("ambiguous host timing trace")
    trace = transport.benchmarks[0]
    sample["host_phase_ms"] = {k: v for k, v in trace.items() if k.endswith("_ms")}
    sample["powered_configuration"] = trace["powered_configuration"]
    sample["submit_wall_ms"], sample["session_wall_ms"] = submit_ms, session_ms
    return sample


def summarize(samples):
    """Reuse baseline units with depth=two blocks per group and doubled MACs."""
    report = baseline.summarize(samples)
    for point in report["points"]:
        point["groups"] = point["depth"] // 2
    for slope in report["resident_slopes"]:
        slope["basis"] = "ticks per 32-depth block; batch2 consumes two blocks per packet"
    comparisons = []
    identities = sorted({(s["matrix_case"], s["groups"]) for s in samples})
    for matrix_case, groups in identities:
        control = [
            s["dsp_phase_ticks"]["hmx"]
            for s in samples
            if s["matrix_case"] == matrix_case and s["groups"] == groups and s["mode"] == 0
        ]
        batched = [
            s["dsp_phase_ticks"]["hmx"]
            for s in samples
            if s["matrix_case"] == matrix_case and s["groups"] == groups and s["mode"] == 1
        ]
        if control and batched:
            control_stats, batch_stats = (
                baseline.percentiles(control),
                baseline.percentiles(batched),
            )
            comparisons.append(
                {
                    "matrix_case": matrix_case,
                    "groups": groups,
                    "logical_shape": [32, 64 * groups, 32],
                    "control_hmx_job_ticks": control_stats,
                    "batch2_hmx_job_ticks": batch_stats,
                    "speedup_at_p50": control_stats["p50"] / batch_stats["p50"],
                }
            )
    report["comparisons"] = comparisons
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    parser.add_argument("--probe", required=True)
    parser.add_argument("--samples", type=int, default=20)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--groups", type=int, nargs="+", default=[1, 16, 64, 256])
    parser.add_argument("--transactions", type=int, default=4)
    parser.add_argument("--cases", choices=CASES, nargs="+", default=list(CASES))
    parser.add_argument("--modes", choices=(0, 1), type=int, nargs="+", default=[0])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    if not args.run_on_device:
        parser.error("--run-on-device is required; this powers HMX and changes DSP state")
    if not 1 <= args.samples <= 200 or not 0 <= args.warmup <= 10:
        parser.error("require 1..200 samples and 0..10 warmups")
    if not 1 <= args.transactions <= 4 or any(not 1 <= n <= 256 for n in args.groups):
        parser.error("require 1..4 transactions and group counts in 1..256")
    if os.geteuid() != 0:
        parser.error("run with sudo for /dev/mem access")
    probe = Job.load(args.probe)
    if not probe.requires_hmx or not probe.supports_direct:
        parser.error("probe requires HMX and direct-entry flags")
    cases = {case: operands(case) for case in dict.fromkeys(args.cases)}
    expected = {
        (case, groups, mode): reference(cases[case][1], groups)
        for case in cases
        for groups in sorted(set(args.groups))
        for mode in sorted(set(args.modes))
    }
    report = {
        "ref_hz": baseline.REF_HZ,
        "code_bytes": len(probe.code),
        "code_sha256": hashlib.sha256(probe.code).hexdigest(),
        "case_input_sha256": {
            case: hashlib.sha256(value[0]).hexdigest() for case, value in cases.items()
        },
        "samples_per_point": args.samples,
        "warmups_per_point": args.warmup,
        "percentile_method": "nearest rank; small-sample p99 may equal the maximum",
        "workload": "groups*(A0@A0+A1@A1); activation/weight bases deliberately alias",
        "interpretation_scope": "instruction batching diagnostic, not distinct-weight model throughput",
        "samples": [],
        "checked_warmup_jobs": 0,
    }
    rng, failed = random.Random(0x32424154), False
    with PowerSnapshot() as power:
        initial_power = power.read()
        try:
            report["initial_health"] = health(power, initial_power)
            for index in range(args.samples + args.warmup):
                order = list(expected)
                rng.shuffle(order)
                for matrix_case, groups, mode in order:
                    sample = one(
                        probe,
                        cases[matrix_case][0],
                        groups,
                        args.transactions,
                        mode,
                        expected[(matrix_case, groups, mode)],
                    )
                    if health(power, initial_power) != report["initial_health"]:
                        raise ValueError("session restoration differs from initial health")
                    configuration = sample["powered_configuration"]
                    if "powered_configuration" not in report:
                        report["powered_configuration"] = configuration
                    elif configuration != report["powered_configuration"]:
                        raise ValueError("powered clock configuration changed during comparison")
                    sample.update(
                        matrix_case=matrix_case,
                        case=f"{matrix_case}/{MODES[mode]}",
                        sample_index=index - args.warmup,
                    )
                    if index < args.warmup:
                        report["checked_warmup_jobs"] += 1
                    else:
                        report["samples"].append(sample)
                if index == args.warmup - 1 or (index - args.warmup + 1) % 10 == 0:
                    print(
                        f"batch2: {len(report['samples'])} measured jobs, {report['checked_warmup_jobs']} checked warmups",
                        file=sys.stderr,
                        flush=True,
                    )
            report["summary"] = summarize(report["samples"])
            report["final_health"] = health(power, initial_power)
            report["all_outputs_match"] = True
        except (FrameNPUError, ValueError, OSError) as exc:
            failed = True
            report["error"] = f"{type(exc).__name__}: {exc}"
            report["after_failure"] = failure_snapshot(power)
            report["interpretation"] = "inconclusive; no recovery attempted"
    encoded = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded)
        print(
            json.dumps(
                {
                    "report": str(args.output),
                    "measured_jobs": len(report["samples"]),
                    "all_outputs_match": report.get("all_outputs_match", False),
                    "error": report.get("error"),
                    "comparisons": report.get("summary", {}).get("comparisons"),
                },
                indent=2,
            )
        )
    else:
        print(encoded, end="")
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
