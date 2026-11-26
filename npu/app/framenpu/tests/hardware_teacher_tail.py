"""Device run of the FP16 teacher tail (conv_12 .. convdwclip_15, 42 jobs per
crop) on the qualified corpus crops, with host FP32 pooling and heads.

Each crop starts from its ncnn FP32 blobs 8 and 10 (rounded to FP16) and
runs layer by layer on the DSP: tests/teacher_pw_probe.c for 1x1 layers
(HMX, fused bias/clip/residual) and tests/teacher_dw_probe.c for depthwise
layers (HVX QFloat16, stride 1/2). Activations live in grid buffers in the
arena window; each layer's weights are staged just before its job. Checks
per layer: metadata, finite flags, zero padding lanes and |device - exact|
within an FP16 bound computed from the device's actual input. Reported:
drift of every tensor and the four heads from the crop's ncnn blobs, and
per-job prologue/compute/epilogue time (for the VTCM-resident forecast).

Protections as tests/hardware_hmx_chain.py: arena lease, refusal record,
window save/restore with whole-arena hash, audited direct dispatch, guarded
fault, out-of-window rejection and a clean rerun. Stops on anything
unexpected without retry, recovery or service restart. Only statistics are
written; no tensors leave the device.
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

TICKS_PER_US = 19.2


def load_jobs(root):
    jobs = {}
    for k in ("pw", "dw"):
        job = Job.load(root / k / "probe.fjob")
        build = json.loads((root / k / "build.json").read_text())
        if hashlib.sha256(job.code).hexdigest() != build["sha256"]:
            raise ValueError(f"{k}: build/image hash differs")
        for name, sha in build["sources"].items():
            if digest(ROOT / name) != sha:
                raise ValueError(f"{k}: source pin differs: {name}")
        jobs[k] = (job, build)
    return jobs


def main():
    from framenpu.convolution import load_landmark_graph
    from framenpu.landmark import specifications

    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    for name in ("jobs", "model", "corpus", "output"):
        p.add_argument("--" + name, required=True, type=Path)
    p.add_argument("--rounds", type=int, default=3)
    args = p.parse_args()
    if not args.run_on_device or os.geteuid() != 0 or not 1 <= args.rounds <= 10:
        p.error("Frame root, --run-on-device and rounds in 1..10 required")
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
        "format": "framenpu-teacher-tail-run",
        "builds": {k: v[1] for k, v in jobs.items()},
        "source_sha256": {
            n: digest(ROOT / n)
            for n in ("tests/hardware_teacher_tail.py", "tests/teacher_tail_layout.py")
        },
        "manifest_sha256": digest(args.corpus / "manifest.json"),
        "scope": "conv_12 .. convdwclip_15 on the DSP from ncnn blobs 8/10; FP32 pooling and heads on the host",
        "window": [f"{T.WIN_LO:#x}", f"{T.WIN_HI:#x}"],
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
        "crops": {},
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
                        for i, crop in enumerate(manifest["crops"]):
                            report["crops"][crop["name"]] = runner.crop(
                                args.corpus, crop, args.rounds, controls=i == 0
                            )
                            args.output.write_text(json.dumps(report, indent=1) + "\n")
                finally:
                    with Device() as observer:
                        mem = observer._transport.memory
                        mem[T.WIN_LO : T.WIN_HI] = saved
                        after = bytes(mem[ARENA_START:ARENA_END])
                report["arena_after_sha256"] = hashlib.sha256(after).hexdigest()
                if after != before:
                    raise ValueError("resident arena differs after window restoration")
                report["summary"] = summarize(report["crops"])
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


class Runner:
    def __init__(self, dev, mem, jobs, graph, specs):
        self.dev, self.mem, self.jobs, self.graph, self.specs = dev, mem, jobs, graph, specs
        self.by = {s["name"]: s for s in specs}
        self.ops, _ = T.tail_ops(specs)
        self.sh = T.shapes(specs)
        self.where, self.stage = T.allocate(self.ops, self.sh, ["8", "10"])
        self.prepared = [self.prepare(op) for op in self.ops]

    def prepare(self, op):
        s = self.by[op.name]
        gin, gout = T.grid_of(self.sh[op.src]), T.grid_of(self.sh[op.dst])
        if op.kind == "pw":
            tiles, bias, nk, no = T.pw_weights(self.graph, s)
            staged = tiles + bias + gin.mask_bytes()[: gin.nj * 128]
            desc = T.pw_descriptor(
                s,
                gin,
                nk,
                no,
                self.where[op.src][0],
                self.stage,
                [len(tiles), len(bias)],
                self.where[op.res][0] if op.res else 0,
                self.where[op.dst][0],
            )
        else:
            coef, bias = T.dw_weights(self.graph, s)
            staged = coef + bias + gin.mask_bytes()
            desc = T.dw_descriptor(
                s,
                gin,
                gout,
                self.where[op.src][0],
                self.stage,
                [len(coef), len(bias)],
                self.where[op.dst][0],
            )
        if self.stage + len(staged) > T.WIN_HI:
            raise ValueError(f"{op.name}: staging exceeds the window")
        return {
            "op": op,
            "spec": s,
            "staged": staged,
            "desc": desc,
            "gin": gin,
            "gout": gout,
            "nblocks": gin.nj if op.kind == "pw" else gin.nj4,
        }

    def read(self, blob):
        off, _ = self.where[blob]
        g = T.grid_of(self.sh[blob])
        raw = bytes(self.mem[off : off + g.tensor_bytes(self.sh[blob][0])])
        return g.from_bytes(raw, self.sh[blob][0])

    def write(self, blob, tensor):
        import numpy as np

        off, _ = self.where[blob]
        data = T.grid_of(self.sh[blob]).to_bytes(np.asarray(tensor).astype(np.float16))
        self.mem[off : off + len(data)] = data

    def submit(self, prep, *, desc=None, fault=False, reject=False):
        op = prep["op"]
        job, _ = self.jobs[op.kind]
        off, size = self.where[op.dst]
        self.mem[off : off + size] = bytes(size)
        self.mem[self.stage : self.stage + len(prep["staged"])] = prep["staged"]
        seen = []

        def validate(result):
            completion(result, job, fault=fault, reject=reject, output_bytes=T.META_BYTES)
            seen.append(result)

        t0 = time.perf_counter()
        try:
            res = self.dev._submit(
                job,
                desc or prep["desc"],
                opcode=T.PW_OPCODE if op.kind == "pw" else T.DW_OPCODE,
                params=(1, 0, 0, 0, 0, int(fault)),
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
        magic = T.PW_MAGIC if op.kind == "pw" else T.DW_MAGIC
        if (
            m[0] != magic
            or not 0x87A92000 <= m[1] < 0x87A92000 + len(job.code)
            or m[2] != prep["nblocks"]
            or any(m[64:96])
            or any(m[8:32])
            or (op.kind == "dw" and m[7])
            or not 0 < m[4] + m[5] + m[6] <= res.kernel_reference_ticks
        ):
            raise ValueError(
                f"{op.name}: metadata/finite/budget differs: {[hex(w) for w in m[:9]]}"
            )
        return {
            "wall_ms": wall * 1000,
            "wrapper_ms": res.kernel_reference_ticks / 19200,
            "prologue_us": m[4] / TICKS_PER_US,
            "compute_us": m[5] / TICKS_PER_US,
            "epilogue_us": m[6] / TICKS_PER_US,
            "cycles": m[7],
        }

    def check_layer(self, prep, t):
        """Device output vs exact FP64/FP32 of its actual device input."""
        import numpy as np
        from landmark_reference import evaluate

        op, s = prep["op"], prep["spec"]
        out, stray = self.read(op.dst)
        if stray or not np.isfinite(out).all():
            raise ValueError(f"{op.name}: stray padding {stray} or non-finite output")
        x = t[op.src]
        if op.kind == "pw":
            exact = evaluate(self.graph, s, [x], hmx=False).astype(np.float64)
            if op.res:
                exact = exact + t[op.res]
            allow = T.bound_pw(self.graph, s, x, t[op.res] if op.res else None)
        else:
            exact = evaluate(self.graph, s, [x]).astype(np.float64)
            allow = T.bound_dw(self.graph, s, x)
        ratio = float(np.max(np.abs(out - exact) / allow))
        if ratio > 1:
            raise ValueError(f"{op.name}: device output outside the FP16 bound (ratio {ratio:.3g})")
        return out, ratio

    def crop(self, corpus, crop, rounds, *, controls):
        import numpy as np

        shape_of = self.sh | {"in0": (3, 224, 224)}

        def blob(name):
            raw = (corpus / crop["name"] / f"blob_{name}.f32").read_bytes()
            if hashlib.sha256(raw).hexdigest() != crop["files"][name]["sha256"]:
                raise ValueError(f"corpus blob hash differs: {crop['name']}/{name}")
            return np.frombuffer(raw, "<f4").reshape(shape_of[name]).astype(np.float64)

        entry = {"split": crop["split"], "rounds": []}
        for round_ in range(rounds):
            t = {}
            for b in ("8", "10"):
                self.write(b, blob(b))
                t[b], stray = self.read(b)
                if stray:
                    raise ValueError("input padding not zero")
            timings, ratios = [], {}
            for prep in self.prepared:
                timings.append(self.submit(prep) | {"op": prep["op"].name, "kind": prep["op"].kind})
                t[prep["op"].dst], ratios[prep["op"].name] = self.check_layer(prep, t)
            h = T.heads(self.graph, self.specs, t["79"])
            ref = {k: blob(k) for k in ("out0", "out1", "out2", "out3")}
            row = {
                "round": round_,
                "jobs": timings,
                "max_bound_ratio": max(ratios.values()),
                "landmarks": T.landmark_error(h["out0"], ref["out0"]),
                "world_max_abs": T.compare(h["out3"], ref["out3"])["max_abs"],
                "presence": [float(h["out2"][0]), float(ref["out2"][0])],
                "handedness": [float(h["out1"][0]), float(ref["out1"][0])],
            }
            if round_ == 0:
                row["bound_ratio_per_layer"] = ratios
                row["drift_per_tensor"] = {
                    p["op"].dst: T.compare(t[p["op"].dst], blob(p["op"].dst)) | {"op": p["op"].name}
                    for p in self.prepared
                }
            entry["rounds"].append(row)
            print(
                json.dumps(
                    {
                        "crop": crop["name"],
                        "round": round_,
                        "landmarks": row["landmarks"],
                        "presence": row["presence"],
                        "max_bound_ratio": row["max_bound_ratio"],
                        "compute_ms": sum(j["compute_us"] for j in timings) / 1000,
                        "wrapper_ms": sum(j["wrapper_ms"] for j in timings),
                    }
                ),
                flush=True,
            )
        if controls:
            pw = next(p for p in self.prepared if p["op"].kind == "pw")
            dw = next(p for p in self.prepared if p["op"].kind == "dw")
            self.submit(pw, fault=True)
            for prep, word in ((pw, 14), (dw, 13)):
                bad = bytearray(prep["desc"])
                struct.pack_into("<I", bad, 4 * word, T.WIN_HI)  # output outside the window
                self.submit(prep, desc=bytes(bad), reject=True)
            entry["controls"] = {
                "guarded_fault_pw": 1,
                "window_rejection_pw": 1,
                "window_rejection_dw": 1,
            }
            # the next crop's first round re-runs every op cleanly after these controls
        return entry


def summarize(crops):
    import numpy as np

    rows = [r for c in crops.values() for r in c["rounds"]]

    def per(key, kind=None):
        return float(
            np.median([sum(j[key] for j in r["jobs"] if kind in (None, j["kind"])) for r in rows])
        )

    return {
        "crops": len(crops),
        "jobs_per_crop": len(rows[0]["jobs"]),
        "max_landmark_px": max(r["landmarks"]["max_px"] for r in rows),
        "max_world_abs": max(r["world_max_abs"] for r in rows),
        "max_presence_delta": max(abs(r["presence"][0] - r["presence"][1]) for r in rows),
        "max_handedness_delta": max(abs(r["handedness"][0] - r["handedness"][1]) for r in rows),
        "max_layer_bound_ratio": max(r["max_bound_ratio"] for r in rows),
        "median_ms_per_crop": {
            "compute_pw": per("compute_us", "pw") / 1000,
            "compute_dw": per("compute_us", "dw") / 1000,
            "prologue": per("prologue_us") / 1000,
            "epilogue": per("epilogue_us") / 1000,
            "wrapper": per("wrapper_ms"),
            "host_wall": per("wall_ms"),
        },
    }


if __name__ == "__main__":
    sys.exit(main())
