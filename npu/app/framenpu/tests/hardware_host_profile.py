"""Opt-in host profiling of the qualified fused graph; no native or policy changes.

ABBA sessions compare audited transport with ordinary Device. Scoped timers wrap
whole replay/packing calls, never individual work units. Uninstrumented control
frames use the same checks. Failures stop without retries or recovery. cProfile
runs saved host-only replay inputs after all device sessions have closed.
"""

import argparse
from contextlib import contextmanager
import cProfile
import hashlib
import json
import math
import os
from pathlib import Path
import pstats
import sys
import time
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tools")]
from framenpu import Device
from framenpu.convolution import load_landmark_graph
from framenpu.landmark import OUTPUTS
import framenpu.resident_landmark as rl
from hardware_hmx import PowerSnapshot, audited_device
from hardware_landmark import digest, service
from hardware_resident import settled_health
from hmx_multi_run import failure_snapshot


def milliseconds(start):
    return (time.monotonic_ns() - start) / 1e6


class HostTimers:
    def __init__(self):
        self.current = None
        self.replay_inputs = []

    @contextmanager
    def installed(self):
        replay, pack = rl._batch_transition, rl._fp32

        def measured_replay(before, words, count, records, fused=False):
            if self.current is None:
                return replay(before, words, count, records, fused)
            start = time.monotonic_ns()
            result = replay(before, words, count, records, fused)
            self.current["replay_ms"] += milliseconds(start)
            if self.current["capture"]:
                self.replay_inputs.append(
                    (list(before), list(words), count, records, fused, result)
                )
            return result

        def measured_pack(values):
            if self.current is None:
                return pack(values)
            start = time.monotonic_ns()
            result = pack(values)
            self.current["input_pack_ms"] += milliseconds(start)
            return result

        with (
            patch.object(rl, "_batch_transition", measured_replay),
            patch.object(rl, "_fp32", measured_pack),
        ):
            yield


class ProfiledResident(rl.ResidentLandmark):
    def __init__(self, *args, timers, **kwargs):
        super().__init__(*args, **kwargs)
        self.timers = timers

    def _command(self, *args, **kwargs):
        current = self.timers.current
        if current is None:
            return super()._command(*args, **kwargs)
        start = time.monotonic_ns()
        replay = current["replay_ms"]
        result = super()._command(*args, **kwargs)
        elapsed = milliseconds(start)
        self.metrics["commands"][-1]["host_profile"] = {
            "command_api_ms": elapsed,
            "replay_ms": current["replay_ms"] - replay,
        }
        return result

    def _read_tensor(self, *args, **kwargs):
        start = time.monotonic_ns()
        result = super()._read_tensor(*args, **kwargs)
        if self.timers.current is not None:
            self.timers.current["head_read_ms"] += milliseconds(start)
        return result


def frame_breakdown(wall_ms, body_ms, commands, measured):
    """Disjoint wall-time partition. Device phase times contain replay time."""
    phases = {
        name: sum(c["host_phase_ms"].get(name, 0) for c in commands)
        for name in commands[0]["host_phase_ms"]
    }
    api = sum(c["host_profile"]["command_api_ms"] for c in commands)
    native = sum(c["wrapper_ms"] for c in commands)
    replay = measured["replay_ms"]
    result = {
        "input_pack_ms": measured["input_pack_ms"],
        "input_check_and_call_ms": wall_ms - body_ms - measured["input_pack_ms"],
        "native_wrapper_ms": native,
        "publication_over_wrapper_ms": phases["publish_wait_ms"] - native,
        "completion_replay_ms": replay,
        "completion_other_ms": phases["read_validate_ms"] - replay,
        "claim_ms": phases["claim_ms"],
        "ack_ms": phases["ack_ms"],
        "held_power_verify_ms": phases["power_start_ms"],
        "submit_other_ms": api
        - sum(
            phases[k]
            for k in ("claim_ms", "ack_ms", "power_start_ms", "publish_wait_ms", "read_validate_ms")
        ),
        "head_read_ms": measured["head_read_ms"],
        "body_other_ms": body_ms - api - measured["head_read_ms"],
    }
    if any(not math.isfinite(v) or v < -0.01 for v in result.values()):
        raise ValueError("host timing scopes overlap or differ")
    if not math.isclose(sum(result.values()), wall_ms, abs_tol=1e-7):
        raise ValueError("host timing partition does not sum to frame wall time")
    return result


def preflight(args):
    import numpy as np

    jobs = rl.load_linked(args.jobs)
    if not jobs.fused:
        raise ValueError("host profile requires the qualified fused build")
    identity = {
        "loader": hashlib.sha256(jobs.loader.code).hexdigest(),
        "linked": hashlib.sha256(jobs.image).hexdigest(),
    }
    build = json.loads((args.jobs / "link.json").read_text())
    for name, expected in build["sources"].items():
        if digest(ROOT / name) != expected:
            raise ValueError("native build source pin differs")
    graph = load_landmark_graph(args.model / "hand.ncnn.param", args.model / "hand.ncnn.bin")
    manifest = json.loads((args.corpus / "manifest.json").read_text())
    frozen = "381a033d6f4c10047998dfe42abb8cd6b18a756741c8a21690a3e6d6dc951f7b"
    if manifest["tolerances_sha256"] != frozen or digest(args.corpus / "tolerances.json") != frozen:
        raise ValueError("frozen numerical policy differs")
    crops = {c["name"]: c for c in manifest["crops"]}
    expected_heads = {}
    for split in ("calibration", "heldout"):
        prior = json.loads(getattr(args, split + "_report").read_text())
        if (
            not all(
                prior.get(k)
                for k in (
                    "qualification_complete",
                    "all_checks_passed",
                    "linked",
                    "cached_code",
                    "hold_power",
                )
            )
            or prior["budget_ms"] != 20
        ):
            raise ValueError("completed cached/held-power 20ms qualification required")
        for key, expected in [
            ("jobs_sha256", identity),
            ("model_sha256", graph.weights_sha256),
            ("param_sha256", graph.param_sha256),
            ("manifest_sha256", digest(args.corpus / "manifest.json")),
            ("tolerances_sha256", frozen),
            ("split", split),
        ]:
            if prior[key] != expected:
                raise ValueError("qualified report/model/corpus pin differs")
        for name, expected in prior["source_sha256"].items():
            if digest(ROOT / name) != expected:
                raise ValueError("qualified host source pin differs")
        if len(prior["crops"]) != 2:
            raise ValueError("incomplete split qualification")
        for crop in prior["crops"]:
            if (
                len(crop["checks"]) != 77
                or not crop["execution"]["complete"]
                or set(crop["outputs"]) != set(OUTPUTS)
                or crops[crop["name"]]["split"] != split
            ):
                raise ValueError("incomplete qualified crop")
            expected_heads[crop["name"]] = {k: v["sha256"] for k, v in crop["outputs"].items()}
    if set(crops) != set(expected_heads) or len(crops) != 4:
        raise ValueError("four qualified recorded crops required")
    inputs = {}
    for name, crop in crops.items():
        path = args.corpus / name / "in0.f32"
        if digest(path) != crop["files"]["in0"]["sha256"]:
            raise ValueError("recorded input hash differs")
        inputs[name] = np.fromfile(path, dtype="<f4")
        if len(inputs[name]) != 150528 or not np.isfinite(inputs[name]).all():
            raise ValueError("input shape/range differs")
    return graph, jobs, inputs, expected_heads, identity


def host_only_profile(inputs):
    """Replay saved requests with cProfile only after all HMX owners close."""
    profiler = cProfile.Profile()
    start = time.monotonic_ns()
    profiler.enable()
    for before, words, count, records, fused, expected in inputs:
        if rl._batch_transition(before, words, count, records, fused) != expected:
            raise ValueError("saved host-only replay differs")
    profiler.disable()
    elapsed = milliseconds(start)
    stats = pstats.Stats(profiler)
    rows = [
        {
            "function": Path(path).name + ":" + name,
            "primitive_calls": primitive,
            "calls": calls,
            "self_ms": own * 1000,
            "cumulative_ms": cumulative * 1000,
        }
        for (path, line, name), (primitive, calls, own, cumulative, callers) in stats.stats.items()
    ]
    return {
        "requests": len(inputs),
        "units": sum(x[2] for x in inputs),
        "wall_ms_with_cprofile": elapsed,
        "functions": sorted(rows, key=lambda x: x["cumulative_ms"], reverse=True)[:16],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    for name in ("jobs", "model", "corpus", "calibration-report", "heldout-report", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--frames", type=int, default=8, help="instrumented frames per session")
    parser.add_argument("--warmup", type=int, default=2)
    args = parser.parse_args(argv)
    if not args.run_on_device or os.geteuid() != 0:
        parser.error("--run-on-device and Frame root required")
    if not 4 <= args.frames <= 32 or args.frames % 4 or not 1 <= args.warmup <= 4:
        parser.error("frames must be a multiple of four in [4,32]; warmup in [1,4]")
    graph, jobs, inputs, expected, identity = preflight(args)
    timers = HostTimers()
    sources = [
        "tests/hardware_host_profile.py",
        "tests/hardware_hmx.py",
        "tests/hardware_resident.py",
        "src/framenpu/resident_landmark.py",
        "src/framenpu/device.py",
        "src/framenpu/transport.py",
        "src/framenpu/power.py",
        "src/framenpu/dispatch.py",
    ]
    report = {
        "format": "framenpu-host-profile",
        "version": 1,
        "jobs_sha256": identity,
        "model_sha256": graph.weights_sha256,
        "manifest_sha256": digest(args.corpus / "manifest.json"),
        "source_sha256": {s: digest(ROOT / s) for s in sources},
        "qualification_reports_sha256": {
            s: digest(getattr(args, s + "_report")) for s in ("calibration", "heldout")
        },
        "budget_ms": 20,
        "timeout_ms": 40,
        "cached_code": True,
        "hold_power": True,
        "blocks": [],
        "service_before": service(),
    }
    failed = False
    with PowerSnapshot() as power:
        baseline = power.read()
        report["initial_health"] = settled_health(power, baseline)
        try:
            for index, mode in enumerate(("audited", "ordinary", "ordinary", "audited")):
                block = {
                    "index": index,
                    "mode": mode,
                    "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
                    "frames": [],
                }
                report["blocks"].append(block)
                with Device() as observer:
                    original = bytes(observer._transport.memory[rl.ARENA_START : rl.ARENA_END])
                block["arena_before_sha256"] = hashlib.sha256(original).hexdigest()
                factory = audited_device(power, block["audits"]) if mode == "audited" else Device
                owner = ProfiledResident(
                    graph,
                    jobs,
                    cached_code=True,
                    hold_power=True,
                    budget_ms=20,
                    timers=timers,
                    _device_factory=factory,
                )
                block["metrics"] = owner.metrics
                # No instrumentation during open/close. Ordinary transport keeps all
                # production validation, power checks and restoration, minus the
                # harness's extra per-job snapshots/readbacks only.
                with owner:
                    held = power.read()
                    schedule = (
                        ["warmup"] * args.warmup + ["control"] * 2 + ["profiled"] * args.frames
                    )
                    with timers.installed():
                        for number, kind in enumerate(schedule):
                            crop = list(inputs)[number % 4]
                            first = len(owner.metrics["commands"])
                            measured = {
                                "replay_ms": 0.0,
                                "input_pack_ms": 0.0,
                                "head_read_ms": 0.0,
                                "capture": mode == "ordinary"
                                and kind == "profiled"
                                and not timers.replay_inputs,
                            }
                            timers.current = measured if kind == "profiled" else None
                            start = time.monotonic_ns()
                            try:
                                outputs = owner.run(inputs[crop])
                            finally:
                                wall = milliseconds(start)
                                timers.current = None
                            hashes = {
                                k: hashlib.sha256(v.tobytes()).hexdigest()
                                for k, v in outputs.items()
                            }
                            if hashes != expected[crop]:
                                raise ValueError("profile frame heads differ from qualified crop")
                            commands = owner.metrics["commands"][first:]
                            run = owner.metrics["runs"][-1]
                            frame = {
                                "crop": crop,
                                "kind": kind,
                                "wall_ms": wall,
                                "body_ms": run["body_ms"],
                                "commands": [first, len(owner.metrics["commands"])],
                                "command_count": len(commands),
                                "units": sum(c["count"] for c in commands if c["op"] == 5),
                                "native_wrapper_ms": sum(c["wrapper_ms"] for c in commands),
                                "heads_sha256": hashes,
                            }
                            if kind == "profiled":
                                frame["breakdown_ms"] = frame_breakdown(
                                    wall, run["body_ms"], commands, measured
                                )
                            block["frames"].append(frame)
                            print(
                                json.dumps(
                                    {
                                        "block": index,
                                        "mode": mode,
                                        "kind": kind,
                                        "crop": crop,
                                        "wall_ms": wall,
                                    }
                                ),
                                flush=True,
                            )
                    if power.read() != held:
                        raise ValueError("held HMX power changed")
                with Device() as observer:
                    restored = bytes(observer._transport.memory[rl.ARENA_START : rl.ARENA_END])
                block["arena_after_sha256"] = hashlib.sha256(restored).hexdigest()
                if restored != original:
                    raise ValueError("full resident arena restoration differs")
                block["final_health"] = settled_health(power, baseline)
                if service() != report["service_before"]:
                    raise ValueError("DSP service changed")
                block["all_checks_passed"] = True
                args.output.write_text(json.dumps(report, indent=2) + "\n")
            report["final_health"] = settled_health(power, baseline)
            report["service_after"] = service()
            if (
                report["initial_health"] != report["final_health"]
                or report["service_before"] != report["service_after"]
            ):
                raise ValueError("final service/health changed")
            report["host_only_replay"] = host_only_profile(timers.replay_inputs)
            report["all_checks_passed"] = True
        except BaseException as exc:
            failed = True
            report["error"] = f"{type(exc).__name__}: {exc}"
            report["after_failure"] = failure_snapshot(power)
            report["interpretation"] = "stopped without retry, recovery or restart"
            if "owner" in locals():
                report["failed_native"] = owner.native
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
