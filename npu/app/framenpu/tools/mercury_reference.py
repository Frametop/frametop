"""Check the Mercury keypoint lowering and FP16 device contract on real hand crops.

CPU only; no DSP access. Three evaluations per crop are compared:

* ONNX Runtime FP32 (the backend steam-frame-hand-tracking uses), the oracle;
* the framenpu lowered graph in FP32 (validates the lowering itself);
* the framenpu lowered graph under the FP16 HMX/HVX contract.

Crops are cut from the private recorded frames of the existing convolution
corpus (``frame.u8`` plus ROI recipes), as 128x128 bilinear affine crops in 8
orientations, normalized the way Mercury does. They are real hands on real
fisheye frames but not Mercury's stereographic crops, so this measures
numerical drift, not tracking accuracy. Each crop runs without a hint and
then with the FP32 oracle's own keypoints as the hint.

The control is the model's own sensitivity: the FP32 oracle's keypoint shift
when the crop moves by one source pixel. Only statistics are written; crops,
keypoint positions and weights stay local.

Requires NumPy and onnxruntime.
"""

import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import mercury

IS_HAND_MIN = 0.97  # Mercury: accept a view only when the model is sure
CONFIDENT_PEAK = 0.3  # joints whose oracle heatmap peak is at least this (about the median)
VARIANTS = tuple((rot, mirror) for mirror in (False, True) for rot in range(4))


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def affine_crop(frame, roi, size=mercury.INPUT_SIZE, shift=(0.0, 0.0), turn=0, mirror=False):
    """Bilinear rotated-square crop. roi = (cx, cy, side, radians) in source pixels."""
    import numpy as np

    cx, cy, side, angle = roi
    angle = angle + turn * np.pi / 2
    g = (np.arange(size) + 0.5) / size - 0.5
    u, v = np.meshgrid(g, g)
    if mirror:
        u = -u
    c, s = np.cos(angle), np.sin(angle)
    x = cx + shift[0] + side * (c * u - s * v) - 0.5
    y = cy + shift[1] + side * (s * u + c * v) - 0.5
    h, w = frame.shape
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = x - x0, y - y0
    src = frame.astype(np.float64)

    def at(yy, xx):
        ok = (xx >= 0) & (xx < w) & (yy >= 0) & (yy < h)
        out = np.zeros(xx.shape)
        out[ok] = src[yy[ok], xx[ok]]
        return out

    val = (
        at(y0, x0) * (1 - fx) * (1 - fy)
        + at(y0, x0 + 1) * fx * (1 - fy)
        + at(y0 + 1, x0) * (1 - fx) * fy
        + at(y0 + 1, x0 + 1) * fx * fy
    )
    return np.clip(np.rint(val), 0, 255).astype(np.uint8)


class Oracle:
    def __init__(self, path):
        import onnxruntime as ort

        so = ort.SessionOptions()
        so.intra_op_num_threads = 1
        so.inter_op_num_threads = 1
        so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_DISABLE_ALL
        self.session = ort.InferenceSession(str(path), so, providers=["CPUExecutionProvider"])
        self.version = ort.__version__

    def run(self, feeds):
        names = list(mercury.OUTPUTS)
        return dict(zip(names, self.session.run(names, feeds)))


def feeds_for(img, hint=None):
    import numpy as np

    return {
        "inputImg": img[None, None],
        "lastKeypoints": hint if hint is not None else np.zeros((1, 42), np.float32),
        "useLastKeypoints": np.array([0.0 if hint is None else 1.0], np.float32),
    }


def compare(actual, reference):
    """Decoded and raw differences of one evaluation against the oracle."""
    import numpy as np

    a, r = mercury.decode(actual), mercury.decode(reference)
    kp = np.linalg.norm(a["crop_px"] - r["crop_px"], axis=1)
    sure = kp[r["peak"] >= CONFIDENT_PEAK]
    raw = {}
    for k in mercury.OUTPUTS:
        d = np.abs(actual[k].astype(np.float64) - reference[k])
        raw[k] = {
            "max_abs": float(d.max()),
            "rms": float(np.sqrt((d**2).mean())),
            "ref_max_abs": float(np.abs(reference[k]).max()),
        }
    return {
        "keypoint_px": {"mean": float(kp.mean()), "max": float(kp.max())},
        "confident_joints": int(sure.size),
        "confident_keypoint_max_px": float(sure.max()) if sure.size else 0.0,
        "rel_depth_max_abs": float(np.abs(a["rel_depth"] - r["rel_depth"]).max()),
        "peak_max_abs": float(np.abs(a["peak"] - r["peak"]).max()),
        "is_hand": {"value": a["is_hand"], "reference": r["is_hand"]},
        "is_hand_abs": abs(a["is_hand"] - r["is_hand"]),
        "curls_max_abs": float(np.abs(a["curls"] - r["curls"]).max()),
        "raw": raw,
    }


def layer_drift(model, feeds):
    """FP16-contract versus FP32 per lowered tensor, both from the same input."""
    import numpy as np

    _, exact = mercury.run(model, feeds, "fp32", keep=True)
    _, dev = mercury.run(model, feeds, "fp16", keep=True)
    rows = {}
    for op in model.ops:
        if mercury.unit(op) == "layout":
            continue
        e, d = exact[op.output].astype(np.float64), dev[op.output].astype(np.float64)
        scale = max(float(np.abs(e).max()), 1e-12)
        rows[op.name] = {
            "unit": mercury.unit(op),
            "max_abs": float(np.abs(d - e).max()),
            "relative_to_tensor_max": float(np.abs(d - e).max() / scale),
        }
    return rows


def evaluate(oracle, model, corpus):
    import numpy as np

    manifest = json.loads((corpus / "manifest.json").read_text())
    result = {"crops": {}, "corpus_manifest_sha256": digest(corpus / "manifest.json")}
    drift = {}
    for recipe in manifest["crops"]:
        name = recipe["name"]
        raw_path = corpus / name / "frame.u8"
        frame = np.frombuffer(raw_path.read_bytes(), np.uint8).reshape(recipe["source_shape"])
        roi = recipe["roi_center_size_rotation"]
        variants = []
        for turn, mirror in VARIANTS:
            u8 = affine_crop(frame, roi, turn=turn, mirror=mirror)
            img = mercury.normalize(u8)
            if img is None:
                raise ValueError(f"{name}: flat crop")
            passes = {}
            no_hint = oracle.run(feeds_for(img))
            hint = mercury.hint_from_grid(mercury.decode(no_hint)["grid"])
            for label, feeds in (("no_hint", feeds_for(img)), ("self_hint", feeds_for(img, hint))):
                ref = oracle.run(feeds)
                passes[label] = {
                    "reference_is_hand": mercury.decode(ref)["is_hand"],
                    "lowered_fp32": compare(mercury.run(model, feeds, "fp32"), ref),
                    "fp16_contract": compare(mercury.run(model, feeds, "fp16"), ref),
                }
                # Control: model sensitivity to a one-source-pixel crop shift.
                shifted = []
                for shift in ((1.0, 0.0), (0.0, 1.0)):
                    moved = mercury.normalize(
                        affine_crop(frame, roi, shift=shift, turn=turn, mirror=mirror)
                    )
                    f = feeds_for(moved, hint if label == "self_hint" else None)
                    shifted.append(compare(oracle.run(f), ref)["keypoint_px"])
                passes[label]["control_one_pixel_shift_keypoint_px"] = {
                    "mean": float(np.mean([s["mean"] for s in shifted])),
                    "max": float(max(s["max"] for s in shifted)),
                }
            for op, row in layer_drift(model, feeds_for(img, hint)).items():
                prev = drift.setdefault(op, dict(row, relative_to_tensor_max=0.0, max_abs=0.0))
                prev["max_abs"] = max(prev["max_abs"], row["max_abs"])
                prev["relative_to_tensor_max"] = max(
                    prev["relative_to_tensor_max"], row["relative_to_tensor_max"]
                )
            variants.append({"rotation_quarter_turns": turn, "mirror": mirror, "passes": passes})
            print(
                json.dumps(
                    {
                        "crop": name,
                        "turn": turn,
                        "mirror": mirror,
                        "is_hand": round(passes["self_hint"]["reference_is_hand"], 4),
                        "fp16_kp_max_px": round(
                            passes["self_hint"]["fp16_contract"]["keypoint_px"]["max"], 4
                        ),
                    }
                ),
                flush=True,
            )
        result["crops"][name] = {
            "split": recipe["split"],
            "source_frame_sha256": digest(raw_path),
            "variants": variants,
        }
    result["fp16_layer_drift_worst"] = drift
    return result


def summarize(result):
    import numpy as np

    def group(accepted_only):
        rows = []
        for crop in result["crops"].values():
            for v in crop["variants"]:
                for p in v["passes"].values():
                    if not accepted_only or p["reference_is_hand"] >= IS_HAND_MIN:
                        rows.append(p)
        if not rows:
            return {"evaluations": 0}

        def stat(path):
            vals = []
            for r in rows:
                x = r
                for k in path:
                    x = x[k]
                vals.append(x)
            return {
                "median": float(np.median(vals)),
                "p95": float(np.percentile(vals, 95)),
                "max": float(np.max(vals)),
            }

        return {
            "evaluations": len(rows),
            "lowered_fp32_raw_max_abs": max(
                r["lowered_fp32"]["raw"][k]["max_abs"] for r in rows for k in mercury.OUTPUTS
            ),
            "lowered_fp32_keypoint_max_px": stat(("lowered_fp32", "keypoint_px", "max")),
            "fp16_keypoint_mean_px": stat(("fp16_contract", "keypoint_px", "mean")),
            "fp16_keypoint_max_px": stat(("fp16_contract", "keypoint_px", "max")),
            "fp16_confident_keypoint_max_px": stat(("fp16_contract", "confident_keypoint_max_px")),
            "confident_joints": sum(r["fp16_contract"]["confident_joints"] for r in rows),
            "fp16_rel_depth_max_abs": stat(("fp16_contract", "rel_depth_max_abs")),
            "fp16_is_hand_abs": stat(("fp16_contract", "is_hand_abs")),
            "fp16_curls_max_abs": stat(("fp16_contract", "curls_max_abs")),
            "fp16_heatmap_xy_max_abs": stat(("fp16_contract", "raw", "heatmap_xy", "max_abs")),
            "control_one_pixel_shift_keypoint_mean_px": stat(
                ("control_one_pixel_shift_keypoint_px", "mean")
            ),
            "control_one_pixel_shift_keypoint_max_px": stat(
                ("control_one_pixel_shift_keypoint_px", "max")
            ),
            "acceptance_flips_at_0_97": sum(
                (r["fp16_contract"]["is_hand"]["value"] >= IS_HAND_MIN)
                != (r["reference_is_hand"] >= IS_HAND_MIN)
                for r in rows
            ),
        }

    return {"all": group(False), "reference_accepted_is_hand_ge_0_97": group(True)}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--model", required=True, type=Path, help=mercury.KEYPOINT_NAME)
    parser.add_argument(
        "--corpus", required=True, type=Path, help="convolution corpus with frame.u8"
    )
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    model = mercury.load_keypoint(args.model)  # refuses an unpinned model
    oracle = Oracle(args.model)
    result = evaluate(oracle, model, args.corpus)
    report = {
        "format": "framenpu-mercury-keypoint-reference",
        "version": 1,
        "cpu_only": True,
        "labeled_ground_truth": False,
        "model": {
            "name": mercury.KEYPOINT_NAME,
            "sha256": model.sha256,
            "revision": mercury.MODELS_REVISION,
            "license": "unverified (no license file upstream); not redistributed",
            "parameters": model.params,
            "lowered_ops": len(model.ops),
        },
        "oracle": {"onnxruntime": oracle.version, "threads": 1, "graph_optimization": "disabled"},
        "crop_method": "128x128 bilinear affine of private recorded fisheye frames, 8 orientations",
        "is_hand_threshold": IS_HAND_MIN,
        "confident_peak_threshold": CONFIDENT_PEAK,
        "source_sha256": {
            "tool": digest(Path(__file__)),
            "mercury.py": digest(ROOT / "src/framenpu/mercury.py"),
            "onnx_graph.py": digest(ROOT / "src/framenpu/onnx_graph.py"),
        },
        "summary": summarize(result),
        **result,
    }
    args.output.write_text(json.dumps(report, indent=1) + "\n")
    print(json.dumps(report["summary"], indent=1))


if __name__ == "__main__":
    main()
