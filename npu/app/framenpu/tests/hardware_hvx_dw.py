"""Device run of the HVX FP16 depthwise probe (tests/hvx_dw_probe.c).

Times every stride-1 depthwise layer of the public v20a teacher at its real
shape with operands resident in borrowed VTCM, QFloat16 arithmetic and
deterministic synthetic activations/weights (timing does not depend on the
values; no recordings). Per layer: a checked warmup, then rounds alternating
1 and 4 compute repetitions; the per-pass cost is the repetition slope.
Every output is checked against an FP64 oracle with an FP16/QFloat bound.

Protections match tests/hardware_hmx_chain.py: arena lease, refusal-record
check, the window [0x1a40000, 0x1f00000) saved before any write and restored
at the end with the whole arena hash compared, audited direct dispatch, and
one guarded fault, one out-of-window rejection and a clean post-fault run.
Any unexpected result stops without retry, recovery or service restart.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tests"))
from framenpu import Device, Job, JobFailed
from framenpu.resident_gemm import _lease, RECORD
from framenpu.resident_landmark import CONTROL, ARENA_START, ARENA_END
from hardware_ieee_probe import completion
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health
from hardware_landmark import digest, service
from hmx_multi_run import failure_snapshot
import hvx_dw_layout as layout_mod
from hvx_dw_layout import (
    MAGIC,
    META_BYTES,
    OPCODE,
    STRIDE2_MACS,
    TEACHER_STRIDE1,
    WIN_HI,
    WIN_LO,
    arena_image,
    check_output,
    descriptor,
    fixture,
)

TICKS_PER_US = 19.2


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    p.add_argument("--jobs", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--rounds", type=int, default=5)
    p.add_argument("--kernel", choices=("v1", "v2"), default="v1")
    args = p.parse_args()
    if not args.run_on_device or os.geteuid() != 0 or not 1 <= args.rounds <= 20:
        p.error("Frame root, --run-on-device and rounds in 1..20 required")
    job = Job.load(args.jobs / "probe.fjob")
    build = json.loads((args.jobs / "build.json").read_text())
    if hashlib.sha256(job.code).hexdigest() != build["sha256"]:
        p.error("build/image hash differs")
    for name, sha in build["sources"].items():
        if digest(ROOT / name) != sha:
            p.error(f"source pin differs: {name}")
    report = {
        "format": "framenpu-hvx-dw-probe",
        "kernel": args.kernel,
        "build": build,
        "source_sha256": {
            n: digest(ROOT / n) for n in ("tests/hardware_hvx_dw.py", "tests/hvx_dw_layout.py")
        },
        "conditions": "synthetic deterministic activations and weights; headset idle",
        "window": [f"{WIN_LO:#x}", f"{WIN_HI:#x}"],
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
        "layers": {},
        "service_before": service(),
    }
    failed = False
    lease = _lease()
    try:
        with PowerSnapshot() as power:
            baseline = power.read()
            report["initial_health"] = settled_health(power, baseline)
            try:
                with Device() as observer:
                    mem = observer._transport.memory
                    if bytes(mem[CONTROL : CONTROL + 8]) == RECORD:
                        raise ValueError("resident refusal record present")
                    before = bytes(mem[ARENA_START:ARENA_END])
                report["arena_before_sha256"] = hashlib.sha256(before).hexdigest()
                saved_window = before[WIN_LO - ARENA_START : WIN_HI - ARENA_START]
                try:
                    with audited_device(power, report["audits"])(dispatch="direct") as dev:
                        mem = dev._transport.memory
                        for i, layer in enumerate(TEACHER_STRIDE1):
                            report["layers"][layer.name] = run_layer(
                                dev,
                                mem,
                                job,
                                layer,
                                args.rounds,
                                controls=i == 0,
                                kernel=args.kernel,
                            )
                            args.output.write_text(json.dumps(report, indent=1) + "\n")
                finally:
                    with Device() as observer:
                        mem = observer._transport.memory
                        mem[WIN_LO:WIN_HI] = saved_window
                        after = bytes(mem[ARENA_START:ARENA_END])
                report["arena_after_sha256"] = hashlib.sha256(after).hexdigest()
                if after != before:
                    raise ValueError("resident arena differs after window restoration")
                report["summary"] = summary(report["layers"])
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
        args.output.write_text(json.dumps(report, indent=1) + "\n")
    print(
        json.dumps(
            {
                "all_checks_passed": report.get("all_checks_passed", False),
                "error": report.get("error"),
                "summary": report.get("summary"),
            },
            indent=1,
        ),
        flush=True,
    )
    return int(failed)


def summary(layers):
    total_us = sum(v["pass_us"] for v in layers.values())
    macs = sum(v["macs"] for v in layers.values())
    rate = macs / total_us  # MAC per us
    return {
        "stride1_pass_ms": total_us / 1000,
        "stride1_macs": macs,
        "mmac_per_ms": rate / 1000,
        "ns_per_tap_vector": 1000 * total_us / (macs / 64),
        "stride2_scaled_ms": STRIDE2_MACS / rate / 1000,
        "all_depthwise_scaled_ms": (macs + STRIDE2_MACS) / rate / 1000,
        "copy_in_ms": sum(v["copy_in_us"] for v in layers.values()) / 1000,
        "copy_out_ms": sum(v["copy_out_us"] for v in layers.values()) / 1000,
        "splat_ms": sum(v["splat_us"] for v in layers.values()) / 1000,
        "cycles_per_tap_vector": sum(
            v["cycles_per_tap_vector"] * v["macs"] for v in layers.values()
        )
        / macs,
        "note": "stride-2 scaled at the measured stride-1 MAC rate (not measured)",
    }


KERNELS = {
    "v1": dict(
        magic=MAGIC,
        opcode=OPCODE,
        image=arena_image,
        desc=descriptor,
        check=check_output,
        nj=lambda L: L.nj,
        out_word=11,
    ),
    "v2": dict(
        magic=layout_mod.MAGIC2,
        opcode=layout_mod.OPCODE2,
        image=layout_mod.v2_arena_image,
        desc=layout_mod.v2_descriptor,
        check=layout_mod.v2_check_output,
        nj=lambda L: layout_mod.v2_shape(L)[1],
        out_word=10,
    ),
}


def run_layer(dev, mem, job, layer, rounds, *, controls, kernel="v1"):
    import numpy as np

    K = KERNELS[kernel]
    x, wt, b = fixture(layer)
    plan, writes = K["image"](layer, x, wt, b)
    desc = K["desc"](layer, plan)
    for off, data in writes.items():
        mem[off : off + len(data)] = data
    out_off, out_size = plan["out"]

    def submit(reps, data=desc, *, fault=False, reject=False):
        seen = []

        def validate(result):
            completion(result, job, fault=fault, reject=reject, output_bytes=META_BYTES)
            seen.append(result)

        mem[out_off : out_off + out_size] = bytes(out_size)
        t0 = time.perf_counter()
        try:
            res = dev._submit(
                job,
                data,
                opcode=K["opcode"],
                params=(1, reps, 0, 0, 0, int(fault)),
                output_size=META_BYTES,
                use_direct=True,
                _completion_validator=validate,
            )
        except JobFailed:
            if not seen or not (fault or reject) or seen[0].workers[0][2] != (2 if fault else 1):
                raise
            return None
        wall = time.perf_counter() - t0
        if fault or reject:
            raise ValueError("expected fault/rejection missing")
        m = struct.unpack_from("<256I", res.output)
        if (
            m[0] != K["magic"]
            or not 0x87A92000 <= m[1] < 0x87A92000 + len(job.code)
            or tuple(m[2:4]) != (reps, K["nj"](layer))
            or any(m[64:96])
            or any(m[9 if kernel == "v2" else 8 : 32])
            or not 0 < m[4] + m[5] + m[6] + m[8] <= res.kernel_reference_ticks
        ):
            raise ValueError(f"dw metadata/finite/budget differs: {[hex(w) for w in m[:8]]}")
        accuracy = K["check"](layer, bytes(mem[out_off : out_off + out_size]), x, wt, b)
        return {
            "reps": reps,
            "wall_ms": wall * 1000,
            "wrapper_ms": res.kernel_reference_ticks / 19200,
            "copy_in_us": m[4] / TICKS_PER_US,
            "compute_us": m[5] / TICKS_PER_US,
            "copy_out_us": m[6] / TICKS_PER_US,
            "splat_us": m[8] / TICKS_PER_US,
            "compute_cycles": m[7],
            "core_mhz": m[7] / (m[5] / TICKS_PER_US) if m[5] else None,
            "max_error_to_bound_ratio": accuracy["max_error_to_bound_ratio"],
        }

    entry = {
        "shape": vars(layer) | {"pad": layer.pad, "nbin": layer.nbin, "nj": layer.nj},
        "macs": layer.macs,
    }
    submit(1)  # checked warmup, excluded
    runs = []
    for round_ in range(rounds):
        for reps in (1, 4) if round_ % 2 == 0 else (4, 1):
            runs.append(submit(reps) | {"round": round_})
    if controls:
        submit(1, fault=True)
        bad = bytearray(desc)
        struct.pack_into("<I", bad, K["out_word"] * 4, WIN_HI)  # output region outside the window
        submit(1, bytes(bad), reject=True)
        submit(1)
        entry["controls"] = {"guarded_fault": 1, "window_rejection": 1, "post_fault_valid": 1}
    med = lambda xs: float(np.median(xs))  # noqa: E731
    one = [r for r in runs if r["reps"] == 1]
    four = [r for r in runs if r["reps"] == 4]
    slopes = [(f["compute_us"] - o["compute_us"]) / 3 for o, f in zip(one, four)]
    entry |= {
        "runs": runs,
        "pass_us": med(slopes),
        "pass_us_single": med([r["compute_us"] for r in one]),
        "copy_in_us": med([r["copy_in_us"] for r in one]),
        "copy_out_us": med([r["copy_out_us"] for r in one]),
        "splat_us": med([r["splat_us"] for r in one]),
        "wrapper_ms_reps1": med([r["wrapper_ms"] for r in one]),
        "max_error_to_bound_ratio": max(r["max_error_to_bound_ratio"] for r in runs),
    }
    entry["ns_per_tap_vector"] = 1000 * entry["pass_us"] / (layer.macs / 64)
    entry["core_mhz"] = med([r["core_mhz"] for r in runs if r["core_mhz"]] or [0])
    cyc = [(f["compute_cycles"] - o["compute_cycles"]) / 3 for o, f in zip(one, four)]
    entry["cycles_per_tap_vector"] = med(cyc) / (layer.macs / 64)
    print(
        json.dumps(
            {
                k: entry[k]
                for k in (
                    "pass_us",
                    "copy_in_us",
                    "copy_out_us",
                    "ns_per_tap_vector",
                    "core_mhz",
                    "cycles_per_tap_vector",
                )
            }
            | {"layer": layer.name}
        ),
        flush=True,
    )
    return entry


if __name__ == "__main__":
    sys.exit(main())
