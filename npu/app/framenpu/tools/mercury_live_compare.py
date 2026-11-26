"""Run balanced (ABBA) tracker trials and keep aggregate timing evidence.

--order lists runs as backend[:scheduler[:uclamp]]; scheduler is early
(--merc-early, the default) or fresh (--merc-fresh); uclamp (0..1024) passes
--uclamp-max. Default: CPU/NPU/NPU/CPU, early, no clamp. Each run also records
rail power (Frame MAX34417 monitors), zone temperatures and fan speed after
the first 8 s of the process.

Camera capture and calibration must already be ready. Raw telemetry stays in
--directory on the Frame. The output contains timings/counts/temperatures only.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from mercury_live_report import summarize


def zones():
    out = {}
    for z in Path("/sys/class/thermal").glob("thermal_zone*"):
        kind = (z / "type").read_text().strip()
        if kind.startswith(("cpu", "gpuss", "nsp")):
            try:
                out[kind] = int((z / "temp").read_text()) / 1000
            except (OSError, ValueError):
                pass
    return out


def hottest():
    return max(t for k, t in zones().items() if k.startswith(("cpu", "gpuss")))


def rails():
    """Rail power in W from the Frame's MAX34417 monitors, plus fan RPM (empty elsewhere)."""
    out = {}
    for h in Path("/sys/class/hwmon").glob("hwmon*"):
        try:
            name = (h / "name").read_text().strip()
            if name.startswith("max34417"):
                for i in range(1, 5):
                    label = (h / f"power{i}_label").read_text().strip()
                    if label != "unused":
                        out[label] = int((h / f"power{i}_input").read_text()) / 1e6
            elif (h / "fan1_input").exists():
                out["fan_rpm"] = int((h / "fan1_input").read_text())
        except (OSError, ValueError):
            pass
    return out


def physical(samples, skip_s=8):
    import numpy as np

    kept = [s for s in samples if s["t"] >= skip_s] or samples
    cpu = [{k: t for k, t in s["zones"].items() if k.startswith(("cpu", "gpuss"))} for s in kept]
    hot = [max(z.values()) for z in cpu]
    which = [max(z, key=z.get) for z in cpu]
    keys = sorted({k for s in kept for k in s["rails"]})
    return {
        "samples": len(kept),
        "hottest_c": {
            "median": float(np.median(hot)),
            "p95": float(np.percentile(hot, 95)),
            "max": max(hot),
        },
        "hottest_zone_mode": max(set(which), key=which.count),
        "zone_max_c": {
            k: max(s["zones"].get(k, 0) for s in kept) for k in sorted(kept[0]["zones"])
        },
        "rail_mean": {
            k: float(np.mean([s["rails"][k] for s in kept if k in s["rails"]])) for k in keys
        },
    }


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("tracker", "jobs", "directory", "output"):
        p.add_argument("--" + name, type=Path, required=True)
    p.add_argument("--seconds", type=int, default=18)
    p.add_argument("--warmup", type=float, default=3)
    p.add_argument("--order", default="cpu,framenpu,framenpu,cpu")
    p.add_argument(
        "--cooldown-c",
        type=float,
        help="before each run, wait (at most 300 s) until the hottest CPU/GPU zone is at or below this",
    )
    a = p.parse_args()
    order = []
    for token in a.order.split(","):
        parts = token.split(":")
        backend, scheduler, clamp = (parts + ["early", ""][len(parts) - 1 :])[:3]
        if (
            backend not in ("cpu", "framenpu")
            or scheduler not in ("early", "fresh")
            or (clamp and not (clamp.isdigit() and int(clamp) <= 1024))
        ):
            p.error("--order tokens are cpu|framenpu[:early|fresh[:0..1024]]")
        order.append(":".join(x for x in (backend, scheduler, clamp) if x))
    if not a.directory.is_dir():
        p.error("existing private test directory required")
    report = {
        "format": "framenpu-live-tracker-abba",
        "seconds_per_run": a.seconds,
        "warmup_s": a.warmup,
        "runs": [],
        "order": order,
        "scope": "live camera pipeline, local pose only; camera detected timestamps, not motion-to-photon latency",
    }
    try:
        for i, token in enumerate(order):
            mode, scheduler, clamp = (token.split(":") + [""])[:3]
            stem = f"abba-{i}-{mode}-{scheduler}" + (f"-u{clamp}" if clamp else "")
            telemetry, log = a.directory / f"{stem}.csv", a.directory / f"{stem}.log"
            command = [
                sys.executable,
                "-m",
                "handtrack.live",
                "--backend",
                "mercury",
                "--merc-inference",
                mode,
                "--framenpu-jobs",
                str(a.jobs),
                "--merc-" + scheduler,
                "--flow-v2",
                "--no-http",
                "--no-bridge",
                "--seconds",
                str(a.seconds),
                "--telemetry",
                str(telemetry),
            ] + (["--uclamp-max", clamp] if clamp else [])
            env = dict(os.environ)
            env["PYTHONPATH"] = str(a.tracker) + os.pathsep + env.get("PYTHONPATH", "")
            samples, waited = [], 0.0
            if a.cooldown_c is not None:
                begin = time.monotonic()
                while hottest() > a.cooldown_c:
                    if time.monotonic() - begin > 300:
                        raise RuntimeError("device did not cool to the requested start temperature")
                    time.sleep(2)
                waited = time.monotonic() - begin
            print(
                json.dumps(
                    {
                        "run": i,
                        "backend": mode,
                        "scheduler": scheduler,
                        "uclamp_max": clamp or None,
                        "phase": "starting",
                    }
                ),
                flush=True,
            )
            with log.open("w") as output:
                process = subprocess.Popen(
                    command, env=env, stdout=output, stderr=subprocess.STDOUT
                )
                try:
                    deadline = time.monotonic() + a.seconds + 60
                    started, physical_samples = time.monotonic(), []
                    while process.poll() is None:
                        if time.monotonic() > deadline:
                            raise RuntimeError(
                                "tracker did not exit; worker startup or shutdown hung"
                            )
                        z = zones()
                        temp = max(t for k, t in z.items() if k.startswith(("cpu", "gpuss")))
                        samples.append(temp)
                        physical_samples.append(
                            {"t": time.monotonic() - started, "zones": z, "rails": rails()}
                        )
                        if temp >= 92:
                            raise RuntimeError("existing tracker thermal pause threshold reached")
                        time.sleep(0.5)
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=20)  # graceful DSP cleanup; never SIGKILL/retry
            summary = summarize(telemetry, a.warmup)
            row = {
                "index": i,
                "backend": mode,
                "scheduler": scheduler,
                "uclamp_max": int(clamp) if clamp else None,
                "cooldown_s": waited,
                "physical": physical(physical_samples) if physical_samples else None,
                "exit_code": process.returncode,
                "temperature_c": {"start": samples[0], "end": samples[-1], "max": max(samples)},
                "summary": summary,
            }
            report["runs"].append(row)
            print(json.dumps(row), flush=True)
            if process.returncode or "error" in summary:
                raise RuntimeError("tracker run did not produce a valid measurement")
            if set(summary["backend_observations"]) != {mode}:
                raise RuntimeError("requested backend was not used throughout; refusing comparison")
            if "FrameNPU disabled" in log.read_text() or "FrameNPU unavailable" in log.read_text():
                raise RuntimeError("accelerator failed; no further DSP requests will be attempted")
        report["all_checks_passed"] = True
    except BaseException as exc:
        report["all_checks_passed"] = False
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        a.output.write_text(json.dumps(report, indent=1) + "\n")
    return int(not report["all_checks_passed"])


if __name__ == "__main__":
    sys.exit(main())
