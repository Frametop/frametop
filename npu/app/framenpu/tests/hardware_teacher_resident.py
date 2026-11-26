"""Device run of the resident teacher tail (conv_12 .. convdwclip_15) on the
qualified corpus crops, with host FP32 pooling and heads.

Phase A (qualification, plan mode "qualify"): every compute op runs as its own
job with the resident kernel (opcode 111) or, for the three stride-2 layers,
tests/teacher_dw_probe.c (opcode 110). Every output is read back and checked
against an FP16 error bound computed from the device's actual input, and
compared with the crop's ncnn blobs.

Phase B (plan mode "resident"): 4 resident jobs + 3 stride-2 jobs per frame,
tensors kept in VTCM inside each resident job, all parameters written to the
window once. Round 0 reads every tensor the plan stores and requires it to be
bit-identical to phase A; later rounds read only the final tensor (also
required bit-identical) and time the frame.

Controls on the first crop: a guarded fault inside a resident job, an
out-of-window descriptor rejected natively, and clean frames afterwards.
Protections as tests/hardware_teacher_tail.py (lease, refusal record, window
save/restore with whole-arena hash, audited direct dispatch, no retry,
recovery or restart). Only statistics are written.
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
sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tests"), str(ROOT / "tools")]
from framenpu import Device, Job, JobFailed
from framenpu.resident_gemm import _lease, RECORD
from framenpu.resident_landmark import CONTROL, ARENA_START, ARENA_END
from hardware_ieee_probe import completion
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health
from hardware_landmark import digest, service
from hmx_multi_run import failure_snapshot
import teacher_tail_layout as T
import teacher_resident_layout as RL

TICKS_PER_US = 19.2
CODE_BASE = 0x87A92000


def load_jobs(root):
    jobs = {}
    for k in ("rs", "dw"):
        job = Job.load(root / k / "probe.fjob")
        build = json.loads((root / k / "build.json").read_text())
        if hashlib.sha256(job.code).hexdigest() != build["sha256"]:
            raise ValueError(f"{k}: build/image hash differs")
        for name, sha in build["sources"].items():
            if digest(ROOT / name) != sha:
                raise ValueError(f"{k}: source pin differs: {name}")
        jobs[k] = (job, build)
    return jobs


class Runner:
    def __init__(self, dev, mem, jobs, graph, specs):
        self.dev, self.mem, self.jobs, self.graph, self.specs = dev, mem, jobs, graph, specs
        self.by = {s["name"]: s for s in specs}
        self.sh = T.shapes(specs)
        self.plans = {m: RL.build_plan(graph, specs, m) for m in ("qualify", "resident")}

    # -- window I/O
    def put(self, off, data):
        self.mem[off : off + len(data)] = data

    def raw(self, plan, blob):
        g = T.grid_of(self.sh[blob])
        off = plan.where[blob]
        return bytes(self.mem[off : off + g.tensor_bytes(self.sh[blob][0])])

    def tensor(self, plan, blob):
        out, stray = T.grid_of(self.sh[blob]).from_bytes(self.raw(plan, blob), self.sh[blob][0])
        if stray:
            raise ValueError(f"{blob}: {stray} nonzero padding halves")
        return out

    def install(self, plan):
        for off, data in RL.setup_writes(plan).items():
            self.put(off, data)

    def inputs(self, plan, blob):
        import numpy as np

        for b in RL.INPUTS:
            self.put(
                plan.where[b],
                T.grid_of(self.sh[b]).to_bytes(np.asarray(blob(b)).astype(np.float16)),
            )

    # -- one job
    def submit(self, plan, job, *, fault=False, reject=False):
        for off, data in job.staged.items():
            self.put(off, data)
        kind = job.kind
        image, _ = self.jobs["rs" if kind == "rs" else "dw"]
        if kind == "rs":
            data, opcode, params = (
                RL.payload(plan),
                RL.RS_OPCODE,
                (1, job.first, job.end, 0, 0, int(fault)),
            )
        else:
            data, opcode, params = job.desc, T.DW_OPCODE, (1, 0, 0, 0, 0, int(fault))
        seen = []

        def validate(result):
            completion(result, image, fault=fault, reject=reject, output_bytes=T.META_BYTES)
            seen.append(result)

        t0 = time.perf_counter()
        try:
            res = self.dev._submit(
                image,
                data,
                opcode=opcode,
                params=params,
                output_size=T.META_BYTES,
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
        ref = res.kernel_reference_ticks
        if not CODE_BASE <= m[1] < CODE_BASE + len(image.code) or any(m[64:96]):
            raise ValueError(f"{job.ops}: pc/finite flags differ: {[hex(w) for w in m[:8]]}")
        if kind == "rs":
            n = job.end - job.first
            ticks = list(m[96 : 96 + n])
            if (
                m[0] != RL.RS_MAGIC
                or m[2] != n
                or any(m[3:64])
                or any(m[96 + n :])
                or any(b < a for a, b in zip(ticks, ticks[1:]))
                or not 0 < ticks[-1] <= ref
            ):
                raise ValueError(
                    f"{job.ops}: resident metadata/budget differs: {[hex(w) for w in m[:8]]}"
                )
            per = [b - a for a, b in zip([0] + ticks, ticks)]
            return {
                "kind": kind,
                "wall_ms": wall * 1e3,
                "wrapper_ms": ref / 19200,
                "body_ms": ticks[-1] / 19200,
                "op_us": [p / TICKS_PER_US for p in per],
            }
        gin = T.grid_of(self.sh[job.op.src])
        if (
            m[0] != T.DW_MAGIC
            or m[2] != gin.nj4
            or any(m[7:32])
            or not 0 < m[4] + m[5] + m[6] <= ref
        ):
            raise ValueError(
                f"{job.ops}: stride-2 metadata/budget differs: {[hex(w) for w in m[:8]]}"
            )
        return {
            "kind": kind,
            "wall_ms": wall * 1e3,
            "wrapper_ms": ref / 19200,
            "body_ms": (m[4] + m[5] + m[6]) / 19200,
            "op_us": [(m[4] + m[5] + m[6]) / TICKS_PER_US],
        }

    # -- phase A
    def qualify(self, blob):
        import numpy as np
        from landmark_reference import evaluate

        plan = self.plans["qualify"]
        self.install(plan)
        self.inputs(plan, blob)
        t = {b: self.tensor(plan, b) for b in RL.INPUTS}
        ratios, raw = {}, {}
        ops = {o.name: o for o in plan.ops}
        for job in plan.jobs:
            self.submit(plan, job)
            o = ops[job.ops[0]]
            out = self.tensor(plan, o.dst)
            raw[o.dst] = self.raw(plan, o.dst)
            x = t[o.src]
            if o.kind == "pw":
                exact = evaluate(self.graph, o.spec, [x], hmx=False).astype(np.float64)
                if o.res:
                    exact = exact + t[o.res]
                allow = T.bound_pw(self.graph, o.spec, x, t[o.res] if o.res else None)
            else:
                exact = evaluate(self.graph, o.spec, [x]).astype(np.float64)
                allow = T.bound_dw(self.graph, o.spec, x)
            if not np.isfinite(out).all():
                raise ValueError(f"{o.name}: non-finite output")
            ratios[o.name] = float(np.max(np.abs(out - exact) / allow))
            if ratios[o.name] > 1:
                raise ValueError(f"{o.name}: outside the FP16 bound (ratio {ratios[o.name]:.3g})")
            t[o.dst] = out
        return t, ratios, raw

    # -- phase B
    def frame(self, blob, *, observe):
        plan = self.plans["resident"]
        t0 = time.perf_counter()
        self.inputs(plan, blob)
        rows, seen = [], {}
        for job in plan.jobs:
            rows.append(self.submit(plan, job))
            if observe:
                for b in job.stores:
                    seen[b] = self.raw(plan, b)
        final = self.raw(plan, RL.FINAL)
        wall = (time.perf_counter() - t0) * 1e3
        return rows, seen, final, wall


def heads_row(graph, specs, t79, blob):
    h = T.heads(graph, specs, t79)
    ref = {k: blob(k) for k in ("out0", "out1", "out2", "out3")}
    return {
        "landmarks": T.landmark_error(h["out0"], ref["out0"]),
        "world_max_abs": T.compare(h["out3"], ref["out3"])["max_abs"],
        "presence": [float(h["out2"][0]), float(ref["out2"][0])],
        "handedness": [float(h["out1"][0]), float(ref["out1"][0])],
    }


def run(runner, corpus, manifest, rounds):
    import numpy as np

    graph, specs, sh = runner.graph, runner.specs, runner.sh
    shape_of = sh | {"in0": (3, 224, 224)}
    crops, ref_raw = {}, {}

    def loader(crop):
        def blob(name):
            raw = (corpus / crop["name"] / f"blob_{name}.f32").read_bytes()
            if hashlib.sha256(raw).hexdigest() != crop["files"][name]["sha256"]:
                raise ValueError(f"corpus blob hash differs: {crop['name']}/{name}")
            return np.frombuffer(raw, "<f4").reshape(shape_of[name]).astype(np.float64)

        return blob

    # phase A
    for crop in manifest["crops"]:
        blob = loader(crop)
        t, ratios, raw = runner.qualify(blob)
        ref_raw[crop["name"]] = raw
        crops[crop["name"]] = {
            "split": crop["split"],
            "qualify": {
                "max_bound_ratio": max(ratios.values()),
                "bound_ratio_per_layer": ratios,
                "drift_per_tensor": {
                    o.dst: T.compare(t[o.dst], blob(o.dst)) | {"op": o.name}
                    for o in runner.plans["qualify"].ops
                },
            }
            | heads_row(graph, specs, t[RL.FINAL], blob),
        }
        print(
            json.dumps(
                {
                    "phase": "qualify",
                    "crop": crop["name"],
                    "max_bound_ratio": max(ratios.values()),
                    "landmarks": crops[crop["name"]]["qualify"]["landmarks"],
                }
            ),
            flush=True,
        )

    # phase B
    plan = runner.plans["resident"]
    runner.install(plan)
    for i, crop in enumerate(manifest["crops"]):
        blob = loader(crop)
        name = crop["name"]
        frames = []
        for r in range(rounds):
            rows, seen, final, wall = runner.frame(blob, observe=r == 0)
            if final != ref_raw[name][RL.FINAL]:
                raise ValueError(f"{name} round {r}: final tensor differs from qualification")
            for b, data in seen.items():
                if b in ref_raw[name] and data != ref_raw[name][b]:
                    raise ValueError(f"{name}: stored tensor {b} differs from qualification")
            frames.append({"jobs": rows, "host_wall_ms": wall})
            print(
                json.dumps(
                    {
                        "phase": "resident",
                        "crop": name,
                        "round": r,
                        "wrapper_ms": sum(j["wrapper_ms"] for j in rows),
                        "body_ms": sum(j["body_ms"] for j in rows),
                        "host_wall_ms": wall,
                    }
                ),
                flush=True,
            )
        entry = {"frames": frames, "bit_identical_tensors": sorted(seen)}
        if i == 0:
            rs = next(j for j in plan.jobs if j.kind == "rs")
            runner.submit(plan, rs, fault=True)
            table = bytearray(plan.table)
            k = next(
                n
                for n in range(rs.first, rs.end)
                if struct.unpack_from("<I", table, 128 * n)[0] == 1
            )
            struct.pack_into("<I", table, 128 * k + 36, T.WIN_HI)  # dpar outside the window
            runner.put(plan.table_at, bytes(table))
            runner.submit(plan, rs, reject=True)
            runner.install(plan)
            entry["controls"] = {"guarded_fault_resident": 1, "descriptor_rejection_resident": 1}
            rows, _, final, wall = runner.frame(blob, observe=False)
            if final != ref_raw[name][RL.FINAL]:
                raise ValueError("clean frame after controls differs")
            entry["clean_after_controls"] = True
        crops[name]["resident"] = entry
    return crops


def summarize(crops, plan):
    import numpy as np

    frames = [f for c in crops.values() for f in c["resident"]["frames"]]
    med = lambda xs: float(np.median(xs))  # noqa: E731
    kinds = [j.kind for j in plan.jobs]
    out = {
        "crops": len(crops),
        "jobs_per_frame": len(kinds),
        "max_layer_bound_ratio": max(c["qualify"]["max_bound_ratio"] for c in crops.values()),
        "max_landmark_px": max(c["qualify"]["landmarks"]["max_px"] for c in crops.values()),
        "max_presence_delta": max(
            abs(c["qualify"]["presence"][0] - c["qualify"]["presence"][1]) for c in crops.values()
        ),
        "resident_bit_identical_to_qualification": True,
        "median_ms_per_frame": {
            "wrapper_total": med([sum(j["wrapper_ms"] for j in f["jobs"]) for f in frames]),
            "body_total": med([sum(j["body_ms"] for j in f["jobs"]) for f in frames]),
            "body_resident_jobs": med(
                [sum(j["body_ms"] for j in f["jobs"] if j["kind"] == "rs") for f in frames]
            ),
            "body_stride2_jobs": med(
                [sum(j["body_ms"] for j in f["jobs"] if j["kind"] == "s2") for f in frames]
            ),
            "host_wall": med([f["host_wall_ms"] for f in frames]),
        },
        "median_ms_per_job": [
            {
                "kind": k,
                "ops": plan.jobs[n].ops[:1] + plan.jobs[n].ops[-1:],
                "wrapper": med([f["jobs"][n]["wrapper_ms"] for f in frames]),
                "body": med([f["jobs"][n]["body_ms"] for f in frames]),
            }
            for n, k in enumerate(kinds)
        ],
    }
    return out


def main():
    from framenpu.convolution import load_landmark_graph
    from framenpu.landmark import specifications

    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    for name in ("jobs", "model", "corpus", "output"):
        p.add_argument("--" + name, required=True, type=Path)
    p.add_argument("--rounds", type=int, default=5)
    args = p.parse_args()
    if not args.run_on_device or os.geteuid() != 0 or not 2 <= args.rounds <= 20:
        p.error("Frame root, --run-on-device and rounds in 2..20 required")
    jobs = load_jobs(args.jobs)
    graph = load_landmark_graph(args.model / "hand.ncnn.param", args.model / "hand.ncnn.bin")
    specs = specifications(graph)
    manifest = json.loads((args.corpus / "manifest.json").read_text())
    if (
        manifest["param_sha256"] != graph.param_sha256
        or manifest["weights_sha256"] != graph.weights_sha256
    ):
        p.error("corpus/model mismatch")
    report = {
        "format": "framenpu-teacher-resident-run",
        "builds": {k: v[1] for k, v in jobs.items()},
        "source_sha256": {
            n: digest(ROOT / n)
            for n in (
                "tests/hardware_teacher_resident.py",
                "tests/teacher_resident_layout.py",
                "tests/teacher_tail_layout.py",
            )
        },
        "manifest_sha256": digest(args.corpus / "manifest.json"),
        "scope": "conv_12 .. convdwclip_15 on the DSP from ncnn blobs 8/10; FP32 pooling and heads on the host",
        "window": [f"{T.WIN_LO:#x}", f"{T.WIN_HI:#x}"],
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
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
                saved = before[T.WIN_LO - ARENA_START : T.WIN_HI - ARENA_START]
                try:
                    with audited_device(power, report["audits"])(dispatch="direct") as dev:
                        runner = Runner(dev, dev._transport.memory, jobs, graph, specs)
                        report["plans"] = {
                            m: {
                                "jobs": [
                                    {
                                        "kind": j.kind,
                                        "ops": len(j.ops),
                                        "descriptors": j.end - j.first,
                                        "vtcm_peak_vectors": j.vtcm_peak,
                                    }
                                    for j in pl.jobs
                                ],
                                "window_end": f"{pl.end:#x}",
                            }
                            for m, pl in runner.plans.items()
                        }
                        report["crops"] = run(runner, args.corpus, manifest, args.rounds)
                        report["summary"] = summarize(report["crops"], runner.plans["resident"])
                finally:
                    with Device() as observer:
                        mem = observer._transport.memory
                        mem[T.WIN_LO : T.WIN_HI] = saved
                        after = bytes(mem[ARENA_START:ARENA_END])
                    report["arena_after_sha256"] = hashlib.sha256(
                        after
                    ).hexdigest()  # also on failure
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
                "summary": report.get("summary"),
            },
            indent=1,
        ),
        flush=True,
    )
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
