"""Qualify the packaged MercurySession on private cases, using prior final hashes.

Runs available groups of 1..3 immediately; checks the hint staging against the
qualified cases, every final tensor against replay evidence, idle/resume, and
full arena, service, firmware and power restoration. No recovery or retries.
"""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "src"), str(ROOT / "tests")]
from framenpu import Device, mercury
from framenpu import mercury_runtime as M
from framenpu import _mercury_layout as L
from framenpu.resident_landmark import ARENA_START, ARENA_END
from hardware_hmx import PowerSnapshot, audited_device
from hardware_resident import settled_health
from hardware_landmark import service
from hmx_multi_run import failure_snapshot


def main():
    import numpy as np

    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    for name in ("model", "jobs", "cases", "baseline", "output"):
        p.add_argument("--" + name, required=True, type=Path)
    a = p.parse_args()
    if not a.run_on_device or os.geteuid() != 0:
        p.error("Frame root and --run-on-device required")
    manifest = json.loads((a.cases / "manifest.json").read_text())
    baseline = json.loads(a.baseline.read_text())
    if not baseline["all_checks_passed"] or baseline["model_sha256"] != mercury.KEYPOINT_SHA256:
        p.error("qualified replay baseline required")
    loaded = []
    for entry in manifest["cases"]:
        raw = (a.cases / (entry["name"] + ".npz")).read_bytes()
        if hashlib.sha256(raw).hexdigest() != entry["sha256"]:
            p.error("private case hash differs")
        with np.load(io.BytesIO(raw), allow_pickle=False) as z:
            loaded.append({k: z[k] for k in z.files})
    feeds = []
    for i, entry in enumerate(manifest["cases"]):
        hint = np.zeros((1, 42), np.float32)
        if entry["hinted"]:
            name = entry["name"].removesuffix("-hint") + "-nohint"
            j = next(j for j, e in enumerate(manifest["cases"]) if e["name"] == name)
            ref = {n: loaded[j]["fp32/" + n] for n in mercury.OUTPUTS}
            hint = mercury.hint_from_grid(mercury.decode(ref)["grid"]).reshape(1, 42)
        feeds.append(
            {
                "inputImg": loaded[i]["image"].astype(np.float32).reshape(1, 1, 128, 128),
                "lastKeypoints": hint,
                "useLastKeypoints": np.array([float(entry["hinted"])], np.float32),
            }
        )
    model = mercury.load_keypoint(a.model)
    for i, feed in enumerate(feeds):
        ts = {
            "lastKeypoints": feed["lastKeypoints"].reshape(42),
            "useLastKeypoints": feed["useLastKeypoints"],
        }
        for name in L.HOST_HINT_OPS[:3]:
            op = model.op(name)
            ts[op.output] = mercury._step(op, ts, "fp16", "fp16")
        if not np.array_equal(ts[L.HINT], loaded[i]["hint"]):
            raise ValueError(f"case {i}: reconstructed hint differs; no DSP submitted")
    report = {
        "format": "framenpu-mercury-runtime-run",
        "model_sha256": mercury.KEYPOINT_SHA256,
        "image_sha256": M.IMAGE_SHA256,
        "source_sha256": {
            n: hashlib.sha256((ROOT / n).read_bytes()).hexdigest()
            for n in (
                "src/framenpu/mercury_runtime.py",
                "src/framenpu/_mercury_layout.py",
                "src/framenpu/_mercury_common.py",
                "tests/hardware_mercury_runtime.py",
            )
        },
        "baseline_sha256": hashlib.sha256(a.baseline.read_bytes()).hexdigest(),
        "cases_manifest_sha256": hashlib.sha256(
            (a.cases / "manifest.json").read_bytes()
        ).hexdigest(),
        "service_before": service(),
        "requests": [],
        "audits": {"restored_jobs": 0, "restored_hmx_jobs": 0},
    }
    failed = False
    with PowerSnapshot() as power:
        initial_power = power.read()
        report["initial_health"] = settled_health(power, initial_power)
        with Device() as dev:
            before = bytes(dev._transport.memory[ARENA_START:ARENA_END])
        report["arena_before_sha256"] = hashlib.sha256(before).hexdigest()
        session = M.MercurySession(
            a.model, a.jobs, _device_factory=audited_device(power, report["audits"])
        )
        try:
            for i, feed in enumerate(feeds):
                count = 1 + i % 3
                indices = [(i + k) % len(feeds) for k in range(count)]
                t0 = time.perf_counter()
                outputs = session.infer([feeds[j] for j in indices])
                wall = (time.perf_counter() - t0) * 1000
                hashes = []
                for k, j in enumerate(indices):
                    mp = session.plan.crop(k)
                    g = mp.grid(L.OUTPUT)
                    at = mp.where[L.OUTPUT]
                    data = bytes(session.device._transport.memory[at : at + g.tensor_bytes(160)])
                    sha = hashlib.sha256(data).hexdigest()
                    if sha != baseline["cases"][f"case{j:02d}"]["final_sha256"]:
                        raise ValueError(f"case {j} crop {k}: final hash differs")
                    if not all(
                        np.isfinite(v).all() and v.shape == mercury.OUTPUTS[n]
                        for n, v in outputs[k].items()
                    ):
                        raise ValueError("host heads returned invalid output")
                    hashes.append(sha)
                report["requests"].append(
                    dict(session.last_timing, cases=indices, final_sha256=hashes, host_wall_ms=wall)
                )
                if i == 7:
                    session.idle()
                    if session.power is not None:
                        raise ValueError("idle did not release held power")
                    report["idle_release"] = True
        except BaseException as exc:
            failed = True
            report["error"] = f"{type(exc).__name__}: {exc}"
            report["after_failure"] = failure_snapshot(power)
        finally:
            try:
                session.close()
            except BaseException as exc:
                failed = True
                report["close_error"] = f"{type(exc).__name__}: {exc}"
        if not failed:
            with Device() as dev:
                after = bytes(dev._transport.memory[ARENA_START:ARENA_END])
            report["arena_after_sha256"] = hashlib.sha256(after).hexdigest()
            report["final_health"] = settled_health(power, initial_power)
            report["service_after"] = service()
            report["all_checks_passed"] = (
                before == after and report["service_before"] == report["service_after"]
            )
            failed = not report["all_checks_passed"]
    a.output.write_text(json.dumps(report, indent=1) + "\n")
    print(
        json.dumps(
            {
                "all_checks_passed": report.get("all_checks_passed", False),
                "error": report.get("error"),
                "requests": len(report["requests"]),
            }
        )
    )
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
