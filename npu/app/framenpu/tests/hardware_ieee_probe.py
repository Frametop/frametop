"""Bounded IEEE HVX smoke/control experiments; numeric checks follow restoration.

Transport identity, fault location and cache cleanup are validated before
reclamation. Arithmetic mismatches are reported after the audited transaction
has restored, so an incorrect numerical oracle does not poison healthy state.
Unexpected native faults stop the experiment without retry or recovery.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Device, Job, JobFailed, RecoveryRequired
from framenpu.resident_gemm import SSR, RECORD, _lease
from framenpu.resident_landmark import CONTROL, ARENA_START, ARENA_END
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health
from hardware_landmark import digest, service
from hmx_multi_run import failure_snapshot

NAMES = ("qfloat", "ieee_fp32", "fp16_fp32", "fp16", "qfloat_accumulate", "fp16_qfloat_accumulate")


def rounded(v, half=False):
    code = "e" if half else "f"
    try:
        return struct.unpack("<" + code, struct.pack("<" + code, float(v)))[0]
    except OverflowError:
        return math.copysign(math.inf, v)


def payload(x, w, bias=0.125):
    x, w = list(x), list(w)
    # Keep native conversion out of this instruction smoke; real-layer pilots
    # include conversion/staging and report that cost explicitly.
    hx, hw = [rounded(v, True) for v in x], [rounded(v, True) for v in w]
    raw = struct.pack("<192f", *([bias] * 64 + x + w))
    return raw + struct.pack("<192e", *(hx + hw + [bias] * 64)), hx, hw


def expected(x, w, bias, repeats, kind):
    result = [bias] * 64
    if kind in (2, 3, 5):
        x, w = [rounded(v, True) for v in x], [rounded(v, True) for v in w]
    for _ in range(repeats):
        result = [
            rounded(a + (u * v if kind >= 2 else rounded(u * v)), kind == 3)
            for a, u, v in zip(result, x, w)
        ]
    return result


def decode_values(raw, kind, layout="even_odd"):
    values = list(
        struct.unpack("<64e" if kind == 3 else "<64f", raw[256:384] if kind == 3 else raw[256:512])
    )
    if kind in (2, 5) and layout == "even_odd":
        values = [values[i // 2 + 32 * (i % 2)] for i in range(64)]
    return values


def errors(actual, reference):
    finite = [(a, b) for a, b in zip(actual, reference) if math.isfinite(a) and math.isfinite(b)]
    diffs = [abs(a - b) for a, b in finite]
    return {
        "max_abs": max(diffs, default=0),
        "rms": math.sqrt(sum(v * v for v in diffs) / max(len(diffs), 1)),
        "nonfinite_actual": sum(not math.isfinite(a) for a in actual),
        "matching_values": sum(a == b for a, b in zip(actual, reference)),
        "values": len(actual),
    }


def completion(result, job, *, fault=False, reject=False, output_bytes=512):
    """Check resource/completion integrity independently of numerical accuracy."""
    try:
        if len(result.workers) != 3 or len(result.output) != output_bytes:
            raise ValueError("incomplete result")
        for i, row in enumerate(result.workers):
            if (
                len(row) != 16
                or not row[0]
                or row[0] != result.workers[0][0]
                or row[1] != i
                or row[7] != SSR[i]
                or tuple(row[14:16]) != ((1, 1) if i == 0 else (0, 0))
            ):
                raise ValueError("worker identity/SSR/cache cleanup differs")
            if i and (row[2] or any(row[3:6])):
                raise ValueError("unexpected idle-worker fault")
        row = result.workers[0]
        if (
            row[8] != 0x87000000
            or result.kernel_reference_ticks is None
            or not 0 < result.kernel_reference_ticks < 768000
        ):
            raise ValueError("EVB/timing differs")
        if row[2] == 2:
            if not 0x87A92000 <= row[4] < 0x87A92000 + len(job.code):
                raise ValueError("native fault PC outside probe")
            if fault and (
                row[3] & 255 != 37 or struct.unpack_from("<I", result.output)[0] != 0xDECAFBAD
            ):
                raise ValueError("guarded trap/sentinel differs")
        elif row[2] != (1 if reject else 0) or any(row[3:6]):
            raise ValueError("native status differs")
    except Exception as exc:
        raise RecoveryRequired("IEEE probe completion integrity invalid; retain resources") from exc


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    for name in ("probe", "output"):
        p.add_argument("--" + name, required=True, type=Path)
    args = p.parse_args()
    if not args.run_on_device or os.geteuid() != 0:
        p.error("--run-on-device and Frame root required")
    job = Job.load(args.probe)
    build = json.loads(args.probe.with_name("build.json").read_text())
    if hashlib.sha256(job.code).hexdigest() != build["sha256"]:
        p.error("build/image hash differs")
    for name, sha in build["sources"].items():
        if digest(ROOT / name) != sha:
            p.error("probe source pin differs")
    report = {
        "format": "framenpu-ieee-arithmetic-smoke",
        "build": build,
        "source_sha256": {
            n: digest(ROOT / n)
            for n in (
                "tests/hardware_ieee_probe.py",
                "src/framenpu/device.py",
                "src/framenpu/transport.py",
            )
        },
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
        "samples": [],
        "service_before": service(),
    }
    lease = _lease()
    failed = False
    try:
        with PowerSnapshot() as power:
            baseline = power.read()
            report["initial_health"] = settled_health(power, baseline)
            try:
                with Device() as observer:
                    if bytes(observer._transport.memory[CONTROL : CONTROL + 8]) == RECORD:
                        raise ValueError("resident refusal record present")
                    before = bytes(observer._transport.memory[ARENA_START:ARENA_END])
                report["arena_before_sha256"] = hashlib.sha256(before).hexdigest()
                with audited_device(power, report["audits"])(dispatch="direct") as device:

                    def run(kind, data, n=1, fault=False, reject=False):
                        captured = []

                        def validate(result):
                            completion(result, job, fault=fault, reject=reject)
                            captured.append(result)

                        try:
                            device._submit(
                                job,
                                data,
                                opcode=98,
                                params=(1, kind, n, 0, 0, int(fault)),
                                output_size=512,
                                use_direct=True,
                                _completion_validator=validate,
                            )
                        except JobFailed:
                            if not captured:
                                raise
                        result = captured[0]
                        if fault or reject:
                            if result.workers[0][2] != (2 if fault else 1):
                                raise ValueError("expected probe rejection/trap missing")
                            return result
                        if result.workers[0][2]:
                            report["unexpected_native_fault"] = list(result.workers[0])
                            raise ValueError(
                                "native arithmetic fault returned with confirmed cleanup; stopped"
                            )
                        meta = struct.unpack_from("<64I", result.output)
                        body = (meta[6] | meta[7] << 32) - (meta[4] | meta[5] << 32)
                        if (
                            meta[0] != 0x49454531
                            or not 0x87A92000 <= meta[1] < 0x87A92000 + len(job.code)
                            or meta[2] != kind
                            or meta[3] != n
                            or meta[8] != n
                            or meta[9] != 0xDECAFBAD
                            or any(meta[10:])
                            or not 0 <= body <= result.kernel_reference_ticks
                        ):
                            raise ValueError(
                                "probe metadata or budget differs after clean restoration"
                            )
                        return result, body

                    x = [((i % 13) - 6) / 8 for i in range(64)]
                    w = [((i * 5 % 17) - 8) / 16 for i in range(64)]
                    data, _, _ = payload(x, w)
                    # Echo controls distinguish instruction behavior from bad
                    # upload/readback. IEEE is optional on actual Hexagon cores.
                    result, _ = run(6, data)
                    if result.output[256:] != data[:256]:
                        raise ValueError("FP32 echo differs")
                    result, _ = run(7, data)
                    if result.output[256:] != data[768:1024]:
                        raise ValueError("half echo differs")
                    report["echo_controls_passed"] = True
                    report["ieee_observations"] = []
                    for kind in (1, 2, 3):
                        result, _ = run(kind, data)
                        report["ieee_observations"].append(
                            {
                                "kind": NAMES[kind],
                                "all_zero": not any(result.output[256:]),
                                "raw_result_hex": result.output[256:].hex(),
                            }
                        )
                    report["ieee_all_zero"] = all(
                        x["all_zero"] for x in report["ieee_observations"]
                    )
                    if not report["ieee_all_zero"]:
                        raise ValueError(
                            "IEEE behavior differs from the characterized all-zero configuration; review raw lanes"
                        )
                    report["ieee_arithmetic_usable"] = False
                    report["ieee_interpretation"] = (
                        "Optional IEEE extension is operationally unavailable in this configuration; inferred from all-zero results with passing echo and QFloat controls. Hardware absence vs platform disablement is not established."
                    )
                    # Use supported instructions: retain QFloat accumulators and
                    # convert only after the reduction. This changes rounding.
                    result, _ = run(5, data)
                    oracle = expected(x, w, 0.125, 1, 5)
                    layouts = [
                        name
                        for name in ("even_odd", "contiguous")
                        if errors(decode_values(result.output, 5, name), oracle)["max_abs"] < 1e-6
                    ]
                    if len(layouts) != 1:
                        report["mixed_qfloat_raw_hex"] = result.output[256:].hex()
                        raise ValueError("QFloat mixed product lane mapping differs")
                    layout = layouts[0]
                    report["mixed_pair_layout"] = layout
                    for kind in (0, 4, 5):
                        for repeats in (1, 32, 256, 4096):
                            for sample in range(3):
                                result, body = run(kind, data, repeats)
                                actual = decode_values(result.output, kind, layout)
                                oracle = expected(x, w, 0.125, repeats, kind)
                                error = errors(actual, oracle)
                                # Conservative operation-count error bound,
                                # scaled by absolute term magnitudes (not a
                                # relative bound near cancellation).
                                limit = (
                                    max(0.125 + repeats * abs(u * v) for u, v in zip(x, w))
                                    * (2 * repeats + 2)
                                    * 2**-22
                                )
                                if error["nonfinite_actual"] or error["max_abs"] > limit:
                                    raise ValueError(
                                        "QFloat experiment exceeds fixture rounding bound"
                                    )
                                report["samples"].append(
                                    {
                                        "kind": kind,
                                        "name": NAMES[kind],
                                        "repeats": repeats,
                                        "sample": sample,
                                        "body_ticks": body,
                                        "wrapper_ms": result.kernel_reference_ticks / 19200,
                                        "error": error,
                                        "max_abs_bound": limit,
                                    }
                                )
                        print(json.dumps({"kind": NAMES[kind], "passed": True}), flush=True)
                    # Characterize underflow/range behavior separately, not a model
                    # tolerance qualification or an assumption of IEEE subnormals.
                    half_bits = [0, 0x8000, 1, 0x3FF, 0x400, 0x3C00, 0xBC00, 0x7BFF, 0xFBFF]
                    edge = [
                        struct.unpack("<e", struct.pack("<H", half_bits[i % len(half_bits)]))[0]
                        for i in range(64)
                    ]
                    edge_w = [0.5 if i % 2 else 2.0 for i in range(64)]
                    edge_data, _, _ = payload(edge, edge_w, 0.0)
                    report["range_observations"] = []
                    for kind in (0, 4, 5):
                        result, _ = run(kind, edge_data)
                        actual = decode_values(result.output, kind, layout)
                        report["range_observations"].append(
                            {
                                "kind": NAMES[kind],
                                "error": errors(actual, expected(edge, edge_w, 0.0, 1, kind)),
                                "raw_result_hex": result.output[256:].hex(),
                            }
                        )
                    float_bits = [
                        0,
                        0x80000000,
                        1,
                        0x007FFFFF,
                        0x00800000,
                        0x3F800000,
                        0xBF800000,
                        0x7F7FFFFF,
                        0xFF7FFFFF,
                    ]
                    float_edge = [
                        struct.unpack("<f", struct.pack("<I", float_bits[i % len(float_bits)]))[0]
                        for i in range(64)
                    ]
                    float_data, _, _ = payload(float_edge, edge_w, 0.0)
                    result, _ = run(4, float_data)
                    report["range_observations"].append(
                        {
                            "kind": "qfloat_full_range",
                            "error": errors(
                                decode_values(result.output, 1),
                                expected(float_edge, edge_w, 0.0, 1, 1),
                            ),
                            "raw_result_hex": result.output[256:].hex(),
                        }
                    )
                    for kind in (0, 4, 5):
                        run(kind, data, fault=True)
                    run(8, data, reject=True)
                    run(1, data, n=0, reject=True)
                    run(1, data, n=4097, reject=True)
                    report["guarded_faults"] = 3
                    report["rejections"] = 3
                    # Successful arithmetic after guarded faults/rejections.
                    result, _ = run(5, data)
                    if (
                        errors(
                            decode_values(result.output, 5, layout), expected(x, w, 0.125, 1, 5)
                        )["max_abs"]
                        > 1e-6
                    ):
                        raise ValueError("post-fault arithmetic differs")
                with Device() as observer:
                    after = bytes(observer._transport.memory[ARENA_START:ARENA_END])
                report["arena_after_sha256"] = hashlib.sha256(after).hexdigest()
                if after != before:
                    raise ValueError("resident arena changed")
                report["final_health"] = settled_health(power, baseline)
                report["service_after"] = service()
                if report["service_before"] != report["service_after"]:
                    raise ValueError("DSP service changed")
                report["all_checks_passed"] = True
            except BaseException as exc:
                failed = True
                report["error"] = f"{type(exc).__name__}: {exc}"
                report["after_failure"] = failure_snapshot(power)
                report["interpretation"] = "stopped without retry/recovery/restart"
    finally:
        lease.close()
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            {
                "all_checks_passed": report.get("all_checks_passed", False),
                "error": report.get("error"),
            }
        ),
        flush=True,
    )
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
