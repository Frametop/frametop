"""Export pinned ncnn references from private recorded snapshot PNGs.

Requires Pillow for PNG decoding. The external ncnn executable performs the
FrameTop affine and normalization. Raw frames/crops/weights stay in --output.
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

MODEL_REV = "03d7695fecb80f6cf8f3aa9321de130b5efd08b3"
PARAM_SHA = "54e08f284ff6f121ff00f4a9c65afc38c0dbb22a32f4af44fc4e0fde6c4a1bbd"
WEIGHTS_SHA = "b7c3d1be232725a5a143e2648858ec5aec9596c7b83c6a3c508479b04057e00c"
NCNN_REV = "e54f7b1f88434e1d844ea0551b880a1cfb079ce1"
NCNN_ZIP_SHA = "754659d6fe65545cf2ef4483ffb84526fea631f8764c44b150f1601d0fb4004b"
FRAMETOP_REV = "edd7e61fdaa76bfa71f532cf8cb8e0a6bf60d729"
CASES = (
    (
        "calibration_controller",
        "BeginningSnapshot/slam_right.png",
        (460, 395, 260, 0.7),
        "calibration",
    ),
    ("calibration_open", "EndSnapshot/slam_left.png", (355, 115, 300, -2.4), "calibration"),
    ("heldout_left_controller", "EndSnapshot/slam_left.png", (210, 470, 300, -1.0), "heldout"),
    ("heldout_right_controller", "EndSnapshot/slam_right.png", (830, 460, 300, 1.4), "heldout"),
)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recording", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--layer-reference", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    graph = load_landmark_graph(args.model / "hand.ncnn.param", args.model / "hand.ncnn.bin")
    if graph.param_sha256 != PARAM_SHA or graph.weights_sha256 != WEIGHTS_SHA:
        parser.error("reference corpus requires the pinned v20a param/bin hashes")
    from PIL import Image
    import numpy as np

    args.output.mkdir(parents=True, exist_ok=True)
    manifest = {
        "format": "framenpu-convolution-reference",
        "version": 1,
        "model_revision": MODEL_REV,
        "param_sha256": PARAM_SHA,
        "weights_sha256": WEIGHTS_SHA,
        "ncnn_revision": NCNN_REV,
        "ncnn_source_zip_sha256": NCNN_ZIP_SHA,
        "reference_executable_sha256": digest(args.reference),
        "layer_reference_executable_sha256": digest(args.layer_reference),
        "frametop_revision": FRAMETOP_REV,
        "contrast": "none",
        "normalization": "FP32 replicated grayscale times (1/255.f)",
        "reference_options": {
            "threads": 1,
            "packing": False,
            "fp16": False,
            "bf16": False,
            "winograd": False,
            "sgemm": False,
            "vulkan": False,
        },
        "layers": {},
        "crops": [],
    }
    for name in ("convclip_0", "conv_10", "conv_12"):
        layer = graph.convolution(name)
        isolated = args.output / (name + ".param")
        line = next(
            line
            for line in (args.model / "hand.ncnn.param").read_text().splitlines()
            if line.split()[1:2] == [name]
        )
        fields = line.split()
        fields[4:6] = ["input", "output"]
        isolated.write_text("7767517\n2 2\nInput input 0 1 input\n" + " ".join(fields) + "\n")
        (args.output / (name + ".bin")).write_bytes(graph.raw[layer.segment[0] : layer.segment[1]])
        manifest["layers"][name] = {
            "bottom": layer.bottom,
            "top": layer.top,
            "input_shape": layer.input_shape,
            "output_shape": layer.output_shape,
            "kernel": layer.kernel,
            "stride": layer.stride,
            "dilation": layer.dilation,
            "padding": layer.padding,
            "activation": layer.activation,
            "weight_segment_sha256": hashlib.sha256(
                graph.raw[layer.segment[0] : layer.segment[1]]
            ).hexdigest(),
            "reduction": layer.reduction,
        }
    for name, relative, roi, split in CASES:
        source = args.recording / "snapshots" / relative
        image = Image.open(source)
        if image.mode != "L":
            parser.error("recorded snapshots must already be grayscale PNG")
        output = args.output / name
        output.mkdir(exist_ok=True)
        raw = output / "frame.u8"
        raw.write_bytes(image.tobytes())
        command = [
            str(args.reference.resolve()),
            str((args.model / "hand.ncnn.param").resolve()),
            str((args.model / "hand.ncnn.bin").resolve()),
            str(raw.resolve()),
            str(image.width),
            str(image.height),
            *[str(v) for v in roi],
            str(output.resolve()),
            "none",
        ]
        result = subprocess.run(command, check=True, text=True, capture_output=True)
        for layer_name in manifest["layers"]:
            layer = graph.convolution(layer_name)
            source_tensor = output / (
                "in0.f32" if layer.bottom == "in0" else "blob_" + layer.bottom + ".f32"
            )
            original = np.fromfile(source_tensor, dtype="<f4")
            quantized = original.astype("<f2").astype("<f4")
            qinput = output / (layer_name + "_qinput.f32")
            qinput.write_bytes(quantized.tobytes())
            qref = output / (layer_name + "_qref.f32")
            subprocess.run(
                [
                    str(args.layer_reference.resolve()),
                    str((args.output / (layer_name + ".param")).resolve()),
                    str((args.output / (layer_name + ".bin")).resolve()),
                    str(qinput.resolve()),
                    *[str(v) for v in layer.input_shape],
                    str(qref.resolve()),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
        files = {
            file.name: {"sha256": digest(file), "bytes": file.stat().st_size}
            for file in sorted(output.iterdir())
            if file.suffix in (".f32", ".pgm")
        }
        manifest["crops"].append(
            {
                "name": name,
                "split": split,
                "recorded_snapshot": relative,
                "source_png_sha256": digest(source),
                "source_shape": [image.height, image.width],
                "roi_center_size_rotation": roi,
                "crop_shape": [224, 224],
                "files": files,
                "reference_shapes": result.stdout.splitlines(),
            }
        )
        print(name, result.stdout.strip().replace("\n", "; "))
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
