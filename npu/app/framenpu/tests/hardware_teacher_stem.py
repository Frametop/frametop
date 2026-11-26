"""Banded RGB-prefix qualification and two-request timing via hardware_teacher_link.

Private activations stay on the device. Intermediates are checked band by band
against the FP16 contract using the actual preceding DSP stage as input.
"""

import json
import struct
import time

import hardware_teacher_heads as HH
import hardware_teacher_link as L
import teacher_heads_layout as H
import teacher_resident_layout as R
import teacher_stem_layout as S
import teacher_tail_layout as T

DEBUG = T.WIN_LO + 0x10000


class StemRunner(HH.HeadsRunner):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.prefix, self.tail = S.build_plan(self.graph, self.specs)
        self.plans["stem"] = self.prefix

    def prepare_rgb(self):
        self.install(self.tail)
        self.install(self.prefix)

    def stage_rgb(self, rgb):
        import numpy as np

        self.put(self.prefix.where["in0"], np.asarray(rgb, "<f2").tobytes())

    def frame_rgb(self, rgb):
        """Fixed tables/weights installed separately, with no between-request staging."""
        t0 = time.perf_counter()
        self.stage_rgb(rgb)
        rows = [
            self.submit(self.prefix, self.prefix.jobs[0], linked=True),
            self.submit(self.tail, self.tail.jobs[0], linked=True),
        ]
        if any(j["body_ms"] > 20 for j in rows):
            raise ValueError("RGB request exceeds 20 ms body budget")
        raw = self.read_values(self.tail, "heads", 512)
        return rows, raw, (time.perf_counter() - t0) * 1000

    def qualify_prefix(self, rgb):
        import numpy as np

        by = {s["name"]: s for s in self.specs}
        self.install(self.prefix)
        self.stage_rgb(rgb)
        outputs = {"8": np.zeros((64, 56, 56)), "10": np.zeros((16, 56, 56))}
        ratios, band_jobs = [], []
        for band in range(7):
            actual = []
            previous = None
            for stage, name in enumerate(S.NAMES, 1):
                row = list(struct.unpack("<32I", self.prefix.table))
                row[1:3], row[16:18] = [band, band + 1], [stage, DEBUG]
                if not S.native_ok(row, grayscale=getattr(self, "grayscale", False)):
                    raise ValueError("invalid diagnostic prefix descriptor")
                self.put(self.prefix.table_at, struct.pack("<32I", *row))
                job = self.submit(self.prefix, self.prefix.jobs[0], linked=True)
                raw = bytes(self.mem[DEBUG : DEBUG + S.BAND.tensor_bytes(S.CHANNELS[stage - 1])])
                out, padding = S.BAND.from_bytes(raw, S.CHANNELS[stage - 1])
                if padding or not np.isfinite(out).all():
                    raise ValueError("prefix padding/nonfinite output differs")
                rows = range(16 * band - 1, 16 * band + 19)
                spec = by[name]
                if stage == 1:
                    exact = S.emulate_stem(self.graph, spec, rgb, rows)
                    bound = S.bound_stem(self.graph, spec, rgb, rows)
                elif stage in (2, 5):
                    spec = spec | {"stride": (1, 1)}
                    exact = S.emulate_layer(self.graph, spec, actual[-1], dw=True)
                    bound = T.bound_dw(self.graph, spec, actual[-1])
                else:
                    exact = S.emulate_layer(self.graph, spec, actual[-1])
                    bound = T.bound_pw(self.graph, spec, actual[-1])
                valid = (np.asarray(rows) >= 0) & (np.asarray(rows) < 112)
                exact[:, ~valid] = 0
                errors = np.abs(out - exact) / bound
                # DW1's contract is stride 2. Its other temporary positions
                # are never outputs or inputs to another layer.
                checked_errors = errors[:, 2:18:2, 1::2] if stage == 5 else errors
                ratio = float(np.max(checked_errors))
                if ratio > 1 or np.count_nonzero(out[:, ~valid]):
                    raise ValueError(f"band {band}, {name}: FP16 bound/mask differs ({ratio})")
                actual.append(out)
                ratios.append(
                    {
                        "band": band,
                        "stage": name,
                        "max_bound_ratio": ratio,
                        "checked_values": int(checked_errors.size),
                    }
                )
                # Every diagnostic executes the complete band. All five observed
                # runs must produce identical final outputs for that band.
                this = {k: self.raw(self.prefix, k) for k in R.INPUTS}
                if stage > 1 and this != previous:
                    raise ValueError("diagnostic prefix output is not deterministic")
                previous = this
            # Stitch the actual DSP stages, proving the scalar pool/decimator.
            expected = {"8": actual[4][:, 2:18:2, 1::2], "10": S.maxpool(actual[2][:, 1:17])}
            for k in R.INPUTS:
                grid, stray = S.OUTPUT.from_bytes(previous[k], self.sh[k][0])
                part = grid[:, 8 * band : 8 * band + 8]
                if stray or not np.array_equal(part, expected[k]):
                    raise ValueError(f"band {band}, {k}: pool/decimation differs")
                grid[:, 8 * band : 8 * band + 8] = 0
                if np.count_nonzero(grid):
                    raise ValueError("prefix wrote outside its selected output band")
                outputs[k][:, 8 * band : 8 * band + 8] = expected[k]
            band_jobs.append(job)
            print(
                json.dumps(
                    {
                        "phase": "prefix_band",
                        "band": band,
                        "wrapper_ms": job["wrapper_ms"],
                        "max_bound_ratio": max(r["max_bound_ratio"] for r in ratios[-5:]),
                    }
                ),
                flush=True,
            )
        return outputs, ratios, band_jobs


def run(runner, corpus, manifest, rounds):
    import numpy as np

    shapes = runner.sh | {"in0": (3, 224, 224)}

    def loader(crop):
        def blob(name):
            path = corpus / crop["name"] / ("in0.f32" if name == "in0" else f"blob_{name}.f32")
            if L.digest(path) != crop["files"][name]["sha256"]:
                raise ValueError("corpus hash differs")
            return np.frombuffer(path.read_bytes(), "<f4").reshape(shapes[name]).astype(np.float64)

        return blob

    seal_ms = runner.seal()
    crops = {}
    for index, crop in enumerate(manifest["crops"]):
        name, blob = crop["name"], loader(crop)
        rgb = blob("in0")
        outputs, ratios, bands = runner.qualify_prefix(rgb)
        prefix_raw = {k: S.OUTPUT.to_bytes(outputs[k]) for k in R.INPUTS}

        def actual_blob(k):
            return outputs[k] if k in outputs else blob(k)

        # The original per-layer kernels qualify the tail from actual prefix
        # output, providing an independent reference for the merged request.
        t, tail_ratios, reference = runner.qualify(actual_blob)
        host = T.heads(runner.graph, runner.specs, t[R.FINAL])
        _, seen, pool, expected, _ = runner.frame_heads(actual_blob, observe=True)
        if any(raw != reference[k] for k, raw in seen.items()):
            raise ValueError("tail from RGB prefix differs from per-layer qualification")
        pool_error = float(
            np.max(np.abs(np.frombuffer(pool, "<f4") - H.pool_reference(t[R.FINAL])))
        )
        if pool_error > H.CAPS["pool"]:
            raise ValueError("pool exceeds frozen incremental cap")
        values = H.split(np.frombuffer(expected, "<f4"))
        print(
            json.dumps(
                {
                    "phase": "rgb_accuracy",
                    "crop": name,
                    "landmarks": T.landmark_error(values["out0"], blob("out0")),
                    "ncnn": {k: T.compare(values[k], blob(k)) for k in H.ORDER},
                    "prefix": {k: T.compare(outputs[k], blob(k)) for k in R.INPUTS},
                }
            ),
            flush=True,
        )
        checked = HH.accuracy(expected, host, blob)
        runner.prepare_rgb()
        frames = []
        for round_ in range(rounds):
            jobs, raw, wall = runner.frame_rgb(rgb)
            if raw != expected or any(
                runner.raw(runner.prefix, k) != prefix_raw[k] for k in R.INPUTS
            ):
                raise ValueError("two-request RGB frame differs from observed qualification")
            HH.accuracy(raw, host, blob)
            frames.append({"jobs": jobs, "host_wall_ms": wall})
            print(
                json.dumps(
                    {
                        "phase": "rgb_frame",
                        "crop": name,
                        "round": round_,
                        "host_wall_ms": wall,
                        "wrapper_ms": [j["wrapper_ms"] for j in jobs],
                    }
                ),
                flush=True,
            )
        controls = {}
        if index == 0:
            plan, job = runner.prefix, runner.prefix.jobs[0]
            base = list(struct.unpack("<32I", plan.table))
            for stage in range(1, 7):
                row = base.copy()
                row[2], row[15] = 1, stage
                runner.put(plan.table_at, struct.pack("<32I", *row))
                runner.submit(plan, job, linked=True, fault=True, descriptor_fault=True)
            controls["guarded_stage_faults"] = 6
            bad = [
                (1, 7),
                (2, 8),
                (3, 7),
                (4, 19),
                (5, T.WIN_HI),
                (6, T.WIN_HI),
                (7, S.PARAMS - 1),
                (8, S.BLOCK - 1),
                (9, base[5]),
                (10, base[9]),
                (11, 0),
                (12, 0),
                (13, 0),
                (14, 0),
                (15, 7),
                (16, 6),
                (17, 128),
                (18, 1),
            ]
            for field, value in bad:
                row = base.copy()
                row[field] = value
                runner.put(plan.table_at, struct.pack("<32I", *row))
                runner.submit(plan, job, linked=True, reject=True)
            controls["descriptor_rejections"] = len(bad)
            runner.admin(7, reject=True)
            controls["second_seal_refused"] = True
            runner.prepare_rgb()
            _, raw, _ = runner.frame_rgb(rgb)
            if raw != expected:
                raise ValueError("clean RGB frame after controls differs")
            controls["clean_after_controls"] = True
        crops[name] = {
            "split": crop["split"],
            "prefix_bound_ratios": ratios,
            "band_jobs": bands,
            "tail_max_bound_ratio": max(tail_ratios.values()),
            "accuracy": checked,
            "pool_max_abs": pool_error,
            "ncnn_prefix": {k: T.compare(outputs[k], blob(k)) for k in R.INPUTS},
            "bit_identical_tail_checkpoints": sorted(seen),
            "bit_identical_rgb_frames": rounds,
            "frames": frames,
            "controls": controls,
        }
    return crops, seal_ms


def summarize(crops):
    import numpy as np

    frames = [f for c in crops.values() for f in c["frames"]]
    result = {
        "crops": len(crops),
        "frames": len(frames),
        "jobs_per_frame": 2,
        "input_bytes": 301056,
        "output_bytes": 512,
        "host_wall_ms": float(np.median([f["host_wall_ms"] for f in frames])),
        "max_landmark_px": max(c["accuracy"]["landmarks"]["max_px"] for c in crops.values()),
        "max_prefix_bound_ratio": max(
            r["max_bound_ratio"] for c in crops.values() for r in c["prefix_bound_ratios"]
        ),
        "incremental_caps": H.CAPS,
    }
    for i, name in enumerate(("prefix", "tail_heads")):
        result[name] = {
            field: float(np.median([f["jobs"][i][field] for f in frames]))
            for field in ("wrapper_ms", "body_ms")
        }
        result[name]["wrapper_range_ms"] = [
            min(f["jobs"][i]["wrapper_ms"] for f in frames),
            max(f["jobs"][i]["wrapper_ms"] for f in frames),
        ]
    return result
