"""Verify full/quantized ncnn oracles and freeze weight-derived FP16 tolerances.

Requires NumPy. Thresholds depend on the weight/bias model and FP16 precision,
not observed DSP errors. No headset access or performance optimization occurs.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu.convolution import load_landmark_graph


def stats(actual, expected, np):
    difference = np.asarray(actual, dtype=np.float64) - expected
    return {
        "max_abs": float(np.max(np.abs(difference))),
        "rms": float(np.sqrt(np.mean(difference * difference))),
    }


def full_reference_limits(reports, name):
    calibration = [r for r in reports if r["layer"] == name and r["split"] == "calibration"]
    if not calibration:
        raise ValueError("missing calibration precision estimates")
    limits = {}
    for field, stat, grid in (("full_max_abs", "max_abs", 1e-4), ("full_rms", "rms", 1e-5)):
        bound = 1.25 * max(
            r["input_quantization_vs_full"][stat] + r["ideal_fp16_store_vs_quantized_ncnn"][stat]
            for r in calibration
        )
        if not math.isfinite(bound) or bound < 0:
            raise ValueError("invalid calibration precision estimate")
        limits[field] = math.ceil(bound / grid) * grid
    return limits


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    args = parser.parse_args(argv)
    import numpy as np

    graph = load_landmark_graph(args.model / "hand.ncnn.param", args.model / "hand.ncnn.bin")
    manifest = json.loads((args.corpus / "manifest.json").read_text())
    if (graph.param_sha256, graph.weights_sha256) != (
        manifest["param_sha256"],
        manifest["weights_sha256"],
    ):
        parser.error("model hashes differ from corpus")
    tolerances = {
        "format": "framenpu-convolution-tolerances",
        "version": 1,
        "param_sha256": graph.param_sha256,
        "weights_sha256": graph.weights_sha256,
        "corpus_manifest_sha256": hashlib.sha256(
            (args.corpus / "manifest.json").read_bytes()
        ).hexdigest(),
        "derivation": "rtol=2^-10 (one FP16 ULP); atol=rtol*max_abs_bias+1e-4 FP32 reduction allowance; evaluated against ncnn with layer inputs rounded to FP16",
        "acceptance": "every output finite; zero raw accumulator nonfinites; abs(DSP-qref)<=atol+rtol*abs(qref); full-reference allowance adds abs(qref-fullref)",
        "layers": {},
    }
    reports = []
    for name in manifest["layers"]:
        layer = graph.convolution(name)
        weights = np.array(layer.weights(graph.raw), dtype=np.float64)
        bias = np.array(layer.bias(graph.raw), dtype=np.float64)
        rtol = 2**-10
        atol = rtol * float(np.max(np.abs(bias))) + 1e-4
        tolerances["layers"][name] = {
            "atol": atol,
            "rtol": rtol,
            "reference_fp64_check_atol": 1e-4,
            "max_abs_bias": float(np.max(np.abs(bias))),
        }
        for crop in manifest["crops"]:
            path = args.corpus / crop["name"]
            for file, meta in crop["files"].items():
                if hashlib.sha256((path / file).read_bytes()).hexdigest() != meta["sha256"]:
                    parser.error("corpus tensor hash differs")
            source = path / (
                "in0.f32" if layer.bottom == "in0" else "blob_" + layer.bottom + ".f32"
            )
            data = np.fromfile(source, dtype="<f4")
            qdata = np.fromfile(path / (name + "_qinput.f32"), dtype="<f4")
            n, h, w = layer.output_shape
            oracle = np.empty((n, h * w), dtype=np.float64)
            qoracle = oracle.copy()
            predicted = oracle.copy()
            for start in range(0, h * w, 32):
                patch = np.array(layer.patches(data, start), dtype=np.float64)
                qpatch = np.array(layer.patches(qdata, start), dtype=np.float64)
                full = patch @ weights + bias
                quantized = qpatch @ weights + bias
                # Ideal full-depth accumulator, final FP16 store, FP32 bias.
                hmx = (qpatch @ weights).astype(np.float16).astype(np.float32) + bias.astype(
                    np.float32
                )
                if layer.activation == "clip":
                    full = np.clip(full, 0, 6)
                    quantized = np.clip(quantized, 0, 6)
                    hmx = np.clip(hmx, 0, 6)
                rows = len(patch)
                oracle[:, start : start + rows] = full.T
                qoracle[:, start : start + rows] = quantized.T
                predicted[:, start : start + rows] = hmx.T
            reference = np.fromfile(path / ("blob_" + layer.top + ".f32"), dtype="<f4").reshape(
                n, h * w
            )
            qreference = np.fromfile(path / (name + "_qref.f32"), dtype="<f4").reshape(n, h * w)
            full_error = stats(oracle, reference, np)
            quant_error = stats(qoracle, qreference, np)
            if full_error["max_abs"] > 1e-4 or quant_error["max_abs"] > 1e-4:
                parser.error("independent FP64 lowering does not match strict ncnn")
            budget = atol + rtol * np.abs(qreference)
            row = {
                "crop": crop["name"],
                "split": crop["split"],
                "layer": name,
                "values": n * h * w,
                "fp64_vs_ncnn": full_error,
                "fp64_quantized_vs_ncnn": quant_error,
                "input_quantization_vs_full": stats(qreference, reference, np),
                "ideal_fp16_store_vs_quantized_ncnn": stats(predicted, qreference, np),
                "ideal_fp16_store_max_budget_ratio": float(
                    np.max(np.abs(predicted - qreference) / budget)
                ),
            }
            reports.append(row)
            print(
                crop["name"],
                name,
                "FP64 oracle max",
                full_error["max_abs"],
                "input quant",
                row["input_quantization_vs_full"],
                "ideal FP16",
                row["ideal_fp16_store_vs_quantized_ncnn"],
            )
    tolerances["full_reference_derivation"] = (
        "1.25 times worst calibration-only sum of input-quantization and ideal final-FP16-store errors; max rounded up to 1e-4, RMS to 1e-5; heldout crops never set limits"
    )
    tolerances["acceptance"] += (
        "; max absolute and RMS errors against original FP32 ncnn must also meet full_max_abs/full_rms"
    )
    for name, policy in tolerances["layers"].items():
        policy.update(full_reference_limits(reports, name))
    (args.corpus / "tolerances.json").write_text(json.dumps(tolerances, indent=2) + "\n")
    (args.corpus / "reference_analysis.json").write_text(json.dumps(reports, indent=2) + "\n")
    print("Frozen tolerances:", tolerances["layers"])


if __name__ == "__main__":
    main()
