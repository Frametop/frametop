"""Opt-in public-SDK nested-frame fault and same-session reuse qualification.

Build fault_unwind_probe.c with the ordinary SDK entry, with and without --hmx.
Controls return normally; suite adds leaf traps followed by valid work in the
same Device. Completion validation runs before DDR/power reclamation. Invalid
completion or timeout stops immediately and never triggers automatic recovery.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Job, JobFailed, RecoveryRequired
from framenpu.errors import FrameNPUError
from framenpu.profile import PROFILE
from framenpu.provenance import verify_kernel
from hardware_hmx import PowerSnapshot, audited_device, health
from hmx_multi_run import failure_snapshot, timing

MAGIC = 0x554E5744
PENDING = 0xDECAFBAD
BASELINE_SSR = (0x81840200, 0x89840200, 0x91840200)


def reference(depth):
    return (
        sum(0x13579BDF ^ (level * 0x10101) ^ (level & 7) for level in range(depth + 1)) & 0xFFFFFFFF
    )


def decode(result, probe, depth, trap):
    if not 0 <= depth <= 6 or trap not in (0, 1):
        raise ValueError("invalid unwind test parameters")
    if len(result.output) != 128 or len(result.workers) != 3:
        raise ValueError("incomplete unwind result")
    for worker, row in enumerate(result.workers):
        if len(row) != 16 or not row[0] or row[1] != worker:
            raise ValueError("invalid worker identity")
        if row[0] != result.workers[0][0] or row[2] != (2 if trap and worker == 0 else 0):
            raise ValueError("invalid completion sequence or error code")
        if row[7] != BASELINE_SSR[worker]:
            raise ValueError("worker SSR was not restored before this job")
    row = result.workers[0]
    begin, end = timing(row)
    if row[8] != 0x87000000:
        raise ValueError("exception vector base was not restored before this job")
    if trap:
        if row[3] & 255 != 37 or not row[3] & (1 << 17):
            raise ValueError("wrong trap cause or exception state")
        if bool(row[3] & (1 << 26)) != probe.requires_hmx:
            raise ValueError("wrong HMX state at trap")
        code = PROFILE.dsp_base + PROFILE.code
        if not code <= row[4] < code + len(probe.code):
            raise ValueError("trap PC is outside the probe")
    elif any(row[3:6]):
        raise ValueError("normal control recorded a fault")
    words = struct.unpack("<32I", result.output)
    frames = depth + 1
    expected = (MAGIC, 1, depth, trap, int(probe.requires_hmx), frames, 0 if trap else frames)
    if words[:7] != expected or words[7] != (PENDING if trap else reference(depth)):
        raise ValueError("C frames resumed after fault or normal checksum differs")
    stack, frame, levels = words[8 : 8 + frames], words[16 : 16 + frames], words[24 : 24 + frames]
    if levels != tuple(range(depth, -1, -1)):
        raise ValueError("nested frame order differs")
    for index, (sp, fp) in enumerate(zip(stack, frame)):
        if sp % 8 or fp % 8 or not 0x87000000 <= sp <= fp < 0x89000000:
            raise ValueError("invalid live stack/frame pointer")
        if index and fp >= stack[index - 1]:
            raise ValueError("nested C frames are not distinct")
    for start in (8, 16, 24):
        if any(words[start + frames : start + 8]):
            raise ValueError("unused output metadata was modified")
    return {
        "depth": depth,
        "trap": bool(trap),
        "hmx": probe.requires_hmx,
        "frames_entered": frames,
        "frames_returned": words[6],
        "checksum": words[7],
        "stack_span_bytes": frame[0] - stack[-1],
        "worker_error": row[2],
        "fault_registers": list(row[3:6]),
        "worker_ssr_before": [status[7] for status in result.workers],
        "saved_evb": row[8],
        "frame_job_ticks": end - begin,
        "publish_to_result_ms": result.elapsed_ms,
    }


def validate_completion(result, probe, depth, trap):
    try:
        return decode(result, probe, depth, trap)
    except ValueError as exc:
        raise RecoveryRequired("unwind completion is invalid; retain payload and power") from exc


def session(probe, depth, trap, dispatch, power, expected_power, initial_health, counters):
    records = []
    audited = audited_device(power, counters)
    with audited(dispatch=dispatch) as device:
        for mode in (1, 0) if trap else (0,):
            captured = []

            def validate(result):
                captured.append(validate_completion(result, probe, depth, mode))

            try:
                device._submit(
                    probe,
                    output_size=128,
                    opcode=10,
                    params=(depth, mode),
                    use_direct=dispatch == "direct",
                    _completion_validator=validate,
                )
            except JobFailed:
                if not mode or len(captured) != 1:
                    raise
            else:
                if mode:
                    raise ValueError("the requested trap did not fail the job")
            if len(captured) != 1:
                raise ValueError("completion was not captured exactly once")
            records.extend(captured)
    after = health(power, expected_power)
    if after != initial_health:
        raise RecoveryRequired("post-session health differs")
    return {"dispatch": dispatch, "hmx": probe.requires_hmx, "depth": depth, "jobs": records}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    parser.add_argument("--probe", required=True)
    parser.add_argument("--hmx-probe")
    parser.add_argument(
        "--dispatch", nargs="+", choices=("benchmark", "direct"), default=["benchmark"]
    )
    parser.add_argument("--phase", choices=("controls", "suite"), default="controls")
    parser.add_argument("--depths", nargs="+", type=int, default=[0, 1, 3, 6])
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    if not args.run_on_device:
        parser.error("--run-on-device is required; this submits native jobs and may power HMX")
    if os.geteuid() != 0:
        parser.error("run with sudo for read-only power snapshots and optional HMX setup")
    if not 1 <= args.repetitions <= 10 or any(not 0 <= depth <= 6 for depth in args.depths):
        parser.error("require 1..10 repetitions and depths in 0..6")
    probes = [Job.load(args.probe)]
    if probes[0].requires_hmx or not probes[0].supports_direct:
        parser.error("CPU probe must use the ordinary SDK entry without --hmx")
    if args.hmx_probe:
        hmx = Job.load(args.hmx_probe)
        if not hmx.requires_hmx or not hmx.supports_direct or hmx.code != probes[0].code:
            parser.error("HMX probe must contain identical code with --hmx metadata")
        probes.append(hmx)
    report = {
        "runtime": verify_kernel(),
        "probe_code_bytes": len(probes[0].code),
        "probe_code_sha256": hashlib.sha256(probes[0].code).hexdigest(),
        "phase": args.phase,
        "sessions": [],
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
    }
    failed = False
    with PowerSnapshot() as power:
        baseline = power.read()
        try:
            report["initial_health"] = health(power, baseline)
            phases = [False] + ([True] * args.repetitions if args.phase == "suite" else [])
            for trap in phases:
                for dispatch in dict.fromkeys(args.dispatch):
                    for probe in probes:
                        for depth in sorted(set(args.depths)):
                            record = session(
                                probe,
                                depth,
                                trap,
                                dispatch,
                                power,
                                baseline,
                                report["initial_health"],
                                report["audits"],
                            )
                            report["sessions"].append(record)
                print(
                    f"unwind: {len(report['sessions'])} checked sessions",
                    file=sys.stderr,
                    flush=True,
                )
            report["final_health"] = health(power, baseline)
            report["all_checks_passed"] = True
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
                    "sessions": len(report["sessions"]),
                    "all_checks_passed": report.get("all_checks_passed", False),
                    "audits": report["audits"],
                    "error": report.get("error"),
                },
                indent=2,
            )
        )
    else:
        print(encoded, end="")
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
