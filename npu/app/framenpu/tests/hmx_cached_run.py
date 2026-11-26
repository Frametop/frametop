"""Opt-in cached/uncached instruction-fetch A/B with identical GEMM math and clocks.

The experimental FJOB, built with entry_cached.S, publishes a cached alias within the guarded
window. Every FP16 tile and the last FP32 conversion are checked using the
unchanged baseline decoder. Recorded wrapper PC verifies the selected alias;
final disassembly must also establish that hot helpers use PC-relative calls.
The experimental entry cleans code on normal and synchronous-fault return.
Cleanup markers are checked before host DDR/power reclamation; missing markers
retain resources and require recovery. Power and the 8 KiB VTCM span are unchanged.
Publication/cleanup remain in outer frame timing, outside internal HMX windows.
Defaults to uncached control only. Never performs automatic recovery.
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
from framenpu import Device, Job, JobFailed, RecoveryRequired
from framenpu.errors import FrameNPUError
from framenpu.job import JobResult
from hardware_hmx import PowerSnapshot, audited_device, health
from hmx_multi_run import failure_snapshot
import hmx_baseline_run as baseline

MODES = {"uncached": 0, "cached": 1}
BASES = {0: 0xF0A92000, 1: 0x87A92000}


def validate_cleanup(result, mode):
    """Run before Device reclaims borrowed code or powers HMX down."""
    if mode not in BASES:
        raise ValueError("invalid cache mode")
    if len(result.workers) != 3 or any(len(row) != 16 for row in result.workers):
        raise RecoveryRequired("incomplete cache cleanup status; retain payload and power")
    for worker, row in enumerate(result.workers):
        expected = (mode, mode) if worker == 0 else (0, 0)
        if tuple(row[14:16]) != expected:
            raise RecoveryRequired("cache cleanup was not confirmed; retain payload and power")


def decode(result, depth, transactions, mode, expected):
    validate_cleanup(result, mode)
    if len(result.output) < 256:
        raise ValueError("incomplete cached result")
    pc = struct.unpack_from("<I", result.output, 28)[0]
    if not BASES[mode] <= pc < BASES[mode] + 4096:
        raise ValueError(f"wrong execution alias: requested {mode}, observed PC {pc:#x}")
    output = bytearray(result.output)
    struct.pack_into("<I", output, 28, 0)
    sample = baseline.decode(
        JobResult(bytes(output), result.elapsed_ms, result.workers), depth, transactions, expected
    )
    sample.update(mode=mode, observed_wrapper_pc=pc, cache_cleanup=list(result.workers[0][14:16]))
    return sample


def one(probe, data, depth, transactions, mode, expected):
    session_start = time.monotonic_ns()
    transport = baseline.TimedTransport()
    with Device(
        dispatch="direct",
        _transport=transport,
        _power_factory=lambda: baseline.TimedPower(transport.current),
    ) as device:
        start = time.monotonic_ns()
        result = device._submit(
            probe,
            data,
            output_size=256 + transactions * 2048 + 4096,
            opcode=9,
            params=(depth, transactions, mode),
            use_direct=True,
            _completion_validator=lambda result: validate_cleanup(result, mode),
        )
        submit_ms = (time.monotonic_ns() - start) / 1e6
    session_ms = (time.monotonic_ns() - session_start) / 1e6
    sample = decode(result, depth, transactions, mode, expected)
    if len(transport.benchmarks) != 1:
        raise ValueError("ambiguous timing trace")
    trace = transport.benchmarks[0]
    sample["host_phase_ms"] = {k: v for k, v in trace.items() if k.endswith("_ms")}
    sample["powered_configuration"] = trace["powered_configuration"]
    sample["submit_wall_ms"], sample["session_wall_ms"] = submit_ms, session_ms
    return sample


def verify_fault_cleanup(probe, power, expected_power, initial_health, evidence=None):
    """Explicit guarded trap, auditing restoration before acknowledgment."""
    observed, counters = [], {"restored_jobs": 0, "restored_hmx_jobs": 0}

    def validate(result):
        validate_cleanup(result, 1)
        observed.append(result)

    audited = audited_device(power, counters)
    with audited(dispatch="direct") as device:
        try:
            device._submit(
                probe,
                baseline.operands("identity")[0],
                output_size=256 + 2048 + 4096,
                opcode=9,
                params=(1, 1, 1, 1),
                use_direct=True,
                _completion_validator=validate,
            )
        except JobFailed as exc:
            if len(observed) != 1:
                raise ValueError("fault completion was not captured") from None
            job_error = str(exc)
        else:
            raise ValueError("cached trap did not report a guarded fault")
    result = observed[0]
    row = result.workers[0]
    record = {
        "worker_status": [list(status) for status in result.workers],
        "job_error": job_error,
        "error": row[2],
        "fault_elr": row[4],
        "cache_cleanup": list(row[14:16]),
        "restored_hmx_jobs": counters["restored_hmx_jobs"],
    }
    if evidence is not None:
        evidence.update(record)
    if row[2] != 2 or not BASES[1] <= row[4] < BASES[1] + 4096:
        raise ValueError(f"fault did not originate in cached code: error={row[2]}, ELR={row[4]:#x}")
    if any(row[2] for row in result.workers[1:]) or counters["restored_hmx_jobs"] != 1:
        raise ValueError("fault worker or restoration audit failed")
    after = health(power, expected_power)
    if after != initial_health:
        raise ValueError("post-fault restoration differs")
    record["health"] = after
    return record


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    parser.add_argument("--probe", required=True)
    parser.add_argument("--modes", choices=MODES, nargs="+", default=["uncached"])
    parser.add_argument("--samples", type=int, default=50)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--depths", type=int, nargs="+", default=[1, 32, 128, 512])
    parser.add_argument("--transactions", type=int, default=4)
    parser.add_argument("--cases", choices=baseline.CASES, nargs="+", default=list(baseline.CASES))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--verify-fault-cleanup", action="store_true")
    args = parser.parse_args(argv)
    if not args.run_on_device:
        parser.error("--run-on-device is required; this publishes cached DSP code and powers HMX")
    if not 1 <= args.samples <= 100 or not 0 <= args.warmup <= 10:
        parser.error("require 1..100 samples and 0..10 warmups")
    if not 1 <= args.transactions <= 4 or any(not 1 <= n <= 512 for n in args.depths):
        parser.error("require 1..4 transactions and depths in 1..512")
    if args.verify_fault_cleanup and "cached" not in args.modes:
        parser.error("--verify-fault-cleanup requires --modes cached (or both modes)")
    if os.geteuid() != 0:
        parser.error("run with sudo for /dev/mem access")
    probe = Job.load(args.probe)
    if not probe.requires_hmx or not probe.supports_direct:
        parser.error("probe requires HMX and direct-entry flags")
    operands = {case: baseline.operands(case) for case in dict.fromkeys(args.cases)}
    references = {
        (case, depth): baseline.reference(operands[case][1], depth)
        for case in operands
        for depth in sorted(set(args.depths))
    }
    selections = [
        (case, depth, mode) for case, depth in references for mode in dict.fromkeys(args.modes)
    ]
    report = {
        "ref_hz": baseline.REF_HZ,
        "code_bytes": len(probe.code),
        "code_sha256": hashlib.sha256(probe.code).hexdigest(),
        "case_input_sha256": {
            case: hashlib.sha256(value[0]).hexdigest() for case, value in operands.items()
        },
        "samples_per_point": args.samples,
        "warmups_per_point": args.warmup,
        "mode_names": MODES,
        "samples": [],
        "checked_warmup_jobs": 0,
        "timing_scope": "internal baseline HMX windows exclude guarded cache publication and cleanup",
        "entry_contract": "entry_cached.S; worker-0 cleanup markers checked before reclamation",
    }
    failed = False
    rng = random.Random(0x43414348)
    with PowerSnapshot() as power:
        expected_power = power.read()
        try:
            report["initial_health"] = health(power, expected_power)
            if args.verify_fault_cleanup:
                report["fault_cleanup_check"] = {}
                verify_fault_cleanup(
                    probe,
                    power,
                    expected_power,
                    report["initial_health"],
                    report["fault_cleanup_check"],
                )
            for index in range(args.samples + args.warmup):
                order = list(selections)
                rng.shuffle(order)
                for case, depth, mode in order:
                    sample = one(
                        probe,
                        operands[case][0],
                        depth,
                        args.transactions,
                        MODES[mode],
                        references[(case, depth)],
                    )
                    if health(power, expected_power) != report["initial_health"]:
                        raise ValueError("post-session restoration differs")
                    configuration = sample["powered_configuration"]
                    if "powered_configuration" not in report:
                        report["powered_configuration"] = configuration
                    elif configuration != report["powered_configuration"]:
                        raise ValueError("powered clock configuration changed during comparison")
                    sample.update(
                        case=f"{case}:{mode}", input_case=case, sample_index=index - args.warmup
                    )
                    if index < args.warmup:
                        report["checked_warmup_jobs"] += 1
                    else:
                        report["samples"].append(sample)
                if index == args.warmup - 1 or (index - args.warmup + 1) % 10 == 0:
                    print(
                        f"cached A/B: {len(report['samples'])} measured jobs",
                        file=sys.stderr,
                        flush=True,
                    )
            report["summary"] = baseline.summarize(report["samples"])
            report["final_health"] = health(power, expected_power)
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
                    "resident_slopes": report.get("summary", {}).get("resident_slopes"),
                },
                indent=2,
            )
        )
    else:
        print(encoded, end="")
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
