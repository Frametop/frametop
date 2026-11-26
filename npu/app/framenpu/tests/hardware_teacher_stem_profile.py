"""Coarse full-prefix brackets and sampled per-operator profiling. No raw
activations leave the device. Driven by hardware_teacher_link --stem-profile.
"""

import json
import struct

import hardware_teacher_link as L
import hardware_teacher_stem as HS
import teacher_resident_layout as R
import teacher_stem_layout as S
import teacher_stem_profile as P


def switch_image(runner, image):
    if runner.image == image:
        return
    if runner.sealed:
        runner.admin(8)
    runner.image = image
    runner.reservation = image + bytes(L.CODE_BYTES - len(image))
    runner.seal()


def prefix(runner, rgb, level=0, *, band=None, phase=0):
    plan = runner.prefix
    desc = plan.table if not level else P.descriptor(plan, level, band=band, phase=phase)
    runner.put(plan.table_at, desc)
    runner.stage_rgb(rgb)
    row = runner.submit(plan, plan.jobs[0], linked=True)
    raw = {k: runner.raw(plan, k) for k in R.INPUTS}
    log = None
    if level:
        descriptor = struct.unpack("<32I", desc)
        at = descriptor[18]
        log = P.decode(bytes(runner.mem[at : at + P.VECTORS * 128]), descriptor, row["body_ms"])
    return row, raw, log


def run(runner, corpus, manifest, rounds):
    import numpy as np

    crops, seal_ms = HS.run(runner, corpus, manifest, rounds)
    profile_image = runner.image
    baseline_image = runner.profile_baseline_image
    vector_image = getattr(runner, "profile_vector_image", None)
    for crop in manifest["crops"]:
        path = corpus / crop["name"] / "in0.f32"
        if L.digest(path) != crop["files"]["in0"]["sha256"]:
            raise ValueError("profile corpus hash differs")
        rgb = np.frombuffer(path.read_bytes(), "<f4").reshape(3, 224, 224)
        runner.prepare_rgb()
        switch_image(runner, baseline_image)
        _, golden, _ = prefix(runner, rgb)
        _, golden_heads, _ = runner.frame_rgb(rgb)
        frames = {m: [] for m in ("original", "profile_disabled", "coarse")}
        if vector_image is not None:
            frames["vector_uninstrumented"] = []
        full_frames = {m: [] for m in ("original", "vector_uninstrumented")}
        for round_ in range(rounds):
            modes = list(frames)
            modes = modes[round_ % len(modes) :] + modes[: round_ % len(modes)]
            if (round_ // len(modes)) % 2:
                modes.reverse()
            for mode in modes:
                switch_image(
                    runner,
                    baseline_image
                    if mode == "original"
                    else vector_image
                    if mode == "vector_uninstrumented"
                    else profile_image,
                )
                row, raw, log = prefix(runner, rgb, 1 if mode == "coarse" else 0)
                if raw != golden:
                    raise ValueError(f"{mode}: profiled prefix differs from unmodified image")
                frames[mode].append({"job": row, "log": log})
                if vector_image is not None and mode in full_frames:
                    # Sealing and fixed weight/table setup stay outside frame timing.
                    runner.put(runner.prefix.table_at, runner.prefix.table)
                    jobs, heads, wall = runner.frame_rgb(rgb)
                    if (
                        heads != golden_heads
                        or {k: runner.raw(runner.prefix, k) for k in R.INPUTS} != golden
                    ):
                        raise ValueError("paired full RGB frame differs from scalar control")
                    full_frames[mode].append({"jobs": jobs, "host_wall_ms": wall})
                print(
                    json.dumps(
                        {
                            "phase": "profile_coarse",
                            "crop": crop["name"],
                            "round": round_,
                            "mode": mode,
                            "wrapper_ms": row["wrapper_ms"],
                        }
                    ),
                    flush=True,
                )
        switch_image(runner, profile_image)
        detailed = []
        for band in range(7):
            expected = {}
            for k in R.INPUTS:
                tensor, pad = S.OUTPUT.from_bytes(golden[k], runner.sh[k][0])
                if pad:
                    raise ValueError("golden prefix padding differs")
                tensor[:, : 8 * band] = 0
                tensor[:, 8 * band + 8 :] = 0
                expected[k] = S.OUTPUT.to_bytes(tensor)
            phases = []
            for phase in range(8):
                row, raw, log = prefix(runner, rgb, 2, band=band, phase=phase)
                if raw != expected:
                    raise ValueError(
                        f"band {band}, phase {phase}: detailed profiler changes output"
                    )
                phases.append({"phase": phase, "job": row, "log": log})
            row, raw, log = prefix(runner, rgb, 3, band=band)
            if raw != expected:
                raise ValueError("fully bracketed profile changes output")
            detailed.append({"band": band, "phases": phases, "full": {"job": row, "log": log}})
            print(
                json.dumps(
                    {
                        "phase": "profile_detail",
                        "crop": crop["name"],
                        "band": band,
                        "wrapper_ms": float(np.median([f["job"]["wrapper_ms"] for f in phases])),
                    }
                ),
                flush=True,
            )
        crops[crop["name"]]["profile"] = {
            "frames": frames,
            "full_frames": full_frames if vector_image is not None else {},
            "detailed": detailed,
            "bit_identical_profile_outputs": True,
        }
    switch_image(runner, profile_image)
    runner.prepare_rgb()
    return crops, seal_ms


def summarize(crops):
    import numpy as np

    def med(v):
        return float(np.median(v))

    result = {
        "qualification": HS.summarize(crops),
        "modes": {},
        "operators": {},
        "globals": {},
        "work_census": P.census(),
    }
    for mode in next(iter(crops.values()))["profile"]["frames"]:
        fs = [f for c in crops.values() for f in c["profile"]["frames"][mode]]
        result["modes"][mode] = {
            "frames": len(fs),
            "wrapper_ms": med([f["job"]["wrapper_ms"] for f in fs]),
            "body_ms": med([f["job"]["body_ms"] for f in fs]),
        }
    if next(iter(crops.values()))["profile"]["full_frames"]:
        result["paired_full_frames"] = {}
        for mode in ("original", "vector_uninstrumented"):
            fs = [f for c in crops.values() for f in c["profile"]["full_frames"][mode]]
            result["paired_full_frames"][mode] = {
                "frames": len(fs),
                "host_wall_ms": med([f["host_wall_ms"] for f in fs]),
                "prefix_ms": med([f["jobs"][0]["wrapper_ms"] for f in fs]),
                "tail_heads_ms": med([f["jobs"][1]["wrapper_ms"] for f in fs]),
                "total_dsp_ms": med([sum(j["wrapper_ms"] for j in f["jobs"]) for f in fs]),
            }
        result["paired_host_saving_ms"] = med(
            [
                a["host_wall_ms"] - b["host_wall_ms"]
                for c in crops.values()
                for a, b in zip(
                    c["profile"]["full_frames"]["original"],
                    c["profile"]["full_frames"]["vector_uninstrumented"],
                    strict=True,
                )
            ]
        )
    coarse = [f["log"] for c in crops.values() for f in c["profile"]["frames"]["coarse"]]
    for name in P.GLOBALS.values():
        result["globals"][name + "_ms"] = med([v["globals"][name] / 19200 for v in coarse])
    result["globals"]["maxpool_ms"] = med(
        [sum(b["maxpool"] for b in v["bands"].values()) / 19200 for v in coarse]
    )
    result["globals"]["decimation_ms"] = med(
        [sum(b["decimation"] for b in v["bands"].values()) / 19200 for v in coarse]
    )
    result["calibration_empty_ticks"] = med(
        [v["calibration"]["sum"] / v["calibration"]["n"] for v in coarse]
    )
    per_crop = []
    for c in crops.values():
        sample = {name: {key: 0.0 for key in P.FINE_FIELDS} for name in S.NAMES}
        full = {name: {key: 0.0 for key in P.FINE_FIELDS} for name in S.NAMES}
        sampled_operator = dict.fromkeys(S.NAMES, 0.0)
        full_operator = dict.fromkeys(S.NAMES, 0.0)
        sampled_blocks = {name: dict.fromkeys(("core", "halo", "mixed"), 0.0) for name in S.NAMES}
        full_blocks = {name: dict.fromkeys(("core", "halo", "mixed"), 0.0) for name in S.NAMES}
        for detail in c["profile"]["detailed"]:
            band = str(detail["band"])
            for name in S.NAMES:
                rows = [f["log"]["bands"][band]["stages"][name] for f in detail["phases"]]
                entire = detail["full"]["log"]["bands"][band]["stages"][name]
                sampled_operator[name] += med([row["operator"] for row in rows]) / 19200
                full_operator[name] += entire["operator"] / 19200
                for key in P.FINE_FIELDS:
                    sample[name][key] += (
                        med([row[key] for row in rows])
                        if key == "coefficient_splat"
                        else sum(row[key] for row in rows)
                    ) / 19200
                    full[name][key] += entire[key] / 19200
                for j in range(72):
                    sampled_blocks[name][P.block_class(j)] += (
                        sum(row["blocks"][j] for row in rows) / 19200
                    )
                    full_blocks[name][P.block_class(j)] += entire["blocks"][j] / 19200
        per_crop.append(
            (sample, full, sampled_operator, full_operator, sampled_blocks, full_blocks)
        )
    for name in S.NAMES:
        coarse_op = med(
            [
                sum(b["stages"][name]["operator"] for b in v["bands"].values()) / 19200
                for v in coarse
            ]
        )
        result["operators"][name] = {
            **{
                key + "_ms": med(
                    [
                        sum(b["stages"][name][key] for b in v["bands"].values()) / 19200
                        for v in coarse
                    ]
                )
                for key in P.FIELDS[:3]
            },
            "sampled_intervals_ms": {
                key: med([v[0][name][key] for v in per_crop]) for key in P.FINE_FIELDS
            },
            "fully_bracketed_intervals_ms": {
                key: med([v[1][name][key] for v in per_crop]) for key in P.FINE_FIELDS
            },
            "sampled_operator_ms": med([v[2][name] for v in per_crop]),
            "fully_bracketed_operator_ms": med([v[3][name] for v in per_crop]),
            "sampled_block_class_ms": {
                key: med([v[4][name][key] for v in per_crop]) for key in ("core", "halo", "mixed")
            },
            "fully_bracketed_block_class_ms": {
                key: med([v[5][name][key] for v in per_crop]) for key in ("core", "halo", "mixed")
            },
            # Different probes/schedules are deliberately not forced into an
            # additive partition of the uninstrumented operator's runtime.
            "coarse_minus_sampled_intervals_ms": coarse_op
            - med([sum(v[0][name].values()) for v in per_crop]),
        }
    result["interval_totals_ms"] = {
        "sampled": {
            key: med([sum(v[0][name][key] for name in S.NAMES) for v in per_crop])
            for key in P.FINE_FIELDS
        },
        "fully_bracketed": {
            key: med([sum(v[1][name][key] for name in S.NAMES) for v in per_crop])
            for key in P.FINE_FIELDS
        },
        "halo_blocks_sampled": med([sum(v[4][name]["halo"] for name in S.NAMES) for v in per_crop]),
        "halo_blocks_fully_bracketed": med(
            [sum(v[5][name]["halo"] for name in S.NAMES) for v in per_crop]
        ),
        "mixed_blocks_sampled": med(
            [sum(v[4][name]["mixed"] for name in S.NAMES) for v in per_crop]
        ),
    }
    return result
