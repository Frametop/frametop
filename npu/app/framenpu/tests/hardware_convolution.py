"""Opt-in real-weight convolution validation against a frozen ncnn crop corpus."""

import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import sys
import os
import subprocess

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Job, JobFailed
from framenpu.convolution import load_landmark_graph, ResidentConvolution
from framenpu.errors import FrameNPUError
from framenpu.provenance import verify_kernel
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health
from hmx_baseline_run import percentiles
from hmx_multi_run import failure_snapshot


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def floats(path):
    data = array("f")
    data.frombytes(path.read_bytes())
    if sys.byteorder != "little":
        data.byteswap()
    return data


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--split", choices=("calibration", "heldout"), default="calibration")
    parser.add_argument("--layers", nargs="+", default=["convclip_0", "conv_10", "conv_12"])
    parser.add_argument("--tiles", type=int)
    parser.add_argument("--fault-control", action="store_true")
    parser.add_argument("--calibration-report", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    if not args.run_on_device:
        parser.error("--run-on-device is required")
    if os.geteuid() != 0:
        parser.error("run with sudo for HMX and read-only power snapshots")
    if args.tiles is not None and args.tiles <= 0:
        parser.error("tiles must be positive")
    if len(set(args.layers)) != len(args.layers) or not set(args.layers) <= {
        "convclip_0",
        "conv_10",
        "conv_12",
    }:
        parser.error("unsupported/duplicate target layers")
    graph = load_landmark_graph(args.model / "hand.ncnn.param", args.model / "hand.ncnn.bin")
    manifest = json.loads((args.corpus / "manifest.json").read_text())
    tolerance_path = args.corpus / "tolerances.json"
    tolerances = json.loads(tolerance_path.read_text())
    if (graph.param_sha256, graph.weights_sha256) != (
        manifest["param_sha256"],
        manifest["weights_sha256"],
    ) or tolerances["corpus_manifest_sha256"] != digest(args.corpus / "manifest.json"):
        parser.error("corpus/model/tolerance identity differs")
    crops = [c for c in manifest["crops"] if c["split"] == args.split]
    if not crops:
        parser.error("corpus has no selected crops")
    for crop in crops:
        for name, meta in crop["files"].items():
            if digest(args.corpus / crop["name"] / name) != meta["sha256"]:
                parser.error("reference tensor hash differs")
    probe = Job.load(args.probe)
    if args.split == "heldout":
        if args.calibration_report is None:
            parser.error("heldout requires the completed --calibration-report")
        calibration = json.loads(args.calibration_report.read_text())
        if (
            not calibration.get("qualification_complete")
            or calibration["tolerances_sha256"] != digest(tolerance_path)
            or calibration["probe_sha256"] != hashlib.sha256(probe.code).hexdigest()
        ):
            parser.error("calibration is incomplete or identities changed")
    report = {
        "runtime": verify_kernel(),
        "probe_bytes": len(probe.code),
        "probe_sha256": hashlib.sha256(probe.code).hexdigest(),
        "manifest_sha256": digest(args.corpus / "manifest.json"),
        "tolerances_sha256": digest(tolerance_path),
        "split": args.split,
        "cases": [],
        "sessions": [],
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
    }

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

    report["service_before"] = service()
    failed = False
    with PowerSnapshot() as power:
        baseline = power.read()
        report["initial_health"] = settled_health(power, baseline)
        factory = audited_device(power, report["audits"])
        try:
            for name in args.layers:
                layer = graph.convolution(name)
                tolerance = tolerances["layers"][name]
                owner = ResidentConvolution(probe, graph, name, _device_factory=factory)
                with owner:
                    for crop in crops:
                        folder = args.corpus / crop["name"]
                        input_path = folder / (
                            "in0.f32" if layer.bottom == "in0" else "blob_" + layer.bottom + ".f32"
                        )
                        original = floats(folder / ("blob_" + layer.top + ".f32"))
                        quantized = floats(folder / (name + "_qref.f32"))
                        n, h, w = layer.output_shape
                        count = 0
                        square_full = 0.0
                        square_quant = 0.0
                        square_input = 0.0
                        max_full = 0.0
                        max_quant = 0.0
                        max_input = 0.0
                        max_ratio = 0.0
                        worst = None
                        zeros = 0
                        sixes = 0

                        def validate(start, rows, values):
                            nonlocal \
                                count, \
                                square_full, \
                                square_quant, \
                                square_input, \
                                max_full, \
                                max_quant, \
                                max_input, \
                                max_ratio, \
                                worst, \
                                zeros, \
                                sixes
                            for r in range(rows):
                                for c in range(n):
                                    offset = c * h * w + start + r
                                    q = float(quantized[offset])
                                    ref = float(original[offset])
                                    value = values[r * 32 + c]
                                    error_q = abs(value - q)
                                    error_full = abs(value - ref)
                                    input_error = abs(q - ref)
                                    budget = tolerance["atol"] + tolerance["rtol"] * abs(q)
                                    ratio = error_q / budget
                                    if (
                                        not math.isfinite(value)
                                        or ratio > 1
                                        or error_full > input_error + budget + 1e-10
                                        or error_full > tolerance["full_max_abs"]
                                    ):
                                        raise ValueError(
                                            f"{name} numerical tolerance exceeded at c={c}, pixel={start + r}: ratio={ratio}"
                                        )
                                    if error_full >= max_full:
                                        worst = {
                                            "channel": c,
                                            "pixel": start + r,
                                            "actual": value,
                                            "full_reference": ref,
                                            "quantized_reference": q,
                                        }
                                    count += 1
                                    square_full += error_full**2
                                    square_quant += error_q**2
                                    square_input += input_error**2
                                    max_full = max(max_full, error_full)
                                    max_quant = max(max_quant, error_q)
                                    max_input = max(max_input, input_error)
                                    max_ratio = max(max_ratio, ratio)
                                    zeros += int(value == 0)
                                    sixes += int(value == 6)
                            if (
                                start + rows == h * w
                                and math.sqrt(square_full / count) > tolerance["full_rms"]
                            ):
                                raise ValueError(f"{name} full-reference RMS tolerance exceeded")

                        first = len(owner.gemm.metrics["commands"])
                        actual = owner.run(
                            floats(input_path), tile_validator=validate, stop_after=args.tiles
                        )
                        run = owner.runs[-1]
                        commands = [
                            c for c in owner.gemm.metrics["commands"][first:] if c["op"] == 2
                        ]
                        encoded = [c["host_input_encoding_ms"] for c in commands]
                        case = {
                            "layer": name,
                            "crop": crop["name"],
                            "split": args.split,
                            "values": count,
                            "input_source": "pinned ncnn FP32 intermediate",
                            "run": run,
                            "max_abs_vs_full": max_full,
                            "rms_vs_full": math.sqrt(square_full / count),
                            "max_abs_vs_quantized_input_ncnn": max_quant,
                            "rms_vs_quantized_input_ncnn": math.sqrt(square_quant / count),
                            "max_abs_input_quantization": max_input,
                            "rms_input_quantization": math.sqrt(square_input / count),
                            "max_tolerance_ratio": max_ratio,
                            "full_max_abs_limit": tolerance["full_max_abs"],
                            "full_rms_limit": tolerance["full_rms"],
                            "output_zeros": zeros,
                            "output_sixes": sixes,
                            "worst_full_reference_coordinate": worst,
                            "output_sha256": hashlib.sha256(actual.tobytes()).hexdigest(),
                            "command_ms": percentiles([c["command_ms"] for c in commands]),
                            "host_input_encoding_ms": percentiles(encoded),
                            "host_phase_ms": {
                                key: percentiles([c["host_phase_ms"][key] for c in commands])
                                for key in commands[0]["host_phase_ms"]
                            },
                            "dsp_phase_us": {
                                key: percentiles([c["dsp"][key] / 19.2 for c in commands])
                                for key in commands[0]["dsp"]
                                if commands[0]["dsp"][key] is not None
                            },
                            "max_wrapper_ms": max(
                                c["dsp"]["wrapper_ticks"] / 19200 for c in commands
                            ),
                            "max_publish_to_completion_ms": max(
                                c["publish_to_completion_ms"] for c in commands
                            ),
                        }
                        report["cases"].append(case)
                        print(
                            json.dumps(
                                {
                                    "layer": name,
                                    "crop": crop["name"],
                                    "values": count,
                                    "max_full": max_full,
                                    "max_quantized": max_quant,
                                    "ratio": max_ratio,
                                }
                            ),
                            flush=True,
                        )
                    if args.fault_control:
                        patches = layer.patches(floats(input_path), 0)
                        patches = [p + [0.0] * (owner.gemm.k - len(p)) for p in patches]
                        patches += [[0.0] * owner.gemm.k for _ in range(32 - len(patches))]
                        try:
                            owner.gemm.run(patches, _fault=True)
                        except JobFailed:
                            owner.gemm.metrics["convolution_fault_checked"] = True
                        else:
                            raise ValueError("convolution guarded fault did not fail")
                if owner.gemm.metrics["weight_uploads"] != 1:
                    raise ValueError("convolution weights uploaded repeatedly")
                report["sessions"].append({"layer": name, **owner.gemm.metrics})
                settled_health(power, baseline)
            report["final_health"] = settled_health(power, baseline)
            report["service_after"] = service()
            if report["service_after"] != report["service_before"]:
                raise ValueError("DSP service changed")
            report["all_checks_passed"] = True
            report["qualification_complete"] = (
                args.tiles is None and len(args.layers) == 3 and len(report["cases"]) == 6
            )
        except (FrameNPUError, ValueError, OSError) as exc:
            failed = True
            report["error"] = f"{type(exc).__name__}: {exc}"
            report["error_causes"] = []
            cause = exc.__cause__
            while cause is not None:
                report["error_causes"].append(f"{type(cause).__name__}: {cause}")
                cause = cause.__cause__
            report["after_failure"] = failure_snapshot(power)
            report["interpretation"] = "stopped without recovery or restart"
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            {
                "report": str(args.output),
                "passed": report.get("all_checks_passed", False),
                "qualified": report.get("qualification_complete", False),
                "audits": report["audits"],
                "error": report.get("error"),
                "causes": report.get("error_causes"),
            }
        )
    )
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
