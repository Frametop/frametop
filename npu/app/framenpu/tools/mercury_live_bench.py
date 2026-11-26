"""Compare CPU/FrameNPU on identical, currently available live camera observations.

Requires the patched tracker, private factory calibration, ptgrab, MediaPipe,
ORT and FrameNPU. Does not save camera frames or poses. No DSP fallback is
accepted as a successful accelerated measurement. No retries or recovery.
"""

import argparse
import json
import os
from pathlib import Path
import sys
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("tracker", "jobs", "output"):
        p.add_argument("--" + name, type=Path, required=True)
    p.add_argument("--rounds", type=int, default=30)
    p.add_argument("--seconds", type=float, default=35)
    a = p.parse_args()
    sys.path.insert(0, str(a.tracker))
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
    for name in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
        os.environ.setdefault(name, "1")
    import numpy as np
    import cv2
    from handtrack.calib import Rig
    from handtrack.detect import HandDetector
    from handtrack.inference import make_backend
    from handtrack.live import PtShm, ROTATE, ThermalGuard, cpus_from_env
    from handtrack.mercury_kp import MercuryKeypoint
    from handtrack.palm import PalmTracker, SLOT_CAM
    from handtrack import mpworker
    from framenpu import Device
    from framenpu.resident_landmark import ARENA_START, ARENA_END
    import hashlib

    cv2.setNumThreads(1)
    os.sched_setaffinity(0, cpus_from_env())
    rig, guard = Rig(), ThermalGuard()
    model = os.path.expanduser("~/handtrack/models/mercury/grayscale_keypoint_jan18.onnx")
    cpu = MercuryKeypoint(model, rig, SLOT_CAM, 640, backend=make_backend(model))
    npu = MercuryKeypoint(
        model,
        rig,
        SLOT_CAM,
        640,
        backend=make_backend(model, inference="framenpu", framenpu_jobs=a.jobs),
    )
    palm = PalmTracker(640, rig)
    detectors = {s: HandDetector(mirrored=True) for s in (0, 1)}
    shm, last = PtShm(), {0: 0, 1: 0}
    with Device() as dev:
        before = hashlib.sha256(bytes(dev._transport.memory[ARENA_START:ARENA_END])).hexdigest()
    report = {
        "format": "framenpu-live-paired-inference",
        "requests": [],
        "arena_before_sha256": before,
        "scope": "same live observations, alternating backend order; crop + inference + decode; no worker IPC",
    }
    start, previous = time.monotonic(), mpworker._merc
    try:
        while len(report["requests"]) < a.rounds and time.monotonic() - start < a.seconds:
            if guard.read() >= guard.PAUSE_C:
                raise RuntimeError("existing tracker thermal pause threshold reached")
            frames = {s: shm.read(s, last[s]) for s in (0, 1)}
            if any(not f for f in frames.values()) or abs(frames[0][1] - frames[1][1]) > 8e6:
                time.sleep(0.002)
                continue
            last = {s: frames[s][0] for s in (0, 1)}
            imgs = {s: cv2.rotate(frames[s][2], ROTATE[s]) for s in (0, 1)}
            ts = max(frames[s][1] for s in (0, 1)) / 1e9
            hands = {s: detectors[s].detect(imgs[s], ts) for s in (0, 1)}
            poses = palm.fuse(hands[0], hands[1], ts)
            jobs = []
            for side, pose in poses.items():
                if pose is None:
                    continue
                for slot in (0, 1):
                    rotation, origin = palm.pose[slot]
                    jc = (pose[3] - origin) @ rotation
                    if (jc[:, 2] >= 0.08).all():
                        jobs.append((slot, side, jc, None, True))
            if not jobs:
                continue
            index = len(report["requests"])
            count = min(1 + index % 3, len(jobs))
            jobs = jobs[:count]  # use only observations available now, never pad or wait
            order = ("cpu", "framenpu") if index % 2 == 0 else ("framenpu", "cpu")
            row = {"crops": count, "order": order, "temperature_c": guard.temp}
            got = {}
            for mode in order:
                mk = cpu if mode == "cpu" else npu
                mpworker._merc = mk
                t0 = time.perf_counter()
                got[mode] = mpworker.run_mercury(jobs, imgs)
                elapsed = (time.perf_counter() - t0) * 1000
                if mode == "framenpu" and (
                    mk.backend.name != "framenpu" or not mk.backend.last_timing
                ):
                    raise RuntimeError("accelerator fell back to CPU; measurement refused")
                timing = got[mode][0][4]
                row[mode] = {
                    "wall_ms": elapsed,
                    "thread_cpu_ms": timing["cpu_ns"] / 1e6,
                    "runqueue_wait_ms": timing["wait_ns"] / 1e6,
                    "dsp_body_ms": timing.get("dsp_body_ms", 0),
                }
            differences, flips = [], 0
            for c, n in zip(got["cpu"], got["framenpu"]):
                if (c[2] is None) != (n[2] is None):
                    raise ValueError("blank crop handling differs")
                if c[2] is not None:
                    differences.extend(
                        np.linalg.norm(c[2]["det_px"] - n[2]["det_px"], axis=1).tolist()
                    )
                    flips += (c[2]["is_hand"] >= 0.97) != (n[2]["is_hand"] >= 0.97)
            row["detector_px_drift"] = (
                {"median": float(np.median(differences)), "max": max(differences)}
                if differences
                else None
            )
            row["is_hand_flips"] = int(flips)
            report["requests"].append(row)
        report["all_checks_passed"] = bool(report["requests"])
    except BaseException as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
        report["all_checks_passed"] = False
    finally:
        mpworker._merc = previous
        try:
            npu.close()
            cpu.close()
        except BaseException as exc:
            report["close_error"] = f"{type(exc).__name__}: {exc}"
            report["all_checks_passed"] = False
        for d in detectors.values():
            d.close()
        with Device() as dev:
            after = hashlib.sha256(bytes(dev._transport.memory[ARENA_START:ARENA_END])).hexdigest()
        report["arena_after_sha256"] = after
        if after != before:
            report["all_checks_passed"] = False
        a.output.write_text(json.dumps(report, indent=1) + "\n")
    print(
        json.dumps(
            {
                "all_checks_passed": report["all_checks_passed"],
                "requests": len(report["requests"]),
                "error": report.get("error"),
            }
        )
    )
    return int(not report["all_checks_passed"])


if __name__ == "__main__":
    sys.exit(main())
