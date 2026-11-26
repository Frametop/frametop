"""FP16 device-contract accuracy of the teacher tail on the qualified corpus.

Runs the emulated device numerics (tests/teacher_tail_layout.py) from each
crop's ncnn blobs 8 and 10 through convdwclip_15, then FP32 pooling/heads,
and compares every tensor and head with the crop's ncnn FP32 blobs. Writes
error statistics only (no tensors). Opens no device.
"""

import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tests"), str(ROOT / "tools")]


def load_crop(corpus, crop, shape_of):
    import numpy as np

    def blob(name):
        raw = (corpus / crop["name"] / f"blob_{name}.f32").read_bytes()
        if hashlib.sha256(raw).hexdigest() != crop["files"][name]["sha256"]:
            raise ValueError(f"corpus blob hash differs: {crop['name']}/{name}")
        return np.frombuffer(raw, "<f4").reshape(shape_of[name]).astype(np.float64)

    return blob


def run(graph, specs, blob, emulate=True):
    from teacher_tail_layout import emulate_dw, emulate_pw, heads, tail_ops

    by = {s["name"]: s for s in specs}
    ops, _ = tail_ops(specs)
    t = {"8": blob("8"), "10": blob("10")}
    for op in ops:
        s = by[op.name]
        if op.kind == "pw":
            t[op.dst] = emulate_pw(graph, s, t[op.src], t[op.res] if op.res else None)
        else:
            t[op.dst] = emulate_dw(graph, s, t[op.src])
    return ops, t, heads(graph, specs, t["79"])


def summarize(graph, specs, ops, t, h, blob):
    from teacher_tail_layout import compare, heads, landmark_error

    layers = {op.dst: compare(t[op.dst], blob(op.dst)) | {"op": op.name} for op in ops}
    ref_heads = {k: blob(k) for k in ("out0", "out1", "out2", "out3")}
    host_heads = heads(graph, specs, blob("79"))  # host pooling/heads alone, from the oracle blob
    return {
        "layers": layers,
        "heads": {k: compare(h[k], ref_heads[k]) for k in ref_heads},
        "landmarks": landmark_error(h["out0"], ref_heads["out0"]),
        "world_max_abs": compare(h["out3"], ref_heads["out3"])["max_abs"],
        "presence": [float(h["out2"][0]), float(ref_heads["out2"][0])],
        "handedness": [float(h["out1"][0]), float(ref_heads["out1"][0])],
        "host_heads_only": {k: compare(host_heads[k], ref_heads[k])["max_abs"] for k in ref_heads},
    }


def main():
    from framenpu.convolution import load_landmark_graph
    from framenpu.landmark import specifications
    from teacher_tail_layout import shapes

    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--model", required=True, type=Path)
    p.add_argument("--corpus", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()
    graph = load_landmark_graph(args.model / "hand.ncnn.param", args.model / "hand.ncnn.bin")
    specs = specifications(graph)
    manifest = json.loads((args.corpus / "manifest.json").read_text())
    if (
        manifest["param_sha256"] != graph.param_sha256
        or manifest["weights_sha256"] != graph.weights_sha256
    ):
        p.error("corpus/model mismatch")
    shape_of = shapes(specs) | {"in0": (3, 224, 224)}
    report = {
        "format": "framenpu-teacher-tail-accuracy",
        "contract": "FP16 storage, HMX FP16 1x1, QFloat16 depthwise (modelled)",
        "crops": {},
    }
    for crop in manifest["crops"]:
        blob = load_crop(args.corpus, crop, shape_of)
        ops, t, h = run(graph, specs, blob)
        report["crops"][crop["name"]] = {"split": crop["split"]} | summarize(
            graph, specs, ops, t, h, blob
        )
    args.output.write_text(json.dumps(report, indent=1) + "\n")
    for name, c in report["crops"].items():
        worst = max(c["layers"].items(), key=lambda kv: kv[1]["max_abs"])
        print(
            json.dumps(
                {
                    "crop": name,
                    "landmarks": c["landmarks"],
                    "world_max_abs": c["world_max_abs"],
                    "presence": c["presence"],
                    "handedness": c["handedness"],
                    "host_heads_only": c["host_heads_only"],
                    "worst_layer": [
                        worst[1]["op"],
                        worst[1]["max_abs"],
                        worst[1]["reference_max_abs"],
                    ],
                }
            )
        )


if __name__ == "__main__":
    main()
