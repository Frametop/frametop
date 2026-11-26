"""Measure teacher output drift at smaller crops using the pinned strict CPU oracle.

Private source frames and head arrays stay local; output contains statistics
and provenance only. This measures agreement with 224, not labeled accuracy.
"""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / n) for n in ("src", "tests", "tools")]
import build_ncnn_reference as B
from framenpu.landmark import PARAM_SHA, WEIGHTS_SHA
import teacher_tail_layout as T

ORDER = ("out0", "out3", "out2", "out1")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compare_heads(actual, reference, *, xy_scale=1.0, roi_size=224.0):
    import numpy as np

    a = actual["out0"].astype(np.float64).reshape(21, 3)
    r = reference["out0"].astype(np.float64).reshape(21, 3)
    distance = np.linalg.norm(a[:, :2] * xy_scale - r[:, :2], axis=1)
    result = {
        "landmarks": {
            "mean_px": float(distance.mean()),
            "median_px": float(np.median(distance)),
            "p95_px": float(np.percentile(distance, 95)),
            "max_px": float(distance.max()),
            "roi_normalized_mean": float(distance.mean() / 224),
            "source_image_mean_px": float(distance.mean() * roi_size / 224),
            "source_image_max_px": float(distance.max() * roi_size / 224),
            "per_landmark_displacement_px": distance.tolist(),
        },
        "world": T.compare(actual["out3"], reference["out3"]),
    }
    for name, key in (("out1", "presence"), ("out2", "handedness")):
        v = float(actual[name][0])
        ref = float(reference[name][0])
        result[key] = {
            "value": v,
            "reference": ref,
            "abs_delta": abs(v - ref),
            "threshold_0_5_changed": (v >= 0.5) != (ref >= 0.5),
        }
    return result


class Oracle:
    def __init__(self, library, model):
        import numpy as np

        self.lib = ctypes.CDLL(str(library))
        f32 = np.ctypeslib.ndpointer(dtype=np.float32, flags="C_CONTIGUOUS")
        u8 = np.ctypeslib.ndpointer(dtype=np.uint8, flags="C_CONTIGUOUS")
        f64 = np.ctypeslib.ndpointer(dtype=np.float64, flags="C_CONTIGUOUS")
        i32 = np.ctypeslib.ndpointer(dtype=np.int32, flags="C_CONTIGUOUS")
        self.lib.fnpu_resolution_create.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        self.lib.fnpu_resolution_create.restype = ctypes.c_void_p
        self.lib.fnpu_resolution_destroy.argtypes = [ctypes.c_void_p]
        self.lib.fnpu_resolution_destroy.restype = None
        self.lib.fnpu_resolution_crop.argtypes = [
            u8,
            ctypes.c_int,
            ctypes.c_int,
            f64,
            ctypes.c_int,
            ctypes.c_int,
            f32,
        ]
        self.lib.fnpu_resolution_crop.restype = ctypes.c_int
        self.lib.fnpu_resolution_run.argtypes = [ctypes.c_void_p, f32, ctypes.c_int, f32, i32]
        self.lib.fnpu_resolution_run.restype = ctypes.c_int
        self.handle = self.lib.fnpu_resolution_create(
            os.fsencode(model / "hand.ncnn.param"), os.fsencode(model / "hand.ncnn.bin")
        )
        if not self.handle:
            raise ValueError("resolution oracle could not load teacher")

    def close(self):
        if self.handle:
            self.lib.fnpu_resolution_destroy(self.handle)
            self.handle = None

    def crop(self, frame, roi, crop_n, input_n):
        import numpy as np

        gray = np.empty((input_n, input_n), np.float32)
        status = self.lib.fnpu_resolution_crop(
            frame,
            frame.shape[1],
            frame.shape[0],
            np.asarray(roi, np.float64),
            crop_n,
            input_n,
            gray,
        )
        if status:
            raise ValueError(f"resolution crop failed: {status}")
        return gray

    def run(self, gray):
        import numpy as np

        heads = np.empty(128, np.float32)
        shape = np.empty(3, np.int32)
        status = self.lib.fnpu_resolution_run(self.handle, gray, gray.shape[0], heads, shape)
        if status or not np.isfinite(heads).all():
            raise ValueError(f"resolution inference failed: {status}")
        return dict(
            zip(ORDER, (heads[:63], heads[63:126], heads[126:127], heads[127:]))
        ), shape.tolist()


def build(reference, output):
    record = json.loads((reference / "reference_build.json").read_text())
    library = reference / "install/lib/libncnn.a"
    if (
        record["archive_sha256"] != B.ARCHIVE_SHA
        or record["build_options"] != B.OPTIONS
        or digest(library) != record["library_sha256"]
    ):
        raise ValueError("strict reference provenance differs")
    source = ROOT / "tools/ncnn_resolution_bridge.cpp"
    flags = ["-std=c++17", "-O2", "-ffp-contract=off", "-fno-fast-math", "-shared", "-fPIC"]
    subprocess.run(
        [
            "g++",
            *flags,
            "-I",
            str(reference / "install/include/ncnn"),
            str(source),
            str(library),
            "-lpthread",
            "-o",
            str(output),
        ],
        check=True,
    )
    return {
        "strict_reference": record,
        "bridge_source_sha256": digest(source),
        "library_sha256": digest(output),
        "flags": flags,
    }


def run(oracle, model, source_corpus, oracle_corpus, target):
    import numpy as np
    from framenpu.convolution import load_landmark_graph

    source = json.loads((source_corpus / "manifest.json").read_text())
    pinned = json.loads((oracle_corpus / "manifest.json").read_text())
    for manifest in (source, pinned):
        if (manifest["param_sha256"], manifest["weights_sha256"]) != (PARAM_SHA, WEIGHTS_SHA):
            raise ValueError("corpus teacher differs")
    if pinned["prior_manifest_sha256"] != digest(source_corpus / "manifest.json"):
        raise ValueError("source crop recipes differ from oracle provenance")
    graph = load_landmark_graph(
        model / "hand.ncnn.param", model / "hand.ncnn.bin", input_shape=(3, target, target)
    )
    if (graph.param_sha256, graph.weights_sha256) != (PARAM_SHA, WEIGHTS_SHA):
        raise ValueError("model hashes differ")
    result = {
        "target": target,
        "model": {"param_sha256": graph.param_sha256, "weights_sha256": graph.weights_sha256},
        "target_feature_shape": list(graph.shapes["79"]),
        "source_manifest_sha256": digest(source_corpus / "manifest.json"),
        "oracle_manifest_sha256": digest(oracle_corpus / "manifest.json"),
        "crops": {},
    }
    source_by = {c["name"]: c for c in source["crops"]}
    for crop in pinned["crops"]:
        name = crop["name"]
        recipe = source_by[name]
        folder = oracle_corpus / name

        def blob(key):
            path = folder / ("in0.f32" if key == "in0" else f"blob_{key}.f32")
            if digest(path) != crop["files"][key]["sha256"]:
                raise ValueError("oracle blob hash differs")
            return np.frombuffer(path.read_bytes(), "<f4")

        # Crop recipes are tied to the original PNG hashes by the source
        # manifest. Require byte-exact recreation of the pinned normalized
        # 224 input before interpreting any lower-resolution result.
        raw_path = source_corpus / name / "frame.u8"
        frame = np.frombuffer(raw_path.read_bytes(), np.uint8).reshape(recipe["source_shape"])
        roi = recipe["roi_center_size_rotation"]
        gray = oracle.crop(frame, roi, 224, 224)
        baseline_input = blob("in0").reshape(3, 224, 224)
        if any(gray.tobytes() != plane.tobytes() for plane in baseline_input):
            raise ValueError("224 affine/normalization does not reproduce pinned input")
        reference = {k: blob(k) for k in ORDER}
        baseline, shape = oracle.run(gray)
        if any(baseline[k].tobytes() != reference[k].tobytes() for k in ORDER):
            raise ValueError("224 strict model does not reproduce pinned heads exactly")
        modes = {}
        for mode, crop_n, input_n in (
            ("native_224", 224, 224),
            ("native_target", target, target),
            ("target_upscaled_224", target, 224),
        ):
            if mode == "native_224":
                heads, shape_ = baseline, shape
            else:
                heads, shape_ = oracle.run(oracle.crop(frame, roi, crop_n, input_n))
            checked = compare_heads(heads, reference, roi_size=roi[2])
            checked["feature_shape"] = shape_
            checked["crop_size"] = crop_n
            checked["network_input_size"] = input_n
            if mode == "native_target":
                checked["alternative_scale_target_to_224"] = compare_heads(
                    heads, reference, xy_scale=224 / target, roi_size=roi[2]
                )
            modes[mode] = checked
        result["crops"][name] = {
            "split": crop["split"],
            "source_frame_sha256": digest(raw_path),
            "baseline_input_and_heads_byte_exact": True,
            "modes": modes,
        }
        print(
            json.dumps(
                {
                    "crop": name,
                    "mean_landmark_drift_px": modes["native_target"]["landmarks"]["mean_px"],
                    "max_landmark_drift_px": modes["native_target"]["landmarks"]["max_px"],
                }
            ),
            flush=True,
        )
    return result


def summarize(result):
    import numpy as np

    def aggregate(rows):
        if not rows:
            return None
        d = np.asarray(
            [v for row in rows for v in row["landmarks"]["per_landmark_displacement_px"]]
        )
        return {
            "mean_px": float(d.mean()),
            "median_px": float(np.median(d)),
            "p95_px": float(np.percentile(d, 95)),
            "max_px": float(d.max()),
            "presence_threshold_changes": sum(r["presence"]["threshold_0_5_changed"] for r in rows),
            "handedness_threshold_changes": sum(
                r["handedness"]["threshold_0_5_changed"] for r in rows
            ),
            "max_world_abs_delta": max(r["world"]["max_abs"] for r in rows),
        }

    crops = list(result["crops"].values())
    positive = [c for c in crops if c["modes"]["native_224"]["presence"]["reference"] >= 0.5]
    summary = {}
    for group, selected in (("all_crops", crops), ("reference_presence_positive", positive)):
        summary[group] = {"crops": len(selected), "modes": {}}
        for mode in ("native_224", "native_target", "target_upscaled_224"):
            summary[group]["modes"][mode] = aggregate([c["modes"][mode] for c in selected])
        summary[group]["alternative_scaled_native_target"] = aggregate(
            [c["modes"]["native_target"]["alternative_scale_target_to_224"] for c in selected]
        )
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ("reference", "model", "source-corpus", "oracle-corpus", "output"):
        p.add_argument("--" + n, type=Path, required=True)
    p.add_argument("--target", type=int, default=128)
    args = p.parse_args()
    if args.target not in (128, 160, 192):
        p.error("target must be 128, 160 or 192")
    with tempfile.TemporaryDirectory() as tmp:
        library = Path(tmp) / "libresolution.so"
        record = build(args.reference, library)
        oracle = Oracle(library, args.model)
        try:
            result = run(oracle, args.model, args.source_corpus, args.oracle_corpus, args.target)
        finally:
            oracle.close()
    result |= {
        "format": "framenpu-teacher-resolution-drift",
        "build": record,
        "method": "Unmodified strict FP32 CPU teacher, same affine ROI recropped from raw grayscale frame; no DSP change or retraining. Native head coordinates retain the 224-trained units; alternative target-to-224 scaling reported separately. No labeled ground truth: these are prediction differences, not landmark accuracy loss.",
        "source_sha256": digest(Path(__file__)),
        "summary": summarize(result),
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result["summary"], indent=2))


if __name__ == "__main__":
    main()
