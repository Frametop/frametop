"""Paired normalized-RGB -> heads comparison, using hardware_teacher_link's lease,
audits, sealed image, refusal controls and no-recovery lifecycle. Run with
--stem --cpu-prefix-library /path/to/libncnn_prefix.so --cpu-core 2.
"""

from contextlib import ExitStack
import json
import time

import hardware_teacher_heads as HH
import hardware_teacher_link as L
import hardware_teacher_stem as HS
import teacher_heads_layout as H
import teacher_resident_layout as R
import teacher_tail_layout as T
from teacher_cpu_prefix import CpuPrefix

MODES = ("cpu_fp32", "cpu_fp16", "dsp_prefix")


def mode_order(modes, round_):
    """Rotate across a complete cycle, then reverse the next cycle."""
    cut = round_ % len(modes)
    order = list(modes[cut:] + modes[:cut])
    if (round_ // len(modes)) % 2:
        order.reverse()
    return order


def checkpoint_difference(actual, reference, allowance, *, final=False):
    """Check intermediate drift against the frozen layer arithmetic bound.
    The complete final feature map is additionally required to be byte-exact.
    """
    import numpy as np

    delta = np.abs(actual - reference)
    changed = actual != reference
    ratio = float(np.max(delta / allowance))
    if final or ratio > 1:
        raise ValueError("CPU-prefix tail checkpoint differs outside the frozen layer bound")
    return {
        "different_values": int(np.count_nonzero(changed)),
        "max_abs_difference": float(np.max(delta)),
        "max_bound_ratio": ratio,
        "max_changed_magnitude": float(
            np.max(np.maximum(np.abs(actual[changed]), np.abs(reference[changed])))
        )
        if np.any(changed)
        else 0.0,
    }


def frame(runner, rgb, cpu=None):
    """Same loaded RGB input and DSP tail; no parameter setup inside this timer."""
    t0 = time.perf_counter()
    if cpu is None:
        runner.stage_rgb(rgb)
        t1 = time.perf_counter()
        prefix = runner.submit(runner.prefix, runner.prefix.jobs[0], linked=True)
        t2 = time.perf_counter()
        stages = {
            "cpu_prefix_ms": 0.0,
            "input_stage_ms": (t1 - t0) * 1000,
            "prefix_submit_wall_ms": (t2 - t1) * 1000,
        }
        jobs = [prefix]
    else:
        outputs = cpu.compute(rgb)
        t1 = time.perf_counter()
        runner.inputs(runner.tail, lambda name: outputs[name])
        t2 = time.perf_counter()
        stages = {
            "cpu_prefix_ms": (t1 - t0) * 1000,
            "input_stage_ms": (t2 - t1) * 1000,
            "prefix_submit_wall_ms": 0.0,
        }
        jobs = []
    jobs.append(runner.submit(runner.tail, runner.tail.jobs[0], linked=True))
    t3 = time.perf_counter()
    raw = runner.read_values(runner.tail, "heads", 512)
    t4 = time.perf_counter()
    if any(j["body_ms"] > 20 for j in jobs):
        raise ValueError("comparison request exceeds 20 ms body budget")
    stages |= {
        "tail_submit_wall_ms": (t3 - t2) * 1000,
        "output_read_ms": (t4 - t3) * 1000,
        "host_wall_ms": (t4 - t0) * 1000,
        "jobs": jobs,
    }
    return stages, raw


def run(runner, corpus, manifest, rounds):
    import numpy as np

    # Keep the complete prior DSP qualification and controls. Its original
    # timing frames are reported separately from the paired comparison below.
    crops, seal_ms = HS.run(runner, corpus, manifest, rounds)
    shapes = runner.sh | {"in0": (3, 224, 224)}
    for crop in manifest["crops"]:

        def blob(name):
            path = corpus / crop["name"] / ("in0.f32" if name == "in0" else f"blob_{name}.f32")
            if L.digest(path) != crop["files"][name]["sha256"]:
                raise ValueError("comparison corpus hash differs")
            return np.frombuffer(path.read_bytes(), "<f4").reshape(shapes[name])

        rgb = blob("in0")
        with ExitStack() as stack:
            cpus = {
                mode: stack.enter_context(
                    CpuPrefix(runner.cpu_library, runner.cpu_model, fp16=mode == "cpu_fp16")
                )
                for mode in MODES[:2]
            }
            references, checks = {}, {}
            for mode, cpu in cpus.items():
                # Prewarm CPU allocation, dispatch and weight transforms; no
                # image files, library loading or model setup in frame timing.
                for _ in range(20):
                    cpu.compute(rgb)
                output = {k: v.copy() for k, v in cpu.compute(rgb).items()}

                def actual_blob(k):
                    return output[k] if k in output else blob(k)

                t, ratios, reference = runner.qualify(actual_blob)
                host = T.heads(runner.graph, runner.specs, t[R.FINAL])
                _, seen, pool, expected, _ = runner.frame_heads(actual_blob, observe=True)
                differences = {}
                for k, v in seen.items():
                    if v != reference[k]:
                        grid = T.grid_of(runner.sh[k])
                        a, pad_a = grid.from_bytes(v, runner.sh[k][0])
                        b, pad_b = grid.from_bytes(reference[k], runner.sh[k][0])
                        if pad_a or pad_b or k == R.FINAL:
                            raise ValueError(
                                "CPU-prefix final map or padding differs from qualification"
                            )
                        op = next(o for o in runner.plans["qualify"].ops if o.dst == k)
                        allowance = T.bound_dw(runner.graph, op.spec, t[op.src])
                        differences[k] = checkpoint_difference(a, b, allowance)
                pool_error = float(
                    np.max(np.abs(np.frombuffer(pool, "<f4") - H.pool_reference(t[R.FINAL])))
                )
                if pool_error > H.CAPS["pool"]:
                    raise ValueError("comparison pool exceeds frozen cap")
                eligible, error = True, None
                try:
                    accuracy = HH.accuracy(expected, host, blob)
                except ValueError as exc:
                    if str(exc) != "head output exceeds frozen ncnn accuracy ceiling":
                        raise
                    eligible, error = False, str(exc)
                    values = H.split(np.frombuffer(expected, "<f4"))
                    accuracy = {
                        "ncnn": {k: T.compare(values[k], blob(k)) for k in H.ORDER},
                        "landmarks": T.landmark_error(values["out0"], blob("out0")),
                    }
                checks[mode] = {
                    "accuracy": accuracy,
                    "eligible": eligible,
                    "error": error,
                    "max_tail_bound_ratio": max(ratios.values()),
                    "pool_max_abs": pool_error,
                    "prefix_ncnn": {k: T.compare(output[k], blob(k)) for k in R.INPUTS},
                    "checkpoint_differences": differences,
                }
                references[mode] = expected
                print(
                    json.dumps(
                        {
                            "phase": "comparison_qualify",
                            "crop": crop["name"],
                            "mode": mode,
                            "eligible": eligible,
                            "landmarks": checks[mode]["accuracy"]["landmarks"],
                            "checkpoint_differences": differences,
                        }
                    ),
                    flush=True,
                )
            runner.prepare_rgb()
            _, expected_dsp, _ = runner.frame_rgb(rgb)
            references["dsp_prefix"] = expected_dsp
            modes = tuple(m for m in MODES if m == "dsp_prefix" or checks[m]["eligible"])
            # Warm the submission path too. Each mode must match its own
            # independently qualified outputs, rather than the other mode.
            for mode in modes:
                _, raw = frame(runner, rgb, cpus.get(mode))
                if raw != references[mode]:
                    raise ValueError("comparison warmup output differs")
            frames = {mode: [] for mode in MODES}
            orders = []
            for round_ in range(rounds):
                order = mode_order(modes, round_)
                orders.append(order)
                for mode in order:
                    sample, raw = frame(runner, rgb, cpus.get(mode))
                    if raw != references[mode]:
                        raise ValueError(f"{mode}: comparison output differs")
                    frames[mode].append(sample)
                    print(
                        json.dumps(
                            {
                                "phase": "comparison",
                                "crop": crop["name"],
                                "round": round_,
                                "mode": mode,
                                "host_wall_ms": sample["host_wall_ms"],
                                "cpu_prefix_ms": sample["cpu_prefix_ms"],
                                "wrapper_ms": sum(j["wrapper_ms"] for j in sample["jobs"]),
                            }
                        ),
                        flush=True,
                    )
            crops[crop["name"]]["comparison"] = {
                "checks": checks,
                "frames": frames,
                "orders": orders,
                "cpu_build": next(iter(cpus.values())).record,
            }
    return crops, seal_ms


def summarize(crops):
    import numpy as np

    def med(values):
        return float(np.median(values))

    result = {"original_dsp_qualification": HS.summarize(crops), "comparison": {}}
    for mode in MODES:
        frames = [f for c in crops.values() for f in c["comparison"]["frames"][mode]]
        qualified = [name for name, c in crops.items() if c["comparison"]["frames"][mode]]
        if not frames:
            result["comparison"][mode] = {
                "frames": 0,
                "qualified_crops": [],
                "eligible_on_all_crops": False,
            }
            continue
        result["comparison"][mode] = {
            "qualified_crops": qualified,
            "eligible_on_all_crops": len(qualified) == len(crops),
            "frames": len(frames),
            "jobs_per_frame": len(frames[0]["jobs"]),
            "output_bytes": 512,
            **{
                k: med([f[k] for f in frames])
                for k in (
                    "host_wall_ms",
                    "cpu_prefix_ms",
                    "input_stage_ms",
                    "prefix_submit_wall_ms",
                    "tail_submit_wall_ms",
                    "output_read_ms",
                )
            },
            "dsp_total_ms": med([sum(j["wrapper_ms"] for j in f["jobs"]) for f in frames]),
            "host_wall_range_ms": [
                min(f["host_wall_ms"] for f in frames),
                max(f["host_wall_ms"] for f in frames),
            ],
            "submission_non_dsp_ms": med(
                [sum(j["wall_ms"] - j["wrapper_ms"] for j in f["jobs"]) for f in frames]
            ),
        }
    for mode in MODES[:2]:
        paired = [
            d["host_wall_ms"] - h["host_wall_ms"]
            for c in crops.values()
            for d, h in zip(
                c["comparison"]["frames"]["dsp_prefix"], c["comparison"]["frames"][mode]
            )
        ]
        if paired:
            result["comparison"][mode]["median_paired_wall_saving_ms"] = med(paired)
    return result
