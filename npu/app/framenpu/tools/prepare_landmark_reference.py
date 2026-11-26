"""Export every pinned ncnn blob and freeze full-graph calibration tolerances.

Reuses the private, hash-checked step-3 frame/ROI corpus; no model/recording data
is published. Heldout rows are reported but cannot influence tolerance fitting.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu.convolution import load_landmark_graph
from framenpu.landmark import PARAM_SHA, WEIGHTS_SHA, OUTPUTS, specifications, tensor_lifetimes
from landmark_reference import evaluate, errors, frozen_limits


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("model", "convolution-corpus", "reference", "output"):
        p.add_argument("--" + name, required=True, type=Path)
    args = p.parse_args()
    import numpy as np

    graph = load_landmark_graph(args.model / "hand.ncnn.param", args.model / "hand.ncnn.bin")
    if (graph.param_sha256, graph.weights_sha256) != (PARAM_SHA, WEIGHTS_SHA):
        p.error("requires the pinned v20a model")
    prior = json.loads((args.convolution_corpus / "manifest.json").read_text())
    if (prior["param_sha256"], prior["weights_sha256"]) != (PARAM_SHA, WEIGHTS_SHA):
        p.error("prior crop provenance differs")
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = {
        "format": "framenpu-landmark-reference",
        "version": 1,
        "param_sha256": PARAM_SHA,
        "weights_sha256": WEIGHTS_SHA,
        "reference_executable_sha256": digest(args.reference),
        "prior_manifest_sha256": digest(args.convolution_corpus / "manifest.json"),
        "provenance": {
            k: prior[k]
            for k in (
                "ncnn_revision",
                "ncnn_source_zip_sha256",
                "frametop_revision",
                "model_revision",
                "contrast",
                "normalization",
                "reference_options",
            )
        },
        "layout": tensor_lifetimes(graph),
        "crops": [],
    }
    specs = specifications(graph)
    analysis = []
    oracle_checks = []
    for crop in prior["crops"]:
        name = crop["name"]
        folder = args.output / name
        folder.mkdir(exist_ok=True)
        source = args.convolution_corpus / name / "frame.u8"
        # Frame dimensions/ROI are retained verbatim from the pinned crop recipe.
        command = [
            str(args.reference.resolve()),
            str((args.model / "hand.ncnn.param").resolve()),
            str((args.model / "hand.ncnn.bin").resolve()),
            str(source.resolve()),
            str(crop["source_shape"][1]),
            str(crop["source_shape"][0]),
            *[str(v) for v in crop["roi_center_size_rotation"]],
            str(folder.resolve()),
            "none",
            "all",
        ]
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
        for key in (
            "in0.f32",
            "crop.pgm",
            "blob_2.f32",
            "blob_3.f32",
            "blob_4.f32",
            "blob_8.f32",
            "blob_9.f32",
        ):
            if digest(folder / key) != crop["files"][key]["sha256"]:
                raise ValueError("all-blob export differs from pinned step-3 reference: " + key)
        tensors = {"in0": np.fromfile(folder / "in0.f32", dtype="<f4")}
        files = {}
        for s in specs:
            output = s["output"][0]
            actual = (
                tensors["in0"].reshape(s["output_shape"])
                if s["operator"] == "Input"
                else evaluate(graph, s, [tensors[k] for k in s["input"]])
            )
            for key in s["output"]:
                tensors[key] = actual
                path = folder / ("in0.f32" if key == "in0" else "blob_" + key + ".f32")
                if path.stat().st_size != math_prod(graph.shapes[key]) * 4:
                    raise ValueError("reference shape differs for " + key)
                files[key] = {"sha256": digest(path), "bytes": path.stat().st_size}
            reference = np.fromfile(
                folder / ("in0.f32" if output == "in0" else "blob_" + output + ".f32"), dtype="<f4"
            )
            error = errors(actual, reference)
            if s["operator"] != "Input":
                full_inputs = [
                    np.fromfile(
                        folder / ("in0.f32" if key == "in0" else "blob_" + key + ".f32"),
                        dtype="<f4",
                    )
                    for key in s["input"]
                ]
                independent = evaluate(graph, s, full_inputs, hmx=False).ravel()
                delta = np.abs(independent.astype(np.float64) - reference.astype(np.float64))
                allowance = 1e-4 + 2e-5 * np.abs(reference.astype(np.float64))
                if np.any(delta > allowance):
                    raise ValueError("independent CPU formula differs from ncnn: " + s["name"])
                oracle_checks.append(
                    {
                        "crop": name,
                        "layer": s["name"],
                        "values": len(reference),
                        "error": errors(independent, reference),
                        "max_ratio": float(np.max(delta / allowance)),
                    }
                )
            row = {
                "crop": name,
                "split": crop["split"],
                "layer": s["name"],
                "operator": s["operator"],
                "error": error,
                "reference_max_abs": float(np.max(np.abs(reference))),
                "reference_rms": float(np.sqrt(np.mean(reference.astype(np.float64) ** 2))),
            }
            analysis.append(row)
            if output in OUTPUTS:
                print(json.dumps(row), flush=True)
        manifest["crops"].append(
            {"name": name, "split": crop["split"], "source": crop, "files": files}
        )
    limits = frozen_limits(analysis)
    # Provisional graph-regression ceilings, set before any device observation.
    ceilings = {
        "linear_42": {"max_abs": 1.0, "rms": 0.25},
        "linear_43": {"max_abs": 0.003, "rms": 0.001},
        "fcsigmoid_0": {"max_abs": 0.005, "rms": 0.005},
        "fcsigmoid_1": {"max_abs": 0.005, "rms": 0.005},
    }
    for name, cap in ceilings.items():
        for key in cap:
            limits[name][key] = min(limits[name][key], cap[key])
    for row in analysis:
        if any(row["error"][key] > limits[row["layer"]][key] for key in ("max_abs", "rms")):
            raise ValueError("CPU precision estimate exceeds frozen limits: " + str(row))
    policy = {
        "format": "framenpu-landmark-tolerances",
        "version": 1,
        "calibration_only": True,
        "layers": limits,
        "head_ceilings": ceilings,
        "local_hmx_rtol": 2**-10,
        "local_hmx_atol": 1e-4,
        "local_hvx_rtol": 2e-5,
        "local_hvx_atol": 2e-5,
        "local_hvx_roundoff_unit": 2**-22,
        "local_sigmoid_atol": 2e-6,
        "method": "2x calibration CPU propagated max/RMS plus .002 max/.0002 RMS reference scale; absolute floors .002/.0002; capped semantic heads",
    }
    (args.output / "tolerances.json").write_text(json.dumps(policy, indent=2) + "\n")
    manifest["tolerances_sha256"] = digest(args.output / "tolerances.json")
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (args.output / "precision_analysis.json").write_text(json.dumps(analysis, indent=2) + "\n")
    (args.output / "oracle_checks.json").write_text(json.dumps(oracle_checks, indent=2) + "\n")
    print("Frozen graph tolerances", manifest["tolerances_sha256"])


def math_prod(shape):
    import math

    return math.prod(shape)


if __name__ == "__main__":
    main()
