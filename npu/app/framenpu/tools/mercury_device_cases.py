"""Stage private Mercury device-run cases from the convolution corpus (workstation side).

For each corpus crop, 2 orientations x {no hint, FP32 self-hint} give 16 cases.
Each case is an ``.npz`` with the FP16 tensors staged into the device window
(the FP32 host-prefix output ``input.44`` and the hint plane), the FP16-contract
expectation of the device output ``input.336``, and the FP32 and FP16-contract
outputs of the whole model, used by tests/hardware_mercury.py for keypoint drift.
For step 2b it also holds the FP16 image and the expectations under the full
FP16 contract (no FP32 host prefix).

The output directory is private: it holds activations derived from private
frames and from unlicensed weights. Keep it in /tmp, never in the repository.
The manifest pins the model and every case file by SHA256.

Requires NumPy.
"""

import argparse
import hashlib
import io
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tests"), str(ROOT / "tools")]
from framenpu import mercury
import mercury_layout as ML
import mercury_reference as R

ORIENTATIONS = (R.VARIANTS[0], R.VARIANTS[5])  # upright; mirrored and turned once


def cases(model, corpus):
    import numpy as np

    manifest = json.loads((corpus / "manifest.json").read_text())
    for rec in manifest["crops"]:
        frame = np.frombuffer((corpus / rec["name"] / "frame.u8").read_bytes(), np.uint8)
        frame = frame.reshape(rec["source_shape"])
        for turn, mirror in ORIENTATIONS:
            img = mercury.normalize(
                R.affine_crop(frame, rec["roi_center_size_rotation"], turn=turn, mirror=mirror)
            )
            ref0 = mercury.run(model, R.feeds_for(img), "fp32")
            hint = mercury.hint_from_grid(mercury.decode(ref0)["grid"])
            for hinted in (False, True):
                feeds = R.feeds_for(img, hint if hinted else None)
                fp32 = ref0 if not hinted else mercury.run(model, feeds, "fp32")
                fp16, t = mercury.run(model, feeds, "fp16", keep=True, host_prefix=ML.HOST_PREFIX)
                name = (
                    f"{rec['name']}-t{turn}{'m' if mirror else ''}-{'hint' if hinted else 'nohint'}"
                )
                arrays = {
                    "input": t[ML.INPUT].astype(np.float16),
                    "hint": t[ML.HINT].astype(np.float16),
                    "expected": t[ML.OUTPUT].astype(np.float16),
                }
                arrays |= {f"fp32/{k}": v for k, v in fp32.items()}
                arrays |= {f"fp16/{k}": v for k, v in fp16.items()}
                # step 2b: the whole network under the FP16 contract (DSP prefix)
                full, tf = mercury.run(model, feeds, "fp16", keep=True)
                arrays["image"] = tf["inputImg"].reshape(128, 128).astype(np.float16)
                arrays["expectedfull"] = tf[ML.OUTPUT].astype(np.float16)
                arrays |= {f"fp16full/{k}": v for k, v in full.items()}
                yield name, rec["split"], hinted, arrays


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--model", required=True, type=Path)
    p.add_argument("--corpus", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args(argv)
    import numpy as np

    if ROOT in args.output.resolve().parents or args.output.resolve() == ROOT:
        p.error("private cases must not be written inside the repository")
    model = mercury.load_keypoint(args.model)
    args.output.mkdir(parents=True, exist_ok=False)
    out = {
        "format": "framenpu-mercury-device-cases",
        "model_sha256": model.sha256,
        "corpus_manifest_sha256": R.digest(args.corpus / "manifest.json"),
        "host_prefix": ML.HOST_PREFIX,
        "cases": [],
    }
    for name, split, hinted, arrays in cases(model, args.corpus):
        buf = io.BytesIO()
        np.savez(buf, **arrays)
        data = buf.getvalue()
        (args.output / f"{name}.npz").write_bytes(data)
        out["cases"].append(
            {
                "name": name,
                "split": split,
                "hinted": hinted,
                "sha256": hashlib.sha256(data).hexdigest(),
            }
        )
    (args.output / "manifest.json").write_text(json.dumps(out, indent=1) + "\n")
    print(f"{len(out['cases'])} cases in {args.output}")


if __name__ == "__main__":
    main()
