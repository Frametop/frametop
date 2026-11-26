"""Device run of the fused HMX spatial->projection block prototype.

Measures, per 32-position output block, the phases the kernel partitions its
body into: weight-tile copies, source staging (spatial gathering), packing,
HMX clear/load/store, stage-1 post, mid spill/reload, stage-2 post/store and
setup. Uses public weights only for the teacher control; every other fixture
is deterministic synthetic data. No private recordings.

Protections are the existing ones: cached entry, worker 0 and one HMX owner,
saved 8 KiB VTCM, 128 KiB job arena, in-kernel 20 ms budget and the SDK 40 ms
timeout, audited per-job restoration of code/header/status/VTCM backup/arena,
callback and HMX power, resident-arena and DSP-service comparison before and
after. Guarded faults (dirty accumulator) and wrong-geometry rejections are
exercised. On any unexpected condition the run stops without retry,
recovery, gate clearing or service restart. Numerical checks run only after
the audited transaction has restored.
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
from framenpu import Device, Job, JobFailed
from framenpu.convolution import load_landmark_graph
from framenpu.resident_gemm import _lease, RECORD
from framenpu.resident_landmark import CONTROL, ARENA_START, ARENA_END
from hardware_ieee_probe import completion
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health
from hardware_landmark import digest, service
from hmx_multi_run import failure_snapshot
from hmx_block_layout import (
    BY_NAME,
    GEOMETRIES,
    MAGIC,
    META_BYTES,
    OPCODE,
    oracle,
    exact_reference,
    payload,
    synthetic_fixture,
    teacher_fixture,
    unpack,
    unplanar,
)

PHASES = ("weights", "stage", "pack", "hmx", "post", "spill", "store", "setup")
TICKS_PER_US = 19.2
BODY_TARGET_TICKS = 192000  # keep measured jobs <= 10 ms, half the 20 ms budget


def load_builds(folder, p):
    builds = {}
    for g in GEOMETRIES:
        for d in sorted((folder / g.name).iterdir()):
            r = json.loads((d / "build.json").read_text())
            j = Job.load(d / "probe.fjob")
            if hashlib.sha256(j.code).hexdigest() != r["sha256"] or r["geometry"] != g.name:
                p.error(f"build differs: {d}")
            for path, sha in r["sources"].items():
                if digest(ROOT / path) != sha:
                    p.error(f"source pin differs: {path}")
            builds[(g.name, r["variant"])] = (j, r)
    return builds


def check_values(g, raw, fixture):
    """Compare planar output (and spilled mid tiles) against the oracle."""
    import numpy as np

    ref = oracle(g, *fixture)
    body = raw[META_BYTES : META_BYTES + (g.cout // 2) * g.nj * 128]
    out = unplanar(body, g.cout, g.nj)
    err = np.abs(out[:, : g.nout] - ref["out"])
    if not np.isfinite(out).all() or np.any(err > ref["out_allow"]) or np.any(out[:, g.nout :]):
        raise ValueError(f"{g.name}: block output differs after clean restoration")
    result = {
        "values": int(out[:, : g.nout].size),
        "max_abs": float(err.max()),
        "max_error_to_bound_ratio": float(np.max(err / ref["out_allow"])),
        "exact_semantics_max_abs": float(
            np.max(np.abs(out[:, : g.nout] - exact_reference(g, *fixture)))
        ),
    }
    if g.nm > 1:  # spill holds the mid tiles of the last processed block
        j = g.nj - 1
        lo, hi = 32 * j, min(32 * j + 32, g.nout)
        base = META_BYTES + (g.cout // 2) * g.nj * 128
        mid_err = 0.0
        for m in range(g.nm):
            tile = unpack(raw[base + 2048 * m : base + 2048 * (m + 1)])
            rows = min(32, g.cmid - 32 * m)
            want = ref["mid"][32 * m : 32 * m + rows, lo:hi]
            allow = ref["mid_allow"][32 * m : 32 * m + rows, lo:hi]
            e = np.abs(tile[:rows, : hi - lo] - want)
            if np.any(e > allow) or np.any(tile[:, hi - lo :]) or np.any(tile[rows:]):
                raise ValueError(f"{g.name}: spilled mid tile {m} differs")
            mid_err = max(mid_err, float(e.max()))
        result["spilled_mid_max_abs"] = mid_err
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    for name in ("jobs", "model", "output"):
        p.add_argument("--" + name, required=True, type=Path)
    p.add_argument("--rounds", type=int, default=5)
    args = p.parse_args()
    if not args.run_on_device or os.geteuid() != 0 or not 1 <= args.rounds <= 10:
        p.error("Frame root, --run-on-device and rounds in 1..10 required")
    graph = load_landmark_graph(args.model / "hand.ncnn.param", args.model / "hand.ncnn.bin")
    builds = load_builds(args.jobs, p)
    teacher = BY_NAME["teacher_112"]
    fixtures = [(g, "synthetic", synthetic_fixture(g), 0) for g in GEOMETRIES if g is not teacher]
    fixtures += [
        (teacher, f"public_y{y0}", teacher_fixture(teacher, graph, y0), y0) for y0 in (0, 48)
    ]
    report = {
        "format": "framenpu-hmx-block-probe",
        "model_sha256": graph.weights_sha256,
        "param_sha256": graph.param_sha256,
        "builds": {f"{k[0]}/{k[1]}": r for k, (_, r) in builds.items()},
        "source_sha256": {
            n: digest(ROOT / n)
            for n in ["tests/hardware_hmx_block.py", "tests/hmx_block_layout.py"]
        },
        "conditions": "synthetic activations; public v20a weights only for teacher_112",
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
        "cases": [],
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
                    if bytes(observer._transport.memory[CONTROL : CONTROL + 8]) == RECORD:
                        raise ValueError("resident refusal record present")
                    before = bytes(observer._transport.memory[ARENA_START:ARENA_END])
                report["arena_before_sha256"] = hashlib.sha256(before).hexdigest()
                with audited_device(power, report["audits"])(dispatch="direct") as dev:

                    def run(g, variant, data, reps, *, fault=False, reject=False):
                        job, record = builds[(g.name, variant)]
                        seen = []

                        def validate(result):
                            completion(
                                result, job, fault=fault, reject=reject, output_bytes=g.output_bytes
                            )
                            seen.append(result)

                        gid = g.gid % len(GEOMETRIES) + 1 if reject else g.gid
                        try:
                            res = dev._submit(
                                job,
                                data,
                                opcode=OPCODE,
                                params=(1, gid, reps, 0, 0, int(fault)),
                                output_size=g.output_bytes,
                                use_direct=True,
                                _completion_validator=validate,
                            )
                        except JobFailed:
                            if (
                                not seen
                                or not (fault or reject)
                                or seen[0].workers[0][2] != (2 if fault else 1)
                            ):
                                raise
                            return seen[0], None
                        if fault or reject:
                            raise ValueError("expected fault/rejection missing")
                        m = struct.unpack_from("<96I", res.output)
                        body = (m[8] | m[9] << 32) - (m[6] | m[7] << 32)
                        mac = record["macros"]
                        if (
                            m[0] != MAGIC
                            or not 0x87A92000 <= m[1] < 0x87A92000 + len(job.code)
                            or m[2] != g.gid
                            or m[3] != reps
                            or m[4] != reps * g.nj
                            or m[10]
                            != mac["FB_BATCH"]
                            | mac["FB_TIMING"] << 8
                            | mac["FB_REUSE"] << 16
                            | mac.get("FB_NOHMX", 0) << 24
                            | mac.get("FR_PAIR", 0) << 28
                            | mac.get("FR_RESIDENT", 0) << 30
                            or any(m[11:32])
                            or (variant != "nohmx" and any(m[64:96]))  # raw nonfinite flag vector
                            or not 0 <= body <= res.kernel_reference_ticks
                        ):
                            raise ValueError(
                                "block metadata/finite/budget differs after restore: "
                                f"{variant} meta={[hex(w) for w in m[:12]]} "
                                f"nonzero11_32={[i for i in range(11, 32) if m[i]]} "
                                f"flags={sum(1 for w in m[64:96] if w)} body={body} "
                                f"ref={res.kernel_reference_ticks}"
                            )
                        return res, {
                            "body_ticks": body,
                            "phase_ticks": dict(zip(PHASES, m[32:40])),
                            "wrapper_ms": res.kernel_reference_ticks / 19200,
                        }

                    for g, label, fixture, y0 in fixtures:
                        start = time.monotonic_ns()
                        data = payload(g, *fixture, y0=y0)
                        entry = {
                            "geometry": g.name,
                            "fixture": label,
                            "role": g.role,
                            "payload_sha256": hashlib.sha256(data).hexdigest(),
                            "host_payload_ms": (time.monotonic_ns() - start) / 1e6,
                            "per_block": {
                                "k_tiles": g.k_tiles_per_block,
                                "transactions": g.transactions_per_block,
                                "blocks_per_job_rep": g.nj,
                            },
                            "samples": [],
                        }
                        report["cases"].append(entry)
                        variants = sorted(v for (n, v) in builds if n == g.name)
                        warm = {}
                        for v in variants:  # checked warmups, excluded from timing
                            res, meta = run(g, v, data, 1)
                            if v != "nohmx":  # timing-only control has no valid output
                                check_values(g, res.output, fixture)
                            warm[v] = meta["body_ticks"]
                        hi = max(1, min(4, BODY_TARGET_TICKS // max(warm.values())))
                        entry["repeat_counts"] = [1, hi]
                        for round_ in range(args.rounds):
                            order = variants if round_ % 2 == 0 else variants[::-1]
                            for v in order:
                                for reps in (1, hi) if round_ % 2 else (hi, 1):
                                    res, meta = run(g, v, data, reps)
                                    accuracy = (
                                        None
                                        if v == "nohmx"
                                        else check_values(g, res.output, fixture)
                                    )
                                    entry["samples"].append(
                                        {
                                            "variant": v,
                                            "reps": reps,
                                            "round": round_,
                                            **meta,
                                            "error": accuracy,
                                        }
                                    )
                        print(json.dumps({"case": f"{g.name}/{label}", "passed": True}), flush=True)
                        args.output.write_text(json.dumps(report, indent=2) + "\n")
                    controls = 0
                    for g, label, fixture, y0 in fixtures:
                        if label == "public_y48":
                            continue
                        data = payload(g, *fixture, y0=y0)
                        for v in ("main", "res2"):  # v1 and VTCM-resident v2
                            if (g.name, v) not in builds:
                                continue
                            run(g, v, data, 1, fault=True)  # trap after first load
                            run(g, v, data, 1, reject=True)
                            res, _ = run(g, v, data, 1)  # clean result after the fault
                            check_values(g, res.output, fixture)
                            controls += 1
                    report["guarded_faults"] = controls
                    report["wrong_geometry_rejections"] = controls
                    report["post_fault_valid_results"] = controls
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
