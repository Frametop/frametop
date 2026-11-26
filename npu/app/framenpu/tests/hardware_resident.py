"""Opt-in repeated resident GEMM, stale-command, fault and teardown qualification."""

import argparse
import hashlib
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
from framenpu import Device, DeviceBusy, Job, JobFailed, RecoveryRequired
from framenpu.errors import FrameNPUError
from framenpu.resident_gemm import ResidentGemm, _lease, proof_layout
from framenpu.provenance import verify_kernel
from hardware_hmx import PowerSnapshot, audited_device, health
from hmx_baseline_run import percentiles
from hmx_multi_run import failure_snapshot


def settled_health(power, baseline):
    """Read-only post-close idle observation, bounded by the existing 40 ms.

    After releasing an unsubmitted gate, stock processing state 2 was observed
    briefly with no command pending, owner 1 and callback zero. Never retry a
    power/prefix mismatch, active request, callback or poisoned transport.
    """
    deadline = time.monotonic() + 0.04
    while True:
        try:
            return health(power, baseline)
        except RecoveryRequired as exc:
            if not str(exc).startswith("unexpected post-session control:"):
                raise
            with Device() as observer:
                state = observer.status()
            if (
                state["control"] not in ([1, 0, 1], [2, 0, 1])
                or state["callback"]
                or state["poisoned"]
                or time.monotonic() >= deadline
            ):
                raise
            time.sleep(0.0001)


def operands(k, seed):
    rng = random.Random(0x5247454D + seed)
    weights = [[rng.randint(-4, 4) for _ in range(32)] for _ in range(k)]
    blocks = [
        struct.pack("<1024b", *(v for row in weights[t : t + 32] for v in row))
        for t in range(0, k, 32)
    ]
    if len(set(blocks)) != k // 32:
        raise ValueError("depth weight blocks are not distinct")
    return weights


def inputs_and_reference(weights, seed):
    k = len(weights)
    rng = random.Random(0x494E5054 + seed)
    inputs = [[rng.randint(-4, 4) for _ in range(k)] for _ in range(32)]
    product = [
        sum(inputs[r][t] * weights[t][c] for t in range(k)) / 64
        for r in range(32)
        for c in range(32)
    ]
    rounded = struct.unpack("<1024e", struct.pack("<1024e", *product))
    expected = struct.pack("<1024f", *rounded)
    raw = struct.pack(f"<{32 * k}b", *(v for row in inputs for v in row))
    blocks = [
        bytes(v + 4 for r in range(32) for v in inputs[r][t : t + 32]) for t in range(0, k, 32)
    ]
    if len(set(blocks)) != k // 32:
        raise ValueError("depth activation blocks are not distinct")
    return [[v / 8 for v in row] for row in inputs], expected, hashlib.sha256(raw).hexdigest()


def qualify(probe, replacement, sizes, iterations, warmups, suite):
    report = {
        "runtime": verify_kernel(),
        "layout": proof_layout(),
        "iterations": iterations,
        "warmups": warmups,
        "sessions": [],
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
        "probe_sha256": hashlib.sha256(probe.code).hexdigest(),
        "probe_bytes": len(probe.code),
        "replacement_sha256": hashlib.sha256(replacement.code).hexdigest() if replacement else None,
    }

    def service():
        return subprocess.check_output(
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
            text=True,
        )

    report["service_before"] = service()
    with PowerSnapshot() as power:
        baseline = power.read()
        report["initial_health"] = settled_health(power, baseline)
        factory = audited_device(power, report["audits"])
        try:
            for k in sizes:
                weights = operands(k, k)
                refs = []
                owner = ResidentGemm(
                    probe, [[v / 8 for v in row] for row in weights], _device_factory=factory
                )
                with owner:
                    try:
                        second = _lease()
                    except DeviceBusy:
                        owner.metrics["second_owner_refused"] = True
                    else:
                        second.close()
                        raise ValueError("second resident owner was not refused")
                    hashes = []
                    for index in range(iterations + warmups):
                        a, expected, digest = inputs_and_reference(weights, k * 1000 + index)
                        owner.run(a, _expected=expected)
                        refs.append(
                            {
                                "input_sha256": digest,
                                "output_sha256": hashlib.sha256(expected).hexdigest(),
                                "warmup": index < warmups,
                            }
                        )
                        hashes.append(digest)
                    if len(set(hashes)) != len(hashes):
                        raise ValueError("stream inputs repeat")
                    if suite:
                        rejects = []
                        for name, change in (
                            ("sequence", None),
                            ("nonce", None),
                            ("identity", None),
                            ("weight_offset", 18),
                            ("input_length", 21),
                            ("output_offset", 22),
                            ("depth", 3),
                        ):
                            descriptor = owner._descriptor(2)
                            if name == "sequence":
                                descriptor[8:10] = [
                                    owner.sequence & 0xFFFFFFFF,
                                    owner.sequence >> 32,
                                ]
                            elif name == "nonce":
                                descriptor[4] ^= 1
                            elif name == "identity":
                                descriptor[10] ^= 1
                            else:
                                descriptor[change] += 128
                            owner._command(2, descriptor=descriptor, reject=True, expected=expected)
                            rejects.append(name)
                        owner.metrics["native_rejections"] = rejects
                        a, expected, digest = inputs_and_reference(weights, k * 1000 + 999)
                        owner.run(a, _expected=expected)
                        refs.append(
                            {
                                "input_sha256": digest,
                                "output_sha256": hashlib.sha256(expected).hexdigest(),
                                "after_rejections": True,
                            }
                        )
                        a, _, _ = inputs_and_reference(weights, k * 1000 + 998)
                        try:
                            owner.run(a, _fault=True)
                        except JobFailed:
                            owner.metrics["guarded_fault_checked"] = True
                        else:
                            raise ValueError("guarded resident fault did not fail")
                        try:
                            owner.run(a)
                        except RuntimeError:
                            owner.metrics["invalid_session_refused"] = True
                        else:
                            raise ValueError("faulted resident session accepted work")
                owner.metrics.update(k=k, tag=0, references=refs)
                report["sessions"].append(owner.metrics)
                if owner.metrics["weight_uploads"] != 1:
                    raise ValueError("weights were uploaded more than once")
                settled_health(power, baseline)
                if suite:
                    # A different instruction image and new weights follow the
                    # trapped session, exercising cache replacement and reload.
                    weights = operands(k, k + 10000)
                    fresh = ResidentGemm(
                        replacement,
                        [[v / 8 for v in row] for row in weights],
                        _device_factory=factory,
                        _tag=1,
                    )
                    with fresh:
                        a, expected, digest = inputs_and_reference(weights, k + 10000)
                        fresh.run(a, _expected=expected)
                    fresh.metrics.update(
                        k=k,
                        tag=1,
                        references=[
                            {
                                "input_sha256": digest,
                                "output_sha256": hashlib.sha256(expected).hexdigest(),
                                "after_fault_and_replacement": True,
                            }
                        ],
                    )
                    report["sessions"].append(fresh.metrics)
                    settled_health(power, baseline)
                print(f"resident K={k}: checked and restored", file=sys.stderr, flush=True)
            report["final_health"] = settled_health(power, baseline)
            report["service_after"] = service()
            if report["service_before"] != report["service_after"]:
                raise ValueError("DSP service state changed")
            report["all_checks_passed"] = True
        except (FrameNPUError, ValueError, OSError) as exc:
            report["error"] = f"{type(exc).__name__}: {exc}"
            report["error_causes"] = []
            cause = exc.__cause__
            while cause is not None:
                report["error_causes"].append(f"{type(cause).__name__}: {cause}")
                cause = cause.__cause__
            report["after_failure"] = failure_snapshot(power)
            report["interpretation"] = "stopped without recovery or restart"
    summary = []
    for session in report["sessions"]:
        runs = [c for c in session["commands"] if c["op"] == 2 and not c["rejected"]]
        if session["tag"] == 0:
            runs = runs[warmups : warmups + iterations]
        summary.append(
            {
                "k": session["k"],
                "tag": session["tag"],
                "checked_outputs": len(session["references"]) * 1024,
                "phase_ms": {
                    key: percentiles([c["host_phase_ms"][key] for c in runs])
                    for key in runs[0]["host_phase_ms"]
                },
                "dsp_phase_us": {
                    key: percentiles([c["dsp"][key] / 19.2 for c in runs])
                    for key in runs[0]["dsp"]
                    if runs[0]["dsp"][key] is not None
                },
                "command_ms": percentiles([c["command_ms"] for c in runs]),
            }
        )
    report["summary"] = summary
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--replacement", type=Path)
    parser.add_argument("--phase", choices=("controls", "suite"), default="controls")
    parser.add_argument("--sizes", type=int, nargs="+", default=[32, 64, 128])
    parser.add_argument("--iterations", type=int, default=50)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    if not args.run_on_device:
        parser.error("--run-on-device is required")
    if os.geteuid() != 0:
        parser.error("run with sudo for power snapshots and HMX")
    if (
        not 1 <= args.iterations <= 100
        or not 0 <= args.warmups <= 10
        or any(k not in (32, 64, 128) for k in args.sizes)
    ):
        parser.error("require 1..100 iterations, 0..10 warmups and sizes 32/64/128")
    if args.phase == "suite" and args.replacement is None:
        parser.error("suite requires --replacement")
    probe = Job.load(args.probe)
    replacement = Job.load(args.replacement) if args.replacement else None
    if replacement is not None and replacement.code == probe.code:
        parser.error("replacement must be a different instruction image")
    report = qualify(
        probe,
        replacement,
        list(dict.fromkeys(args.sizes)),
        args.iterations,
        args.warmups,
        args.phase == "suite",
    )
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            {
                "report": str(args.output),
                "all_checks_passed": report.get("all_checks_passed", False),
                "audits": report["audits"],
                "summary": report["summary"],
                "error": report.get("error"),
            },
            indent=2,
        )
    )
    return int(not report.get("all_checks_passed"))


if __name__ == "__main__":
    sys.exit(main())
