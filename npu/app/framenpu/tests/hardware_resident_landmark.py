"""Opt-in resident v20a graph: all layers, repeated inputs, controls and teardown."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tools")]
from framenpu import Device, DeviceBusy, Job, JobFailed, RecoveryRequired
from framenpu.convolution import load_landmark_graph
from framenpu.landmark import OUTPUTS, ALIASES, specifications
import framenpu.resident_landmark as rl
from framenpu.resident_landmark import (
    ResidentLandmark,
    JOB_KINDS,
    ARENA_START,
    ARENA_END,
    SPAN,
    BATCH,
    _lease,
    load_linked,
)
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health
from hardware_landmark import digest, service
from hmx_multi_run import failure_snapshot
from landmark_reference import evaluate


def make_validator(graph, crop, folder, policy, layer_checks):
    import numpy as np

    def validator_factory(s, inputs):
        key = s["output"][0]
        reference = np.fromfile(
            folder / ("in0.f32" if key == "in0" else "blob_" + key + ".f32"),
            dtype="<f4",
        ).astype(np.float64)
        if s["operator"] == "Input":
            local = reference.copy()
        else:
            local = evaluate(graph, s, inputs).ravel().astype(np.float64)
        sigmoid = s.get("activation") == "sigmoid"
        condition = None
        roundoff_steps = 0
        if s["operator"] in ("ConvolutionDepthWise", "Pooling", "BinaryOp"):
            condition = evaluate(graph, s, inputs, condition=True).ravel().astype(np.float64)
            if s["operator"] == "ConvolutionDepthWise":
                roundoff_steps = 2 * math.prod(s["kernel"]) + 1
            elif s["operator"] == "Pooling":
                roundoff_steps = (
                    2 * math.prod(s["input_shape"][1:]) + 1 if s["params"].get(4, 0) else 0
                )
            else:
                roundoff_steps = 1
        logits = (
            evaluate(graph, s, inputs, logits=True).ravel().astype(np.float64) if sigmoid else None
        )
        seen = np.zeros(len(reference), dtype=bool)
        full_square = local_square = max_full = max_local = max_ratio = 0.0
        observed_logit = None
        if s["operator"] in ALIASES:
            for alias in s["output"]:
                if crop["files"][alias]["sha256"] != crop["files"][key]["sha256"]:
                    raise ValueError("ncnn alias contents differ")

        def check(indices, values, final, phase):
            nonlocal full_square, local_square, max_full, max_local, max_ratio, observed_logit
            indices = np.asarray(indices, dtype=np.int64)
            actual = np.asarray(values, dtype=np.float64)
            is_hmx = (
                s["operator"] in ("Convolution", "InnerProduct")
                and phase != "output_sigmoid"
                and not (sigmoid and phase == "output")
            )
            expected = logits[indices] if phase == "logits" else local[indices]
            if is_hmx:
                # Include bias cancellation in the FP16 store's ULP allowance.
                from framenpu.landmark import weights_and_bias

                _, bias = weights_and_bias(graph, s)
                tolerance = policy["local_hmx_atol"] + policy["local_hmx_rtol"] * (
                    np.abs(expected) + max(abs(v) for v in bias)
                )
            elif sigmoid:
                expected = np.full(len(actual), 1 / (1 + np.exp(-observed_logit)))
                tolerance = policy["local_sigmoid_atol"]
            else:
                tolerance = policy["local_hvx_atol"] + policy["local_hvx_rtol"] * np.abs(expected)
                if condition is not None:
                    tolerance += (
                        policy["local_hvx_roundoff_unit"] * roundoff_steps * condition[indices]
                    )
            error = np.abs(actual - expected)
            if not np.isfinite(actual).all() or np.any(error > tolerance):
                raise ValueError(
                    f"{s['name']} local {phase} mismatch: max {float(np.max(error))}, ratio {float(np.max(error / tolerance))}"
                )
            if phase == "logits":
                observed_logit = float(actual[0])
                return
            if np.any(seen[indices]) or len(np.unique(indices)) != len(indices):
                raise ValueError("duplicate layer output coordinate")
            seen[indices] = True
            full = np.abs(actual - reference[indices])
            limit = policy["layers"][s["name"]]
            if np.any(full > limit["max_abs"]):
                raise ValueError(
                    f"{s['name']} propagated max exceeded: {float(np.max(full))} > {limit['max_abs']}"
                )
            full_square += float(full @ full)
            local_square += float(error @ error)
            max_full = max(max_full, float(np.max(full)))
            max_local = max(max_local, float(np.max(error)))
            max_ratio = max(max_ratio, float(np.max(error / tolerance)))
            if final:
                rms = math.sqrt(full_square / len(reference))
                if not seen.all() or rms > limit["rms"]:
                    raise ValueError(
                        f"{s['name']} missing outputs or propagated RMS exceeded: {rms} > {limit['rms']}"
                    )
                row = {
                    "name": s["name"],
                    "operator": s["operator"],
                    "blobs": s["output"],
                    "values": len(reference),
                    "max_abs_vs_ncnn": max_full,
                    "rms_vs_ncnn": rms,
                    "max_local_error": max_local,
                    "rms_local_error": math.sqrt(local_square / len(reference)),
                    "max_local_ratio": max_ratio,
                    "limits": limit,
                }
                layer_checks.append(row)
                print(json.dumps({"crop": crop["name"], **row}), flush=True)

        return check

    return validator_factory


def metadata_controls(owner):
    """Rejected requests must leave every resident byte and sequence unchanged."""
    rows = []
    for name, index, change in (
        ("stale_sequence", 8, -1),
        ("nonce", 4, 1),
        ("identity", 10, 1),
        ("reserved", 23, 1),
        ("wrong_module", 3, 1),
        ("wrong_version", 1, 1),
    ):
        before = bytes(owner.device._transport.memory[ARENA_START:ARENA_END])
        seq = owner.sequence
        words = owner._descriptor(3)
        words[index] += change
        owner._command(3, descriptor=words, reject=True)
        if (
            owner.sequence != seq
            or bytes(owner.device._transport.memory[ARENA_START:ARENA_END]) != before
        ):
            raise ValueError("rejected request changed resident state")
        rows.append({"name": name, "unchanged": True})
    if owner.linked:
        # Batch requests outside a frame, or with an invalid budget/stop/unit
        # field, are refused by the linked image itself.
        for name, index, value in (
            ("batch_outside_frame", 19, 1),
            ("batch_zero_budget", 23, 0),
            ("batch_excess_budget", 23, 480001),
            ("batch_stop_layer", 24, 64),
            ("batch_stop_phase", 25, 4),
            ("batch_zero_units", 19, 0),
            ("batch_reserved", 26, 1),
        ):
            before = bytes(owner.device._transport.memory[ARENA_START:ARENA_END])
            seq = owner.sequence
            words = owner._descriptor(5, BATCH, units=1)
            words[index] = value
            owner._command(5, BATCH, descriptor=words, reject=True)
            if (
                owner.sequence != seq
                or bytes(owner.device._transport.memory[ARENA_START:ARENA_END]) != before
            ):
                raise ValueError("rejected batch changed resident state")
            rows.append({"name": name, "unchanged": True})
    try:
        second = _lease()
    except DeviceBusy:
        rows.append({"name": "exclusive_arena_lease", "refused": True})
    else:
        second.close()
        raise ValueError("second resident arena owner was allowed")
    return rows


def phase_profile(owner, chw):
    """Timing-only frame, one request per phase: native ticks by (layer, kind)."""
    owner._command(3)
    owner._command(4, offset=rl.ACTIVATIONS, data=rl._fp32(chw))
    rows = {}
    while owner.native[0] == 3:
        layer = owner.native[19]
        kind = rl._kind(owner.native, owner.records[layer])
        owner._command(
            5,
            rl.BATCH,
            units=rl.UNIT_LIMIT,
            stop=rl._phase_end(owner.native, owner.records, owner.fused),
        )
        done = owner.metrics["commands"][-1]
        row = rows.setdefault((layer, kind), [0, 0, 0.0])
        row[0] += 1
        row[1] += done["count"]
        row[2] += done["wrapper_ms"]
    heads = {k: hashlib.sha256(owner._read_tensor(k).tobytes()).hexdigest() for k in OUTPUTS}
    return {
        "heads": heads,
        "rows": [
            {"layer": layer, "kind": kind, "requests": v[0], "units": v[1], "wrapper_ms": v[2]}
            for (layer, kind), v in rows.items()
        ],
    }


def interrupted_tiles(owner, chw):
    """Timing-only frame whose small-map depthwise layers run in requests of
    three or five units, so requests end at every offset inside a tiled pass
    and the following request must recompute the rows that were waiting."""
    owner._command(3)
    owner._command(4, offset=rl.ACTIVATIONS, data=rl._fp32(chw))
    short = 0
    while owner.native[0] == 3:
        layer = owner.native[19]
        record = owner.records[layer]
        if record[0] == 3 and record[9] in (7, 14) and not record[23]:
            owner._command(5, rl.BATCH, units=5 if record[9] == 7 else 3)
            short += 1
        else:
            owner._drive((layer + 1, 0))
    return {
        "short_requests": short,
        "heads": {k: hashlib.sha256(owner._read_tensor(k).tobytes()).hexdigest() for k in OUTPUTS},
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    for name in ("jobs", "model", "corpus", "output"):
        p.add_argument("--" + name, required=True, type=Path)
    p.add_argument("--split", choices=("calibration", "heldout"), default="calibration")
    p.add_argument("--calibration-report", type=Path)
    p.add_argument("--crop")
    p.add_argument("--stop-after")
    p.add_argument("--load-only", action="store_true")
    p.add_argument("--cached-code", action="store_true")
    p.add_argument("--linked", action="store_true")
    p.add_argument("--budget-ms", type=float, default=5.0)
    p.add_argument("--hold-power", action="store_true")
    p.add_argument("--hold-idle-s", type=float, default=0.0)
    p.add_argument("--phase-profile", action="store_true")
    p.add_argument("--interrupt-tiles", action="store_true")
    p.add_argument("--unqualified-frames", type=int, default=0)
    p.add_argument("--controls", action="store_true")
    p.add_argument("--fault-at", nargs=2, metavar=("LAYER", "KIND"))
    args = p.parse_args()
    if not args.run_on_device or os.geteuid() != 0:
        p.error("--run-on-device and Frame root are required")
    import numpy as np

    graph = load_landmark_graph(args.model / "hand.ncnn.param", args.model / "hand.ncnn.bin")
    manifest = json.loads((args.corpus / "manifest.json").read_text())
    policy = json.loads((args.corpus / "tolerances.json").read_text())
    if args.linked:
        jobs = load_linked(args.jobs)
        identity = {
            "loader": hashlib.sha256(jobs.loader.code).hexdigest(),
            "linked": hashlib.sha256(jobs.image).hexdigest(),
        }
    else:
        jobs = {
            key: Job.load(args.jobs / ("resident_graph_" + key + ".fjob"))
            for key in JOB_KINDS.values()
        }
        identity = {key: hashlib.sha256(job.code).hexdigest() for key, job in jobs.items()}
    source_paths = [
        "src/framenpu/resident_landmark.py",
        "src/framenpu/convolution.py",
        "src/framenpu/landmark.py",
        "tests/hardware_resident_landmark.py",
        "tools/landmark_reference.py",
        "src/framenpu/sdk/entry.S",
        "tools/build_resident_landmark.py",
        "tests/resident_graph_entry.S",
        "tests/resident_graph.h",
        "tests/resident_graph_gather.h",
        "tests/resident_dw_masks.h",
        "tests/resident_dw_tile.h",
        "tests/resident_fp16.h",
        "tests/landmark_common.h",
        "tests/resident_graph_link.c",
        "tests/resident_link.ld",
    ] + ["tests/resident_graph_" + name + ".c" for name in JOB_KINDS.values()]
    sources = {name: digest(ROOT / name) for name in source_paths}
    manifest_sha = digest(args.corpus / "manifest.json")
    if (
        manifest["format"] != "framenpu-landmark-reference"
        or manifest["version"] != 1
        or policy["format"] != "framenpu-landmark-tolerances"
        or not policy["calibration_only"]
        or manifest["param_sha256"] != graph.param_sha256
        or manifest["weights_sha256"] != graph.weights_sha256
        or digest(args.corpus / "tolerances.json") != manifest["tolerances_sha256"]
        or manifest["tolerances_sha256"]
        != "381a033d6f4c10047998dfe42abb8cd6b18a756741c8a21690a3e6d6dc951f7b"
    ):
        p.error("reference/model/frozen tolerance pin differs")
    specs = specifications(graph)
    if set(policy["layers"]) != {s["name"] for s in specs}:
        p.error("policy does not cover every layer")
    for limits in policy["layers"].values():
        if set(limits) != {"max_abs", "rms"} or any(
            not math.isfinite(v) or v <= 0 for v in limits.values()
        ):
            p.error("invalid numerical limits")
    crops = [
        c
        for c in manifest["crops"]
        if c["split"] == args.split and (args.crop is None or c["name"] == args.crop)
    ]
    if not crops or len(crops) != (1 if args.crop else 2):
        p.error("incomplete crop selection")
    for crop in manifest["crops"]:
        if set(crop["files"]) != set(graph.shapes):
            p.error("crop/blob inventory differs")
        for key, entry in crop["files"].items():
            path = (
                args.corpus / crop["name"] / ("in0.f32" if key == "in0" else "blob_" + key + ".f32")
            )
            if (
                entry["bytes"] != math.prod(graph.shapes[key]) * 4
                or path.stat().st_size != entry["bytes"]
                or digest(path) != entry["sha256"]
                or not np.isfinite(np.fromfile(path, dtype="<f4")).all()
            ):
                p.error("reference tensor shape/hash/range differs")
    if args.split == "heldout":
        if not args.calibration_report:
            p.error("heldout requires completed calibration")
        prior = json.loads(args.calibration_report.read_text())
        if (
            not prior.get("qualification_complete")
            or prior.get("cached_code", False) != args.cached_code
            or prior.get("linked", False) != args.linked
            or prior.get("budget_ms", 5.0) != args.budget_ms
            or prior.get("hold_power", False) != args.hold_power
            or not prior.get("all_checks_passed")
            or prior["jobs_sha256"] != identity
            or prior["tolerances_sha256"] != manifest["tolerances_sha256"]
            or prior["source_sha256"] != sources
            or prior["manifest_sha256"] != manifest_sha
        ):
            p.error("calibration pin/completion differs")
    report = {
        "format": "framenpu-resident-landmark-validation",
        "cached_code": args.cached_code,
        "linked": args.linked,
        "budget_ms": args.budget_ms,
        "hold_power": args.hold_power,
        "jobs_sha256": identity,
        "model_sha256": graph.weights_sha256,
        "param_sha256": graph.param_sha256,
        "tolerances_sha256": manifest["tolerances_sha256"],
        "split": args.split,
        "source_sha256": sources,
        "manifest_sha256": manifest_sha,
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
        "crops": [],
        "service_before": service(),
    }
    failed = False
    with PowerSnapshot() as power:
        baseline = power.read()
        report["initial_health"] = settled_health(power, baseline)
        factory = audited_device(power, report["audits"])
        try:
            with Device() as observer:
                original = bytes(observer._transport.memory[ARENA_START:ARENA_END])
            report["arena_before_sha256"] = hashlib.sha256(original).hexdigest()
            owner = ResidentLandmark(
                graph,
                jobs,
                cached_code=args.cached_code,
                budget_ms=args.budget_ms,
                hold_power=args.hold_power,
                _device_factory=factory,
            )
            report["plan"] = {
                k: v for k, v in owner.plan.items() if k not in ("program", "packed_weights")
            }
            with owner:
                print(
                    json.dumps(
                        {
                            "stage": "loaded",
                            "weight_loads": owner.metrics["weight_loads"],
                            "open_ms": owner.metrics["open_ms"],
                        }
                    ),
                    flush=True,
                )
                held = power.read()
                if (held != baseline) != args.hold_power:
                    raise ValueError("HMX power between requests differs from the selected policy")
                report["held_power_sha256"] = (
                    hashlib.sha256(json.dumps(held, sort_keys=True).encode()).hexdigest()
                    if args.hold_power
                    else None
                )
                if args.controls:
                    report["controls"] = metadata_controls(owner)
                if not args.load_only:
                    for crop in crops:
                        layer_checks = []
                        folder = args.corpus / crop["name"]
                        validator_factory = make_validator(
                            graph, crop, folder, policy, layer_checks
                        )
                        fault = (args.fault_at[0], int(args.fault_at[1])) if args.fault_at else None
                        try:
                            outputs = owner.run(
                                np.fromfile(folder / "in0.f32", dtype="<f4"),
                                validator_factory=validator_factory,
                                stop_after=args.stop_after,
                                _fault_at=fault,
                            )
                        except JobFailed:
                            if (
                                fault is None
                                or not owner.invalid
                                or owner.native[0] != 4
                                or owner.device._transport.poisoned
                            ):
                                raise
                            report["confirmed_fault"] = {
                                "at": fault,
                                "native": owner.native,
                                "invalidated": True,
                            }
                            try:
                                owner.run(np.fromfile(folder / "in0.f32", dtype="<f4"))
                            except RuntimeError:
                                report["confirmed_fault"]["reuse_refused"] = True
                            else:
                                raise ValueError("faulted session allowed reuse")
                            break
                        if len(layer_checks) != len(owner.metrics["runs"][-1]["layers"]):
                            raise ValueError("layer validation coverage differs")
                        for key, values in outputs.items():
                            (
                                args.output.parent
                                / (args.output.stem + "_" + crop["name"] + "_" + key + ".f32")
                            ).write_bytes(values.tobytes())
                        report["crops"].append(
                            {
                                "name": crop["name"],
                                "checks": layer_checks,
                                "execution": owner.metrics["runs"][-1],
                                "outputs": {
                                    k: {
                                        "values": len(v),
                                        "sha256": hashlib.sha256(v.tobytes()).hexdigest(),
                                    }
                                    for k, v in outputs.items()
                                },
                            }
                        )
                        args.output.write_text(json.dumps(report, indent=2) + "\n")
                    # Timing-only repeats without intermediate readback. Heads must
                    # reproduce the qualified frame of the same crop bit for bit.
                    if args.hold_idle_s and not args.fault_at:
                        # Idle with the gate released; held power must not move.
                        deadline = time.monotonic() + args.hold_idle_s
                        samples = 0
                        while time.monotonic() < deadline:
                            if power.read() != held:
                                raise RecoveryRequired("HMX power changed while idle")
                            samples += 1
                            time.sleep(0.05)
                        report["hold_idle"] = {"seconds": args.hold_idle_s, "samples": samples}
                    for index in range(args.unqualified_frames if not args.fault_at else 0):
                        crop = crops[index % len(crops)]
                        outputs = owner.run(
                            np.fromfile(args.corpus / crop["name"] / "in0.f32", dtype="<f4")
                        )
                        hashes = {
                            k: hashlib.sha256(v.tobytes()).hexdigest() for k, v in outputs.items()
                        }
                        expected = {
                            k: v["sha256"]
                            for k, v in report["crops"][index % len(crops)]["outputs"].items()
                        }
                        if hashes != expected:
                            raise ValueError("unqualified frame heads differ from qualified frame")
                        report.setdefault("unqualified_frames", []).append(
                            {"name": crop["name"], "execution": owner.metrics["runs"][-1]}
                        )
                if args.phase_profile and args.linked and report["crops"] and not args.fault_at:
                    report["phase_profile"] = phase_profile(
                        owner, np.fromfile(args.corpus / crops[0]["name"] / "in0.f32", dtype="<f4")
                    )
                    if report["phase_profile"].pop("heads") != {
                        k: v["sha256"] for k, v in report["crops"][0]["outputs"].items()
                    }:
                        raise ValueError("phase-profile frame heads differ from qualified frame")
                if args.interrupt_tiles and args.linked and report["crops"] and not args.fault_at:
                    report["interrupted_tiles"] = interrupted_tiles(
                        owner, np.fromfile(args.corpus / crops[0]["name"] / "in0.f32", dtype="<f4")
                    )
                    if report["interrupted_tiles"].pop("heads") != {
                        k: v["sha256"] for k, v in report["crops"][0]["outputs"].items()
                    }:
                        raise ValueError("interrupted-tile frame heads differ from qualified frame")
                if power.read() != held:
                    raise RecoveryRequired("HMX power changed during the session")
            report["metrics"] = owner.metrics
            with Device() as observer:
                restored = bytes(observer._transport.memory[ARENA_START:ARENA_END])
            report["arena_after_sha256"] = hashlib.sha256(restored).hexdigest()
            if restored != original or len(restored) != SPAN:
                raise ValueError("independent full resident arena restoration differs")
            report["final_health"] = settled_health(power, baseline)
            report["service_after"] = service()
            if report["service_after"] != report["service_before"]:
                raise ValueError("DSP service changed")
            report["all_checks_passed"] = True
            report["qualification_complete"] = (
                not args.crop
                and not args.stop_after
                and not args.load_only
                and not args.fault_at
                and len(report["crops"]) == 2
                and all(
                    c["execution"]["complete"] and set(c["outputs"]) == set(OUTPUTS)
                    for c in report["crops"]
                )
            )
        except BaseException as exc:
            failed = True
            report["error"] = f"{type(exc).__name__}: {exc}"
            report["causes"] = []
            cause = exc.__cause__
            while cause:
                report["causes"].append(f"{type(cause).__name__}: {cause}")
                cause = cause.__cause__
            report["after_failure"] = failure_snapshot(power)
            report["interpretation"] = "stopped without recovery or restart"
            if "owner" in locals():
                report["failed_metrics"] = owner.metrics
                report["failed_native"] = owner.native
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            {
                "passed": report.get("all_checks_passed", False),
                "qualified": report.get("qualification_complete", False),
                "audits": report["audits"],
                "error": report.get("error"),
                "causes": report.get("causes"),
            }
        ),
        flush=True,
    )
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
