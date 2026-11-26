"""Forecast Mercury keypoint DSP time from measured teacher per-op costs.

No device access. Per-op HMX pointwise and HVX depthwise compute times measured
for the 42 teacher tail jobs (evidence/teacher-tail-fp16.json) are fitted with
small linear cost models, which are then applied to the lowered Mercury ops.
The compute sum is scaled to a whole-request DSP time by the teacher's own
measured ratio between linked-request time and per-op compute, and host time
is added from the single-request teacher comparison.

This is a forecast, not a measurement. Op kinds the teacher never measured
(dense 3x3 heads via im2col, the 2x2 transposed convolution, FP32 tail ops)
are costed by analogy and listed as such.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tests"))
from framenpu import mercury

# Teacher GAP + four FP32 heads, measured as one epilogue descriptor including its
# FP32 parameter copy (docs/TEACHER_HEADS.md); weight values from the v20a graph.
TEACHER_HEADS_MS = 1.238
TEACHER_HEAD_WEIGHTS = 2 * (42336 + 63) + 2 * (672 + 1)
HOST_OPS = {"Gemm_58"}  # 42->8192 hint branch: proposed host-side, staged as FP16
VTCM_VECTORS = 15296  # largest owned per-job span used by the teacher jobs
VECTOR_BYTES = 128


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def pad32(n):
    return -(-n // 32) * 32


def teacher_samples(model_dir):
    import numpy as np
    from framenpu.convolution import load_landmark_graph
    from framenpu.landmark import specifications
    from teacher_tail_layout import tail_ops

    graph = load_landmark_graph(model_dir / "hand.ncnn.param", model_dir / "hand.ncnn.bin")
    specs = {s["name"]: s for s in specifications(graph)}
    ops, _ = tail_ops(list(specs.values()))
    evidence = json.loads((ROOT / "evidence/teacher-tail-fp16.json").read_text())
    times = {}
    for crop in evidence["crops"].values():
        for rnd in crop["rounds"]:
            for job in rnd["jobs"]:
                times.setdefault(job["op"], []).append(job["compute_us"])
    pw, dw = [], []
    for op in ops:
        s = specs[op.name]
        ci, h, w = s["input_shape"]
        co, oh, ow = s["output_shape"]
        us = float(np.median(times[op.name]))
        if op.kind == "pw":
            pw.append((pad32(co) * pad32(ci) * pad32(oh * ow), co * oh * ow, bool(op.res), us))
        else:
            taps = s["kernel"][0] * s["kernel"][1]
            stride2 = tuple(s["stride"]) == (2, 2)
            full = ci * h * w  # stride-1 pass size (stride 2 computes full res then gathers)
            dw.append((taps * full, full, co * oh * ow if stride2 else 0, us))
    return pw, dw, graph


def fit(rows):
    import numpy as np

    a = np.array([r[:-1] + (1.0,) for r in rows], float)
    y = np.array([r[-1] for r in rows], float)
    # Non-negative least squares by subset enumeration (four features): a cost
    # model must not credit time back for any feature.
    best = None
    for mask in range(1, 1 << a.shape[1]):
        cols = [i for i in range(a.shape[1]) if mask >> i & 1]
        sub, *_ = np.linalg.lstsq(a[:, cols], y, rcond=None)
        if (sub < 0).any():
            continue
        coef = np.zeros(a.shape[1])
        coef[cols] = sub
        err = float(np.sum((a @ coef - y) ** 2))
        if best is None or err < best[0]:
            best = (err, coef)
    coef = best[1]
    pred = a @ coef
    return coef, float(np.max(np.abs(pred - y) / y)), float(np.sum(pred) / np.sum(y))


def main(argv=None):
    import numpy as np

    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--model", required=True, type=Path, help=mercury.KEYPOINT_NAME)
    parser.add_argument("--teacher-model", required=True, type=Path, help="v20a ncnn directory")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    model = mercury.load_keypoint(args.model)
    pw, dw, _ = teacher_samples(args.teacher_model)
    pw_coef, pw_err, pw_sum = fit([(m, o, float(r), us) for m, o, r, us in pw])
    dw_coef, dw_err, dw_sum = fit(dw)

    comparison = json.loads((ROOT / "evidence/teacher-prefix-comparison.json").read_text())
    teacher_tail_compute_ms = sum(us for *_, us in pw + dw) / 1000.0

    fp32_us_per_weight = TEACHER_HEADS_MS * 1000.0 / TEACHER_HEAD_WEIGHTS
    rows, compute_us, host_us = [], 0.0, 0.0
    for r in mercury.inventory(model):
        op = model.op(r["name"])
        est, basis = 0.0, "layout (no compute)"
        if r["kind"] == "conv" and r["conv_class"] == "depthwise":
            c, oh, ow = op.shape
            kh, kw = r["kernel"]
            stride2 = r["stride"] == [2, 2]
            full = c * oh * ow * (4 if stride2 else 1)
            x = np.array([kh * kw * full, full, c * oh * ow if stride2 else 0, 1.0])
            est, basis = float(x @ dw_coef), "fitted HVX depthwise"
        elif r["unit"] == "hmx":
            outvals = r["output_values"]
            x = np.array([r["padded_hmx_macs"], outvals, float(r.get("residual", False)), 1.0])
            est = float(x @ pw_coef)
            if r["kind"] == "deconv2x2":
                basis = "fitted HMX pointwise (4*cout channels) + pixel shuffle, by analogy"
            elif r["conv_class"] == "dense":
                # im2col packing: one extra pass over the K x positions matrix,
                # costed like depthwise per-value work (unmeasured for this kernel).
                cin = op.params["weight"].shape[1]
                k = op.params["weight"].shape[2] * op.params["weight"].shape[3]
                est += float(
                    np.array([0, cin * k * outvals // r["output_shape"][0], 0, 0]) @ dw_coef
                )
                basis = "fitted HMX pointwise on K=cin*kh*kw + im2col pass, by analogy"
            else:
                basis = "fitted HMX pointwise"
        elif r["unit"] == "fp32":
            values = r.get("weight_values", r["output_values"])
            est = fp32_us_per_weight * values
            basis = "teacher FP32 heads rate per weight value (incl. parameter copy)"
        if r["name"] in HOST_OPS:
            r = dict(r, unit="host")
            host_us += est
            basis = "host CPU (proposed); DSP FP32 cost shown for comparison"
            rows.append(dict(r, dsp_alternative_us=round(est, 1), forecast_us=0.0, basis=basis))
            continue
        compute_us += est
        rows.append(dict(r, forecast_us=round(est, 1), basis=basis))

    # Whole-request overhead ratio measured on the teacher tail: linked request DSP
    # time over the sum of its per-op compute. Host time per request is the
    # single-request (CPU prefix) path: submission time beyond DSP + input staging.
    one_request = comparison["summary"]["comparison"]["cpu_fp32"]
    tail_dsp_ms = one_request["dsp_total_ms"]
    host_extra_ms = one_request["submission_non_dsp_ms"] + one_request["input_stage_ms"]
    # Heads are costed explicitly above, so remove them from the teacher ratio.
    ratio = (tail_dsp_ms - TEACHER_HEADS_MS) / teacher_tail_compute_ms
    compute_ms = compute_us / 1000.0

    def dsp_ms(rows_, *, heads_on_host=False, direct_stride2=False):
        total = 0.0
        for r in rows_:
            us = r["forecast_us"]
            if heads_on_host and r["unit"] == "fp32":
                us = 0.0
            if direct_stride2 and r.get("conv_class") == "depthwise" and r["stride"] == [2, 2]:
                # Optimistic: a direct stride-2 kernel costing like stride 1 at output size.
                c, oh, ow = r["output_shape"]
                kh, kw = r["kernel"]
                us = float(np.array([kh * kw * c * oh * ow, c * oh * ow, 0, 1.0]) @ dw_coef)
            total += us
        return total / 1000.0 * ratio

    scenarios = {}
    for name, kw in (
        ("all_on_dsp_hint_on_host", {}),
        ("conv_on_dsp_heads_and_hint_on_host", {"heads_on_host": True}),
        (
            "conv_on_dsp_heads_on_host_direct_stride2",
            {"heads_on_host": True, "direct_stride2": True},
        ),
    ):
        d = dsp_ms(rows, **kw)
        scenarios[name] = {
            "dsp_ms_per_crop": d,
            "host_wall_ms_one_crop_per_request": d + host_extra_ms,
            "host_wall_ms_per_crop_two_per_request": d + host_extra_ms / 2,
            "dsp_bound_crops_per_second": 1000.0 / d,
            "fits_20ms_native_budget_one_crop": d <= 20.0,
        }
    host_macs = sum(r.get("macs", 0) for r in rows if r["unit"] in ("fp32", "host"))

    stage = {}
    for r in rows:
        if r["unit"] in ("hmx", "hvx", "fp32"):
            key = "x".join(str(v) for v in r["output_shape"][1:])
            stage[key] = stage.get(key, 0.0) + r["forecast_us"] / 1000.0
    big = [
        {
            "op": r["name"],
            "fp16_bytes": r["output_values"] * 2,
            "vtcm_vectors": math.ceil(r["output_values"] * 2 / VECTOR_BYTES),
        }
        for r in rows
        if r["output_values"] * 2 > VTCM_VECTORS * VECTOR_BYTES // 4
    ]
    weights = {"fp16_device_bytes": 0, "fp32_bytes": 0}
    for r in rows:
        if "weight_values" in r:
            if r["unit"] == "host":
                continue
            key = "fp16_device_bytes" if r["unit"] in ("hmx", "hvx") else "fp32_bytes"
            weights[key] += r["weight_values"] * (2 if key.startswith("fp16") else 4)

    report = {
        "format": "framenpu-mercury-keypoint-forecast",
        "version": 1,
        "measured": False,
        "model_sha256": model.sha256,
        "teacher_fit": {
            "pointwise": {
                "features": ["padded_hmx_macs", "output_values", "residual", "constant"],
                "coefficients_us": [float(c) for c in pw_coef],
                "max_relative_error": pw_err,
                "sum_ratio": pw_sum,
                "samples": len(pw),
            },
            "depthwise": {
                "features": [
                    "taps*stride1_values",
                    "stride1_values",
                    "stride2_outputs",
                    "constant",
                ],
                "coefficients_us": [float(c) for c in dw_coef],
                "max_relative_error": dw_err,
                "sum_ratio": dw_sum,
                "samples": len(dw),
            },
            "teacher_tail_compute_ms": teacher_tail_compute_ms,
            "fp32_us_per_weight_value": fp32_us_per_weight,
            "teacher_tail_request_dsp_ms": tail_dsp_ms,
            "request_over_compute_ratio": ratio,
        },
        "forecast": {
            "compute_ms": compute_ms,
            "compute_by_resolution_ms": stage,
            "one_request_dsp_ms": compute_ms * ratio,
            "host_wall_ms_one_crop_per_request": compute_ms * ratio + host_extra_ms,
            "host_overhead_ms_per_request": host_extra_ms,
            "hint_branch_on_dsp_fp32_ms_if_not_on_host": host_us / 1000.0,
            "two_crops_per_request_host_wall_ms_per_crop": compute_ms * ratio + host_extra_ms / 2,
        },
        "scenarios": scenarios,
        "host_side_fp32_macs_if_heads_and_hint_on_host": host_macs,
        "activations_over_quarter_vtcm": big,
        "weights": weights,
        "ops": rows,
        "source_sha256": {
            "tool": digest(Path(__file__)),
            "mercury.py": digest(ROOT / "src/framenpu/mercury.py"),
            "teacher-tail-fp16.json": digest(ROOT / "evidence/teacher-tail-fp16.json"),
            "teacher-prefix-comparison.json": digest(
                ROOT / "evidence/teacher-prefix-comparison.json"
            ),
        },
    }
    args.output.write_text(json.dumps(report, indent=1) + "\n")
    print(json.dumps({k: report[k] for k in ("forecast", "scenarios", "weights")}, indent=1))
    print(json.dumps(big))


if __name__ == "__main__":
    main()
