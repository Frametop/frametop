"""Opt-in full pinned graph, every blob, four outputs, controls and teardown."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tools")]
from framenpu import Job, JobFailed, RecoveryRequired
from framenpu.convolution import load_landmark_graph
from framenpu.landmark import StreamingLandmark, OUTPUTS, ALIASES, SSR, specifications
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health
from hmx_multi_run import failure_snapshot
from landmark_reference import evaluate


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def service():
    return subprocess.check_output(
        [
            "systemctl",
            "show",
            "dsp_service",
            "-p",
            "MainPID",
            "-p",
            "NRestarts",
            "-p",
            "ActiveState",
        ],
        text=True,
    )


def controls(jobs, factory):
    """Distinct signed operands, edge values, native bounds rejection and traps."""
    import numpy as np

    cases = []
    k = 704
    a = np.asarray(
        [[((t * 37 + r * 13) % 251 - 125) / 128 for t in range(k)] for r in range(32)],
        dtype=np.float16,
    )
    b = np.asarray(
        [[((t * 17 + c * 7) % 257 - 128) / 128 for c in range(32)] for t in range(k)],
        dtype=np.float16,
    )
    if (
        len({a[:, t : t + 32].tobytes() for t in range(0, k, 32)}) != 22
        or len({b[t : t + 32].tobytes() for t in range(0, k, 32)}) != 22
    ):
        raise ValueError("K704 controls reuse an operand block")
    data = (
        a.reshape(32, 22, 32).transpose(1, 0, 2).tobytes()
        + b.reshape(22, 32, 32).tobytes()
        + bytes(128)
    )
    expected = (
        (a.astype(np.float64) @ b.astype(np.float64)).astype(np.float16).astype(np.float32).ravel()
    )
    cases.append(("hmx", 20, (22,), data, expected))
    # Arbitrary FP32 biases expose one-ULP qf32 conversion of 0+bias. All
    # padded rows must preserve the exact bias identity before activation.
    bias = np.asarray([(i - 17) / 13 for i in range(32)], dtype=np.float32)
    cases.append(("hmx", 20, (1,), bytes(4096) + bias.tobytes(), np.tile(bias, 32)))
    x = np.arange(64, dtype=np.float32).reshape(2, 32) / 16 - 2
    w = np.full((2, 32), 0.25, dtype=np.float32)
    bias = np.linspace(-1, 1, 32, dtype=np.float32)
    expected = (bias + x[0] * w[0] + x[1] * w[1]).astype(np.float32)
    cases.append(("hvx", 21, (1, 2), x.tobytes() + w.tobytes() + bias.tobytes(), expected))
    cases.append(("hvx", 22, (1,), x.tobytes(), (x[0] + x[1]).astype(np.float32)))
    cases.append(
        (
            "hvx",
            23,
            (1, 2),
            (-np.abs(x) - 1).tobytes(),
            np.maximum(-np.abs(x[0]) - 1, -np.abs(x[1]) - 1),
        )
    )
    cases.append(
        (
            "hvx",
            24,
            (1, 2),
            x.tobytes() + np.full(32, 0.5, dtype=np.float32).tobytes(),
            ((x[0] + x[1]) * 0.5).astype(np.float32),
        )
    )
    sigmoid = np.asarray(
        [-100, -80, -20, -2, -0.01, 0, 0.01, 2, 20, 80, 100] + [0] * 21, dtype=np.float32
    )
    expected = (1 / (1 + np.exp(-sigmoid.astype(np.float64)))).astype(np.float32)
    cases.append(("sigmoid", 25, (1,), sigmoid.tobytes(), expected))
    report = []
    with factory(dispatch="direct") as device:
        for kind, opcode, params, data, expected in cases:
            vectors = len(expected) // 32
            for mode in ("valid", "reject", "fault"):
                args = list(params) + [0] * (6 - len(params))
                if mode == "reject":
                    args[3] = 1
                if mode == "fault":
                    args[5] = 1

                def validate(result):
                    try:
                        for i, row in enumerate(result.workers):
                            error = (
                                (1 if mode == "reject" else 2 if mode == "fault" else 0)
                                if i == 0
                                else 0
                            )
                            if (
                                not row[0]
                                or row[0] != result.workers[0][0]
                                or row[1] != i
                                or row[2] != error
                                or row[7] != SSR[i]
                            ):
                                raise ValueError("control worker identity differs")
                        row = result.workers[0]
                        if mode == "fault":
                            if (
                                row[3] & 255 != 37
                                or not 0xF0A92000 <= row[4] < 0xF0A92000 + len(jobs[kind].code)
                                or result.output[:128] != struct.pack("<32I", *([0xDECAFBAD] * 32))
                            ):
                                raise ValueError("guarded trap/unwind sentinel differs")
                        elif any(row[3:6]):
                            raise ValueError("unexpected control fault")
                        if (
                            row[8] != 0x87000000
                            or any(row[14:])
                            or result.kernel_reference_ticks >= 768000
                        ):
                            raise ValueError("control EVB/timing differs")
                        if mode == "valid":
                            actual = np.frombuffer(result.output[:-128], dtype="<f4")
                            equal = (
                                actual.tobytes() == expected.tobytes()
                                if opcode == 20
                                else np.allclose(actual, expected, atol=2e-6, rtol=1e-6)
                            )
                            if opcode == 25:
                                zero_inputs = np.frombuffer(data, dtype="<f4") == 0
                                equal = equal and np.all(actual[zero_inputs] == 0.5)
                            if any(result.output[-128:]) or not equal:
                                raise ValueError("control arithmetic differs")
                    except BaseException as exc:
                        raise RecoveryRequired("landmark control failed") from exc

                try:
                    device._submit(
                        jobs[kind],
                        data,
                        opcode=opcode,
                        params=args,
                        output_size=(vectors + 1) * 128,
                        use_direct=True,
                        _completion_validator=validate,
                    )
                    if mode != "valid":
                        raise ValueError("control unexpectedly succeeded")
                except JobFailed:
                    if mode == "valid":
                        raise
                report.append({"kernel": kind, "opcode": opcode, "mode": mode, "passed": True})
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    for name in ("jobs", "model", "corpus", "output"):
        p.add_argument("--" + name, required=True, type=Path)
    p.add_argument("--split", choices=("calibration", "heldout"), default="calibration")
    p.add_argument("--calibration-report", type=Path)
    p.add_argument("--crop")
    p.add_argument("--stop-after")
    p.add_argument("--controls", action="store_true")
    args = p.parse_args()
    if not args.run_on_device:
        p.error("--run-on-device is required")
    if os.geteuid() != 0:
        p.error("run on Frame as root")
    import numpy as np

    graph = load_landmark_graph(args.model / "hand.ncnn.param", args.model / "hand.ncnn.bin")
    manifest = json.loads((args.corpus / "manifest.json").read_text())
    policy = json.loads((args.corpus / "tolerances.json").read_text())
    jobs = {
        key: Job.load(args.jobs / ("landmark_" + key + ".fjob"))
        for key in ("hmx", "hvx", "sigmoid")
    }
    identity = {key: hashlib.sha256(job.code).hexdigest() for key, job in jobs.items()}
    sources = {
        name: digest(ROOT / name)
        for name in (
            "src/framenpu/landmark.py",
            "src/framenpu/convolution.py",
            "tests/hardware_landmark.py",
            "tests/landmark_hmx.c",
            "tests/landmark_hvx.c",
            "tests/landmark_sigmoid.c",
            "tests/landmark_common.h",
            "tools/landmark_reference.py",
            "src/framenpu/sdk/entry.S",
        )
    }
    manifest_sha = digest(args.corpus / "manifest.json")
    if (
        manifest["format"] != "framenpu-landmark-reference"
        or manifest["version"] != 1
        or policy["format"] != "framenpu-landmark-tolerances"
        or not policy["calibration_only"]
    ):
        p.error("reference/policy schema differs")
    if (
        manifest["param_sha256"] != graph.param_sha256
        or manifest["weights_sha256"] != graph.weights_sha256
        or digest(args.corpus / "tolerances.json") != manifest["tolerances_sha256"]
    ):
        p.error("model/tolerance pin differs")
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
        if crop["name"] not in {
            "calibration_controller",
            "calibration_open",
            "heldout_left_controller",
            "heldout_right_controller",
        } or set(crop["files"]) != set(graph.shapes):
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
            or not prior.get("all_checks_passed")
            or prior["jobs_sha256"] != identity
            or prior["tolerances_sha256"] != manifest["tolerances_sha256"]
            or prior["source_sha256"] != sources
            or prior["manifest_sha256"] != manifest_sha
        ):
            p.error("calibration pin/completion differs")
    report = {
        "format": "framenpu-landmark-validation",
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
            if args.controls:
                report["controls"] = controls(jobs, factory)
                settled_health(power, baseline)
            for crop in crops:
                layer_checks = []
                folder = args.corpus / crop["name"]

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
                        condition = (
                            evaluate(graph, s, inputs, condition=True).ravel().astype(np.float64)
                        )
                        if s["operator"] == "ConvolutionDepthWise":
                            roundoff_steps = 2 * math.prod(s["kernel"]) + 1
                        elif s["operator"] == "Pooling":
                            roundoff_steps = (
                                2 * math.prod(s["input_shape"][1:]) + 1
                                if s["params"].get(4, 0)
                                else 0
                            )
                        else:
                            roundoff_steps = 1
                    logits = (
                        evaluate(graph, s, inputs, logits=True).ravel().astype(np.float64)
                        if sigmoid
                        else None
                    )
                    seen = np.zeros(len(reference), dtype=bool)
                    full_square = local_square = max_full = max_local = max_ratio = 0.0
                    observed_logit = None
                    if s["operator"] in ALIASES:
                        for alias in s["output"]:
                            if crop["files"][alias]["sha256"] != crop["files"][key]["sha256"]:
                                raise ValueError("ncnn alias contents differ")

                    def check(indices, values, final, phase):
                        nonlocal \
                            full_square, \
                            local_square, \
                            max_full, \
                            max_local, \
                            max_ratio, \
                            observed_logit
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
                            tolerance = policy["local_hvx_atol"] + policy[
                                "local_hvx_rtol"
                            ] * np.abs(expected)
                            if condition is not None:
                                tolerance += (
                                    policy["local_hvx_roundoff_unit"]
                                    * roundoff_steps
                                    * condition[indices]
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

                owner = StreamingLandmark(graph, jobs, _device_factory=factory)
                with owner:
                    outputs = owner.run(
                        np.fromfile(folder / "in0.f32", dtype="<f4"),
                        validator_factory=validator_factory,
                        stop_after=args.stop_after,
                    )
                if len(layer_checks) != len(owner.metrics[-1]["layers"]):
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
                        "execution": owner.metrics[-1],
                        "outputs": {
                            k: {"values": len(v), "sha256": hashlib.sha256(v.tobytes()).hexdigest()}
                            for k, v in outputs.items()
                        },
                    }
                )
                settled_health(power, baseline)
                args.output.write_text(json.dumps(report, indent=2) + "\n")
            report["final_health"] = settled_health(power, baseline)
            report["service_after"] = service()
            if report["service_after"] != report["service_before"]:
                raise ValueError("DSP service changed")
            report["all_checks_passed"] = True
            report["qualification_complete"] = (
                args.crop is None
                and args.stop_after is None
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
            if "owner" in locals() and owner.metrics:
                report["failed_execution"] = owner.metrics[-1]
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
    sys.exit(main())
