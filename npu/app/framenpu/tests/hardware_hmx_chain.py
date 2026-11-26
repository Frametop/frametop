"""Device run of the layer-chain executor prototype (tests/hmx_layer_chain_probe.c).

Runs real-shaped stride-1 fused3_w050 chains (5 layers at 14x14, 4 at 7x7)
end to end on the DSP: weights, biases, tables and activations live in a
window of the persistent DDR arena, every layer writes the next layer's
input, and the host only validates. Two schedules per chain, alternated
over rounds: the whole chain in one job, and one job per layer. Synthetic
deterministic data only (no trained student exists; no recordings).

Protections are the audited direct-dispatch ones of the HMX probes, plus
the arena: the arena lease is held, the window [0x1a40000, 0x1f00000) is
saved before any write and restored at the end, and the whole 6 MiB arena
must then equal its initial bytes. Guarded faults and an out-of-window
descriptor rejection are exercised per chain. Any unexpected result stops
the run without retry, recovery, gate clearing or service restart.
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
from hmx_chain_layout import (
    CHAINS,
    MAGIC,
    META_BYTES,
    OPCODE,
    WIN_HI,
    WIN_LO,
    arena_image,
    check_layer,
    descriptors,
    fixture,
)

TICKS_PER_US = 19.2


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    p.add_argument("--jobs", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--rounds", type=int, default=5)
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
    if not ARENA_START < WIN_LO < WIN_HI <= ARENA_END - 4096:
        p.error("window outside the arena")
    report = {
        "format": "framenpu-hmx-chain-probe",
        "build": build,
        "source_sha256": {
            n: digest(ROOT / n)
            for n in ("tests/hardware_hmx_chain.py", "tests/hmx_chain_layout.py")
        },
        "conditions": "synthetic deterministic activations and weights; headset idle",
        "window": [f"{WIN_LO:#x}", f"{WIN_HI:#x}"],
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
        "chains": {},
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
                        for name, layers in CHAINS.items():
                            report["chains"][name] = run_chain(
                                dev, mem, job, name, layers, args.rounds
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
                "summary": {k: v.get("summary") for k, v in report["chains"].items()},
            },
            indent=1,
        ),
        flush=True,
    )
    return int(failed)


def run_chain(dev, mem, job, name, layers, rounds):
    import numpy as np

    x0, weights = fixture(layers)
    plan, writes = arena_image(layers, x0, weights)
    desc = descriptors(layers, plan)
    for off, data in writes.items():
        mem[off : off + len(data)] = data
    nl = len(layers)
    entry = {
        "layers": [
            vars(layer) | {"nj": layer.nj, "nm": layer.nm, "nk1": layer.nk1} for layer in layers
        ],
        "arena_bytes": plan["end"] - WIN_LO,
        "descriptor_sha256": hashlib.sha256(desc).hexdigest(),
        "runs": [],
    }

    def submit(l0, l1, data=desc, *, fault=False, reject=False):
        seen = []

        def validate(result):
            completion(result, job, fault=fault, reject=reject, output_bytes=META_BYTES)
            seen.append(result)

        t0 = time.perf_counter()
        try:
            res = dev._submit(
                job,
                data,
                opcode=OPCODE,
                params=(1, l0, l1, 0, 0, int(fault)),
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
        want_blocks = sum(layer.nj for layer in layers[l0:l1])
        body = (m[9] - m[8]) & 0xFFFFFFFF
        if (
            m[0] != MAGIC
            or not 0x87A92000 <= m[1] < 0x87A92000 + len(job.code)
            or tuple(m[2:8]) != (nl, l0, l1, l1, 0, want_blocks)
            or any(m[64:96])
            or any(m[10:32])
            or not 0 < body <= res.kernel_reference_ticks
        ):
            raise ValueError(f"chain metadata/finite/budget differs: {[hex(w) for w in m[:12]]}")
        return {
            "l0": l0,
            "l1": l1,
            "wall_ms": wall * 1000,
            "wrapper_ms": res.kernel_reference_ticks / 19200,
            "body_us": body / TICKS_PER_US,
            "prologue_us": [m[32 + i] / TICKS_PER_US for i in range(l0, l1)],
            "blocks_us": [m[48 + i] / TICKS_PER_US for i in range(l0, l1)],
        }

    def clear_outputs():
        for off, size in plan["act"][1:]:
            mem[off : off + size] = bytes(size)

    def validate_all():
        acc = []
        for i, layer in enumerate(layers):
            (oi, ni), (oo, no) = plan["act"][i], plan["act"][i + 1]
            acc.append(
                check_layer(layer, bytes(mem[oi : oi + ni]), bytes(mem[oo : oo + no]), *weights[i])
            )
        return acc

    schedules = {"whole": [(0, nl)], "per_layer": [(i, i + 1) for i in range(nl)]}
    for warm in schedules.values():  # checked warmups, excluded from timing
        clear_outputs()
        for l0, l1 in warm:
            submit(l0, l1)
        validate_all()
    for round_ in range(rounds):
        order = list(schedules) if round_ % 2 == 0 else list(schedules)[::-1]
        for sched in order:
            clear_outputs()
            jobs = [submit(l0, l1) for l0, l1 in schedules[sched]]
            accuracy = validate_all()
            entry["runs"].append(
                {
                    "schedule": sched,
                    "round": round_,
                    "jobs": jobs,
                    "wall_ms": sum(j["wall_ms"] for j in jobs),
                    "wrapper_ms": sum(j["wrapper_ms"] for j in jobs),
                    "body_us": sum(j["body_us"] for j in jobs),
                    "max_error_to_bound_ratio": max(
                        a["max_error_to_bound_ratio"] for a in accuracy
                    ),
                    "output_abs_max": max(a["output_abs_max"] for a in accuracy),
                }
            )
    # Controls: guarded fault in the first block, out-of-window descriptor, clean rerun.
    submit(0, nl, fault=True)
    bad = bytearray(desc)
    struct.pack_into("<I", bad, 128 + 13 * 4, WIN_HI)  # layer 0 output outside the window
    submit(0, nl, bytes(bad), reject=True)
    clear_outputs()
    submit(0, nl)
    entry["post_fault_accuracy"] = validate_all()
    entry["controls"] = {"guarded_fault": 1, "window_rejection": 1, "post_fault_valid": 1}
    med = lambda xs: float(np.median(xs))  # noqa: E731
    entry["summary"] = {
        s: {
            "wall_ms": med([r["wall_ms"] for r in entry["runs"] if r["schedule"] == s]),
            "wrapper_ms": med([r["wrapper_ms"] for r in entry["runs"] if r["schedule"] == s]),
            "body_us": med([r["body_us"] for r in entry["runs"] if r["schedule"] == s]),
        }
        for s in schedules
    }
    entry["summary"]["per_layer_us"] = [
        {
            "prologue": med(
                [r["jobs"][0]["prologue_us"][i] for r in entry["runs"] if r["schedule"] == "whole"]
            ),
            "blocks": med(
                [r["jobs"][0]["blocks_us"][i] for r in entry["runs"] if r["schedule"] == "whole"]
            ),
        }
        for i in range(nl)
    ]
    print(json.dumps({"chain": name, "passed": True, "summary": entry["summary"]}), flush=True)
    return entry


if __name__ == "__main__":
    sys.exit(main())
