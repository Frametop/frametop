"""GAP/head qualification and paired timing, driven by hardware_teacher_link
--stride2 --heads. Uses its sealed image, per-job audits and refusal rules.
Only statistics are reported; private activations remain in device memory.
"""

import json
import struct
import time

import hardware_teacher_link as L
import hardware_teacher_resident as HR
import teacher_heads_layout as H
import teacher_resident_layout as R
import teacher_tail_layout as T


class HeadsRunner(L.LinkRunner):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.plans |= {
            m: H.build_plan(self.graph, self.specs, observe=m == "heads-observe")
            for m in ("heads", "heads-observe")
        }

    def read_values(self, plan, name, n):
        at = plan.where[name]
        return bytes(self.mem[at : at + n])

    def frame_heads(self, blob, *, observe=False):
        plan = self.plans["heads-observe" if observe else "heads"]
        self.install(plan)  # switching benchmark plans is setup, outside timing
        t0 = time.perf_counter()
        self.inputs(plan, blob)
        rows = [self.submit(plan, j, linked=True) for j in plan.jobs]
        raw = self.read_values(plan, "heads", 512)
        wall = (time.perf_counter() - t0) * 1000
        seen = {b: self.raw(plan, b) for j in plan.jobs for b in j.stores} if observe else {}
        pool = self.read_values(plan, "pooled", 2688) if observe else None
        return rows, seen, pool, raw, wall

    def frame_cpu_heads(self, blob):
        t0 = time.perf_counter()
        rows, _, raw, tail_wall = self.frame(blob, observe=False, linked=True)
        last, padding = T.grid_of(self.sh[R.FINAL]).from_bytes(raw, 672)
        if padding:
            raise ValueError("baseline final padding differs")
        heads = T.heads(self.graph, self.specs, last)
        return rows, raw, heads, (time.perf_counter() - t0) * 1000, tail_wall


def accuracy(values, host_heads, blob):
    import numpy as np

    heads = H.split(np.frombuffer(values, "<f4")) if isinstance(values, bytes) else values
    incremental, ncnn = {}, {}
    for name in H.ORDER:
        x = np.asarray(heads[name], np.float64)
        if not np.isfinite(x).all():
            raise ValueError("nonfinite head output")
        incremental[name] = float(np.max(np.abs(x - host_heads[name])))
        if incremental[name] > H.CAPS[name]:
            raise ValueError(f"{name}: incremental head error exceeds frozen cap")
        ncnn[name] = T.compare(x, blob(name))
    landmarks = T.landmark_error(heads["out0"], blob("out0"))
    if (
        landmarks["max_px"] > 1
        or ncnn["out0"]["max_abs"] > 1
        or ncnn["out0"]["rms"] > 0.25
        or ncnn["out3"]["max_abs"] > 0.003
        or ncnn["out2"]["max_abs"] > 0.005
        or ncnn["out1"]["max_abs"] > 0.005
    ):
        raise ValueError("head output exceeds frozen ncnn accuracy ceiling")
    return {"incremental_max_abs": incremental, "ncnn": ncnn, "landmarks": landmarks}


def run(runner, corpus, manifest, rounds):
    import numpy as np

    shapes = runner.sh | {"in0": (3, 224, 224)}
    crops, references = {}, {}

    def loader(crop):
        def blob(name):
            path = corpus / crop["name"] / f"blob_{name}.f32"
            if L.digest(path) != crop["files"][name]["sha256"]:
                raise ValueError("corpus hash differs")
            return np.frombuffer(path.read_bytes(), "<f4").reshape(shapes[name]).astype(np.float64)

        return blob

    for crop in manifest["crops"]:
        blob = loader(crop)
        t, ratios, raw = runner.qualify(blob)
        host = T.heads(runner.graph, runner.specs, t[R.FINAL])
        references[crop["name"]] = (raw, host, H.pool_reference(t[R.FINAL]))
        crops[crop["name"]] = {
            "split": crop["split"],
            "qualify": {"max_bound_ratio": max(ratios.values())}
            | HR.heads_row(runner.graph, runner.specs, t[R.FINAL], blob),
        }
        print(
            json.dumps(
                {"phase": "qualify", "crop": crop["name"], "max_bound_ratio": max(ratios.values())}
            ),
            flush=True,
        )
    seal_ms = runner.seal()
    for index, crop in enumerate(manifest["crops"]):
        name, blob = crop["name"], loader(crop)
        ref_raw, host, pool_ref = references[name]
        _, seen, pool, expected, _ = runner.frame_heads(blob, observe=True)
        for b, data in seen.items():
            if data != ref_raw[b]:
                raise ValueError(f"checkpoint {b} differs from qualification")
        pool_error = float(np.max(np.abs(np.frombuffer(pool, "<f4") - pool_ref)))
        if pool_error > H.CAPS["pool"]:
            raise ValueError("GAP exceeds frozen incremental cap")
        checked = accuracy(expected, host, blob)
        frames = {"cpu_heads": [], "dsp_heads": []}
        for round_ in range(rounds):
            # Alternate order too: CPU and DSP modes each use one linked request.
            for mode in (
                ("cpu_heads", "dsp_heads") if round_ % 2 == 0 else ("dsp_heads", "cpu_heads")
            ):
                if mode == "cpu_heads":
                    rows, raw, output, wall, tail_wall = runner.frame_cpu_heads(blob)
                    if raw != ref_raw[R.FINAL]:
                        raise ValueError("baseline tail differs from qualification")
                    accuracy(output, host, blob)
                else:
                    rows, _, _, output, wall = runner.frame_heads(blob)
                    if output != expected:
                        raise ValueError("DSP heads not bit-identical to observed frame")
                    accuracy(output, host, blob)
                    tail_wall = None
                frames[mode].append(
                    {"jobs": rows, "host_wall_ms": wall, "tail_only_wall_ms": tail_wall}
                )
                print(
                    json.dumps(
                        {
                            "phase": mode,
                            "crop": name,
                            "round": round_,
                            "wrapper_ms": sum(j["wrapper_ms"] for j in rows),
                            "host_wall_ms": wall,
                        }
                    ),
                    flush=True,
                )
        controls = {}
        if index == 0:
            plan = runner.plans["heads"]
            job = plan.jobs[0]
            base = bytearray(plan.table)
            n = next(
                i
                for i in range(len(base) // 128)
                if struct.unpack_from("<I", base, 128 * i)[0] == H.KIND
            )
            runner.install(plan)
            for stage in (1, 2):
                table = bytearray(base)
                struct.pack_into("<I", table, 128 * n + 60, stage)
                runner.put(plan.table_at, table)
                runner.submit(plan, job, linked=True, fault=True, descriptor_fault=True)
                controls[f"fault_after_{'pool' if stage == 1 else 'heads'}"] = True
            bad_fields = [(i, 0x4000) for i in range(1, 15)] + [(9, T.WIN_HI), (15, 3), (16, 1)]
            for field, value in bad_fields:
                table = bytearray(base)
                struct.pack_into("<I", table, 128 * n + 4 * field, value)
                runner.put(plan.table_at, table)
                runner.submit(plan, job, linked=True, reject=True)
            controls["head_descriptor_rejections"] = len(bad_fields)
            runner.install(plan)
            runner.admin(7, reject=True)
            controls["second_seal_refused"] = True
            _, _, _, clean, _ = runner.frame_heads(blob)
            if clean != expected:
                raise ValueError("clean heads frame after controls differs")
            controls["clean_after_controls"] = True
        crops[name] |= {
            "accuracy": checked,
            "pool_max_abs": pool_error,
            "bit_identical_tail_checkpoints": sorted(seen),
            "bit_identical_head_frames": rounds,
            "frames": frames,
            "controls": controls,
        }
    return crops, seal_ms


def summarize(crops):
    import numpy as np

    result = {
        "crops": len(crops),
        "jobs_per_frame": 1,
        "output_bytes": 512,
        "max_pool_incremental_abs": max(c["pool_max_abs"] for c in crops.values()),
        "max_landmark_px": max(c["accuracy"]["landmarks"]["max_px"] for c in crops.values()),
        "incremental_caps": H.CAPS,
        "max_incremental_abs": {
            n: max(c["accuracy"]["incremental_max_abs"][n] for c in crops.values()) for n in H.ORDER
        },
    }
    for mode in ("cpu_heads", "dsp_heads"):
        fs = [f for c in crops.values() for f in c["frames"][mode]]
        result[mode] = {
            "frames": len(fs),
            "wrapper_total_ms": float(
                np.median([sum(j["wrapper_ms"] for j in f["jobs"]) for f in fs])
            ),
            "body_total_ms": float(np.median([sum(j["body_ms"] for j in f["jobs"]) for f in fs])),
            "host_wall_ms": float(np.median([f["host_wall_ms"] for f in fs])),
        }
    return result
