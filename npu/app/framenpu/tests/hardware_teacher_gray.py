"""Single-plane 224x224 input, preserving the replicated-gray teacher exactly."""

import json
import struct

import hardware_teacher_link as L
import hardware_teacher_stem as HS
from hardware_teacher_stem_profile import switch_image
import teacher_resident_layout as R
import teacher_stem_layout as S


def weight_analysis(graph, specs):
    import numpy as np
    from framenpu.landmark import weights_and_bias

    spec = next(s for s in specs if s["name"] == S.NAMES[0])
    w, _ = weights_and_bias(graph, spec)
    weights = np.asarray(w, np.float16).astype(np.float64).reshape(24, 3, 9)
    summed = weights.sum(axis=1)
    error = np.abs(summed - summed.astype(np.float16).astype(np.float64))
    return {
        "folded_coefficients": int(summed.size),
        "coefficients_changed_by_fp16_rounding": int(np.count_nonzero(error)),
        "max_coefficient_rounding_abs": float(error.max()),
        "max_filter_l1_rounding": float(error.sum(axis=1).max()),
        "logical_k_rgb": 27,
        "logical_k_folded": 9,
        "padded_hmx_k_rgb": 32,
        "padded_hmx_k_folded": 32,
        "folded_weights_used": False,
    }


class GrayRunner(HS.StemRunner):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.rgb_prefix = self.prefix
        self.gray_prefix, _ = S.build_plan(self.graph, self.specs, grayscale=True)
        self.select_input(True)

    def select_input(self, grayscale):
        self.grayscale = grayscale
        self.prefix = self.gray_prefix if grayscale else self.rgb_prefix
        self.plans["stem"] = self.prefix

    def stage_gray(self, gray):
        import numpy as np

        if gray.shape != (224, 224):
            raise ValueError("grayscale input requires one normalized 224x224 plane")
        self.put(self.prefix.where["in0"], np.asarray(gray, "<f2").tobytes())

    def stage_rgb(self, rgb):
        # The inherited diagnostic oracle still consumes the original RGB
        # corpus. The timed single-plane API below consumes only gray.
        if self.grayscale:
            self.stage_gray(rgb[0] if rgb.ndim == 3 else rgb)
        else:
            super().stage_rgb(rgb)

    def frame_gray(self, gray):
        if not self.grayscale or gray.shape != (224, 224):
            raise ValueError("gray frame requires the grayscale plan and one 224x224 plane")
        return super().frame_rgb(gray)

    def frame_rgb(self, rgb):
        return self.frame_gray(rgb[0]) if self.grayscale else super().frame_rgb(rgb)

    def qualify_prefix(self, rgb):
        import numpy as np

        if (
            rgb.shape != (3, 224, 224)
            or not np.isfinite(rgb).all()
            or any(rgb[i].tobytes() != rgb[0].tobytes() for i in (1, 2))
        ):
            raise ValueError("grayscale qualification requires three identical finite planes")
        return super().qualify_prefix(rgb)


def run(runner, corpus, manifest, rounds):
    import numpy as np

    crops, seal_ms = HS.run(runner, corpus, manifest, rounds)
    gray_image = runner.image
    rgb_image = runner.gray_baseline_image
    for crop in manifest["crops"]:
        path = corpus / crop["name"] / "in0.f32"
        if L.digest(path) != crop["files"]["in0"]["sha256"]:
            raise ValueError("grayscale corpus hash differs")
        rgb = np.frombuffer(path.read_bytes(), "<f4").reshape(3, 224, 224)
        if any(rgb[i].tobytes() != rgb[0].tobytes() for i in (1, 2)):
            raise ValueError("corpus input is not replicated grayscale")
        gray = rgb[0]
        runner.select_input(False)
        switch_image(runner, rgb_image)
        runner.prepare_rgb()
        _, golden_heads, _ = runner.frame_rgb(rgb)
        golden = {k: runner.raw(runner.prefix, k) for k in R.INPUTS}
        frames = {mode: [] for mode in ("rgb", "grayscale")}
        for round_ in range(rounds):
            for mode in ("rgb", "grayscale") if round_ % 2 == 0 else ("grayscale", "rgb"):
                runner.select_input(mode == "grayscale")
                switch_image(runner, gray_image if runner.grayscale else rgb_image)
                runner.put(runner.prefix.table_at, runner.prefix.table)
                jobs, heads, wall = (
                    runner.frame_gray(gray) if runner.grayscale else runner.frame_rgb(rgb)
                )
                if (
                    heads != golden_heads
                    or {k: runner.raw(runner.prefix, k) for k in R.INPUTS} != golden
                ):
                    raise ValueError("single-plane frame differs from replicated RGB control")
                frames[mode].append({"jobs": jobs, "host_wall_ms": wall})
                print(
                    json.dumps(
                        {
                            "phase": "gray_comparison",
                            "crop": crop["name"],
                            "round": round_,
                            "mode": mode,
                            "host_wall_ms": wall,
                            "prefix_ms": jobs[0]["wrapper_ms"],
                        }
                    ),
                    flush=True,
                )
        crops[crop["name"]]["grayscale"] = {
            "replicated_planes_verified": True,
            "bit_identical_prefix_and_heads": True,
            "frames": frames,
        }
    runner.select_input(True)
    switch_image(runner, gray_image)
    runner.prepare_rgb()
    # The kind identifies the input format; reject RGB descriptors explicitly.
    row = list(struct.unpack("<32I", runner.prefix.table))
    row[0] = 7
    runner.put(runner.prefix.table_at, struct.pack("<32I", *row))
    runner.submit(runner.prefix, runner.prefix.jobs[0], linked=True, reject=True)
    runner.prepare_rgb()
    return crops, seal_ms


def summarize(crops):
    import numpy as np

    result = {"qualification": HS.summarize(crops), "modes": {}}
    result["qualification"]["input_bytes"] = 100352
    for mode in ("rgb", "grayscale"):
        frames = [f for c in crops.values() for f in c["grayscale"]["frames"][mode]]
        result["modes"][mode] = {
            "frames": len(frames),
            "input_bytes": 100352 if mode == "grayscale" else 301056,
            "host_wall_ms": float(np.median([f["host_wall_ms"] for f in frames])),
            "prefix_ms": float(np.median([f["jobs"][0]["wrapper_ms"] for f in frames])),
            "tail_heads_ms": float(np.median([f["jobs"][1]["wrapper_ms"] for f in frames])),
        }
    result["paired_host_saving_ms"] = float(
        np.median(
            [
                a["host_wall_ms"] - b["host_wall_ms"]
                for c in crops.values()
                for a, b in zip(
                    c["grayscale"]["frames"]["rgb"],
                    c["grayscale"]["frames"]["grayscale"],
                    strict=True,
                )
            ]
        )
    )
    return result
