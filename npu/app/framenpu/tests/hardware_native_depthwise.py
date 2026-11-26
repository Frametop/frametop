"""Real-weight native depthwise arithmetic pilots, outside the resident graph.

Inputs are pinned ncnn layer inputs. Padding/FP16 storage preparation is timed
on the host; native staging, gather+arithmetic and stores are timed by row.
This compares isolated kernels, not graph/tracking qualification. Numerical
checks follow audited restoration. No retries or recovery on failures.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tools")]
from framenpu import Device, Job, JobFailed
from framenpu.convolution import load_landmark_graph
from framenpu.landmark import specifications, weights_and_bias
from framenpu.resident_gemm import _lease, RECORD
from framenpu.resident_landmark import ARENA_START, ARENA_END, CONTROL
from hardware_ieee_probe import completion
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health
from hardware_landmark import service, digest
from hmx_multi_run import failure_snapshot
from landmark_reference import evaluate

LAYERS = ("convdwclip_0", "convdwclip_1", "convdwclip_8", "convdwclip_12")


def prepare(spec, source, weights, bias, mode):
    import numpy as np

    start = time.monotonic_ns()
    channels, h, w = spec["input_shape"]
    _, oh, ow = spec["output_shape"]
    k, kw = spec["kernel"]
    stride, sw = spec["stride"]
    pt, pb, pl, pr = spec["padding"]
    if (
        k != kw
        or stride != sw
        or pt != pl
        or pb != pr
        or spec["dilation"] != (1, 1)
        and spec["dilation"] != [1, 1]
    ):
        raise ValueError("unsupported pilot geometry")
    elem = 2 if mode == 2 else 4
    srcrow = ((w + pl + pr + 128 // elem - 1) // (128 // elem)) * 128
    srcchan = (h + pt + pb) * srcrow
    dstrow = ((ow + 63) // 64) * 256
    dstchan = oh * dstrow
    chunk = min(8, (131072 - 384) // (128 + srcchan + dstchan))
    if not chunk:
        raise ValueError("pilot channel does not fit borrowed arena")
    packed = np.zeros((channels, h + pt + pb, srcrow // elem), dtype="<f2" if mode == 2 else "<f4")
    packed[:, pt : pt + h, pl : pl + w] = source
    batches = []
    for first in range(0, channels, chunk):
        count = min(chunk, channels - first)
        header = [
            0x4E445031,
            count,
            h,
            w,
            oh,
            ow,
            k,
            stride,
            pt,
            int(spec["activation"] == "clip"),
            srcrow,
            srcchan,
            dstrow,
            dstchan,
            pb,
        ] + [0] * 17
        coef = b"".join(
            (struct.pack("<f", float(bias[c])) + weights[c].astype("<f2").tobytes()).ljust(
                128, b"\0"
            )
            for c in range(first, first + count)
        )
        data = struct.pack("<32I", *header) + coef + packed[first : first + count].tobytes()
        outbytes = 256 + count * dstchan
        if len(data) + outbytes > 131072:
            raise ValueError("pilot arena overflow")
        batches.append(
            {
                "first": first,
                "count": count,
                "data": data,
                "output_bytes": outbytes,
                "row_bytes": dstrow,
                "channel_bytes": dstchan,
            }
        )
    return batches, (time.monotonic_ns() - start) / 1e6


def unpack(result, batch, spec, mode):
    import numpy as np

    _, oh, ow = spec["output_shape"]
    width = batch["row_bytes"] // 4
    data = np.frombuffer(result.output, dtype="<f4", offset=256).reshape(batch["count"], oh, width)
    if mode == 2:
        # The supported half->QFloat32 multiply returns even/odd lane vectors.
        data = (
            data.reshape(batch["count"], oh, -1, 2, 32)
            .transpose(0, 1, 2, 4, 3)
            .reshape(batch["count"], oh, width)
        )
    return data[:, :, :ow].copy()


def error(actual, reference):
    import numpy as np

    diff = actual.astype(np.float64) - reference.astype(np.float64)
    if not np.isfinite(actual).all():
        raise ValueError("nonfinite depthwise result")
    return {
        "values": int(actual.size),
        "max_abs": float(np.max(np.abs(diff))),
        "rms": float(np.sqrt(np.mean(diff * diff))),
        "different_fp32_values": int(
            np.sum(actual.view(np.uint32) != reference.astype(np.float32).view(np.uint32))
        ),
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    for name in ("jobs", "model", "corpus", "baseline-report", "output"):
        p.add_argument("--" + name, required=True, type=Path)
    p.add_argument("--crop", default="calibration_controller")
    p.add_argument("--layers", nargs="+", choices=LAYERS, default=list(LAYERS))
    args = p.parse_args()
    if not args.run_on_device or os.geteuid() != 0:
        p.error("--run-on-device and Frame root required")
    import numpy as np

    graph = load_landmark_graph(args.model / "hand.ncnn.param", args.model / "hand.ncnn.bin")
    manifest = json.loads((args.corpus / "manifest.json").read_text())
    prior = json.loads(args.baseline_report.read_text())
    if (
        not prior.get("all_checks_passed")
        or not prior.get("qualification_complete")
        or prior["model_sha256"] != graph.weights_sha256
        or prior["param_sha256"] != graph.param_sha256
        or prior["manifest_sha256"] != digest(args.corpus / "manifest.json")
    ):
        p.error("model/corpus/qualified baseline differs")
    for name, sha in prior["source_sha256"].items():
        if digest(ROOT / name) != sha:
            p.error("qualified production source pin differs")
    crop = next(c for c in manifest["crops"] if c["name"] == args.crop)
    specs = {s["name"]: s for s in specifications(graph)}
    jobs, builds = {}, {}
    for name in args.layers:
        jobs[name], builds[name] = [], []
        for mode in range(3):
            folder = args.jobs / name / str(mode)
            build = json.loads((folder / "build.json").read_text())
            job = Job.load(folder / "probe.fjob")
            if (
                hashlib.sha256(job.code).hexdigest() != build["sha256"]
                or f"-DND_MODE={mode}" not in build["flags"]
                or f"-DND_CASE={LAYERS.index(name)}" not in build["flags"]
            ):
                p.error("probe identity/mode/geometry differs")
            for path, sha in build["sources"].items():
                if digest(ROOT / path) != sha:
                    p.error("probe source pin differs")
            jobs[name].append(job)
            builds[name].append(build)
    # This bound prevents overflow/saturation inside the unchecked QFloat
    # reduction. It is derived from all real operands before any submission.
    cases = []
    for name in args.layers:
        spec = specs[name]
        key = spec["input"][0]
        path = args.corpus / args.crop / ("blob_" + key + ".f32")
        if digest(path) != crop["files"][key]["sha256"]:
            p.error("recorded layer input hash differs")
        source = np.fromfile(path, dtype="<f4").reshape(spec["input_shape"])
        vals, bias = weights_and_bias(graph, spec)
        weights = np.asarray(vals, dtype="<f2").reshape(spec["output_shape"][0], -1)
        bias = np.asarray(bias, dtype="<f4")
        mag = weights.view(np.uint16) & 0x7FFF
        if (
            np.any((mag != 0) & ((mag & 0x7C00) == 0))
            or not np.isfinite(weights).all()
            or not np.isfinite(bias).all()
            or not np.isfinite(source).all()
            or np.max(np.abs(source)) > 65504
        ):
            p.error("finite/range/normal-coefficient preflight failed")
        bound = np.abs(bias.astype(np.float64)) + np.sum(
            np.abs(weights.astype(np.float64)), axis=1
        ) * np.max(np.abs(source), axis=(1, 2))
        if np.max(bound) >= 2**100:
            p.error("intermediate range bound differs")
        local = evaluate(graph, spec, [source])
        half_local = evaluate(graph, spec, [source.astype(np.float16).astype(np.float32)])
        outkey = spec["output"][0]
        outpath = args.corpus / args.crop / ("blob_" + outkey + ".f32")
        if digest(outpath) != crop["files"][outkey]["sha256"]:
            p.error("ncnn output pin differs")
        ncnn = np.fromfile(outpath, dtype="<f4").reshape(spec["output_shape"])
        cases.append((spec, source, weights, bias, bound, local, half_local, ncnn))
    report = {
        "format": "framenpu-native-depthwise-pilot",
        "crop": args.crop,
        "model_sha256": graph.weights_sha256,
        "manifest_sha256": digest(args.corpus / "manifest.json"),
        "builds": builds,
        "source_sha256": {
            name: digest(ROOT / name)
            for name in (
                "tests/hardware_native_depthwise.py",
                "tests/hardware_ieee_probe.py",
                "tools/landmark_reference.py",
            )
        },
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
        "layers": [],
        "service_before": service(),
    }
    failed = False
    lease = _lease()
    try:
        with PowerSnapshot() as power:
            baseline = power.read()
            report["initial_health"] = settled_health(power, baseline)
            try:
                with Device() as observer:
                    if bytes(observer._transport.memory[CONTROL : CONTROL + 8]) == RECORD:
                        raise ValueError("resident refusal record present")
                    before = bytes(observer._transport.memory[ARENA_START:ARENA_END])
                report["arena_before_sha256"] = hashlib.sha256(before).hexdigest()
                with audited_device(power, report["audits"])(dispatch="direct") as device:

                    def guard_control(job, batch, *, fault):
                        captured = []
                        data = batch["data"] if fault else bytes(4) + batch["data"][4:]

                        def validate_control(result):
                            completion(
                                result,
                                job,
                                fault=fault,
                                reject=not fault,
                                output_bytes=batch["output_bytes"],
                            )
                            captured.append(result)

                        try:
                            device._submit(
                                job,
                                data,
                                opcode=99,
                                params=(1, 0, 0, 0, 0, int(fault)),
                                output_size=batch["output_bytes"],
                                use_direct=True,
                                _completion_validator=validate_control,
                            )
                        except JobFailed:
                            if len(captured) != 1 or captured[0].workers[0][2] != (
                                2 if fault else 1
                            ):
                                raise
                        else:
                            raise ValueError("expected depthwise trap/rejection missing")

                    for spec, source, weights, bias, bound, local, half_local, ncnn in cases:
                        layer = {
                            "name": spec["name"],
                            "input_shape": spec["input_shape"],
                            "output_shape": spec["output_shape"],
                            "kernel": spec["kernel"],
                            "stride": spec["stride"],
                            "absolute_intermediate_bound": float(np.max(bound)),
                            "modes": [],
                        }
                        report["layers"].append(layer)
                        control = None
                        for mode, job in enumerate(jobs[spec["name"]]):
                            batches, prep = prepare(spec, source, weights, bias, mode)
                            actual = np.zeros(spec["output_shape"], dtype=np.float32)
                            samples = []
                            for batch in batches:
                                got = []

                                def validate(result):
                                    completion(result, job, output_bytes=batch["output_bytes"])
                                    got.append(result)

                                start = time.monotonic_ns()
                                result = device._submit(
                                    job,
                                    batch["data"],
                                    opcode=99,
                                    params=(1,),
                                    output_size=batch["output_bytes"],
                                    use_direct=True,
                                    _completion_validator=validate,
                                )
                                host_ms = (time.monotonic_ns() - start) / 1e6
                                meta = struct.unpack_from("<64I", result.output)

                                def ticks(at):
                                    return meta[at] | meta[at + 1] << 32

                                body = ticks(10) - ticks(8)
                                stage = ticks(12)
                                compute = ticks(14)
                                store = ticks(16)
                                if (
                                    meta[0] != 0x4E445031
                                    or not 0x87A92000 <= meta[1] < 0x87A92000 + len(job.code)
                                    or meta[2] != mode
                                    or meta[3] != batch["count"] * spec["output_shape"][1]
                                    or meta[4]
                                    or meta[5:8] != (batch["count"], *spec["output_shape"][1:])
                                    or any(meta[18:])
                                    or not 0
                                    <= stage + compute + store
                                    <= body
                                    <= result.kernel_reference_ticks
                                ):
                                    raise ValueError(
                                        "depthwise metadata/count/finite/budget mismatch after restoration"
                                    )
                                first = batch["first"]
                                actual[first : first + batch["count"]] = unpack(
                                    result, batch, spec, mode
                                )
                                samples.append(
                                    {
                                        "first_channel": first,
                                        "channels": batch["count"],
                                        "body_ms": body / 19200,
                                        "stage_ms": stage / 19200,
                                        "gather_arithmetic_ms": compute / 19200,
                                        "store_check_ms": store / 19200,
                                        "wrapper_ms": result.kernel_reference_ticks / 19200,
                                        "host_submit_ms": host_ms,
                                    }
                                )
                            if mode == 0:
                                control = actual.copy()
                            entry = {
                                "mode": mode,
                                "host_pack_pad_ms": prep,
                                "samples": samples,
                                "vs_local_fp32": error(actual, local),
                                "vs_precision_matched_local": error(
                                    actual, half_local if mode == 2 else local
                                ),
                                "vs_ncnn": error(actual, ncnn),
                                "vs_control": error(actual, control),
                                "output_sha256": hashlib.sha256(actual.tobytes()).hexdigest(),
                            }
                            # Arithmetic, input quantization and ncnn comparison
                            # remain distinct. Broad fixture bound is a gross-
                            # corruption check, NOT tracking accuracy acceptance.
                            if entry["vs_precision_matched_local"]["max_abs"] > max(
                                1e-4, float(np.max(bound)) * 2**-16
                            ):
                                raise ValueError(
                                    "pilot arithmetic exceeds conservative term-magnitude bound"
                                )
                            layer["modes"].append(entry)
                            if spec["name"] == args.layers[0]:
                                guard_control(job, batches[0], fault=True)
                                guard_control(job, batches[0], fault=False)
                                entry["guarded_fault_restored"] = True
                                entry["invalid_header_refused"] = True
                            (
                                args.output.parent
                                / (args.output.stem + "_" + spec["name"] + "_" + str(mode) + ".f32")
                            ).write_bytes(actual.tobytes())
                            args.output.write_text(json.dumps(report, indent=2) + "\n")
                            print(
                                json.dumps(
                                    {
                                        "layer": spec["name"],
                                        "mode": mode,
                                        "body_ms": sum(v["body_ms"] for v in samples),
                                        "error": entry["vs_local_fp32"],
                                    }
                                ),
                                flush=True,
                            )
                with Device() as observer:
                    after = bytes(observer._transport.memory[ARENA_START:ARENA_END])
                report["arena_after_sha256"] = hashlib.sha256(after).hexdigest()
                if before != after:
                    raise ValueError("resident arena changed")
                report["final_health"] = settled_health(power, baseline)
                report["service_after"] = service()
                if report["service_before"] != report["service_after"]:
                    raise ValueError("DSP service changed")
                report["all_checks_passed"] = True
            except BaseException as exc:
                failed = True
                report["error"] = f"{type(exc).__name__}: {exc}"
                report["after_failure"] = failure_snapshot(power)
                report["interpretation"] = "stopped without retry/recovery/restart"
    finally:
        lease.close()
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            {
                "all_checks_passed": report.get("all_checks_passed", False),
                "error": report.get("error"),
            }
        ),
        flush=True,
    )
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
