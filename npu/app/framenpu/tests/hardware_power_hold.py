"""Opt-in idle check for session-held HMX power: no DSP job is published.

Powers HMX on under the operation gate, releases the gate, samples the held
configuration while idle, then powers off under the gate. A drifting register
is reported, and power is still closed and verified against the baseline.
"""

import argparse
import json
import os
from pathlib import Path
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Device
from hardware_hmx import PowerSnapshot
from hardware_resident import settled_health
from hardware_landmark import service


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run-on-device", action="store_true")
    p.add_argument("--seconds", type=float, default=3.0)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()
    if not args.run_on_device or os.geteuid() != 0:
        p.error("--run-on-device and Frame root are required")
    report = {"format": "framenpu-power-hold", "seconds": args.seconds, "service_before": service()}
    with PowerSnapshot() as snapshot:
        baseline = snapshot.read()
        report["initial_health"] = settled_health(snapshot, baseline)
        with Device() as device:
            started = time.monotonic_ns()
            power = device._hold_power()
            report["hold_ms"] = (time.monotonic_ns() - started) / 1e6
            held = snapshot.read()
            changed = {
                f"{region}+{offset}": [baseline[region][offset], value]
                for region, values in held.items()
                for offset, value in values.items()
                if baseline[region][offset] != value
            }
            report["registers_changed_by_power_on"] = len(changed)
            samples = drift = 0
            first_drift = None
            deadline = time.monotonic() + args.seconds
            while time.monotonic() < deadline:
                now = snapshot.read()
                samples += 1
                if now != held or power._held() != power.held:
                    drift += 1
                    first_drift = first_drift or {
                        f"{region}+{offset}": [held[region][offset], value]
                        for region, values in now.items()
                        for offset, value in values.items()
                        if held[region][offset] != value
                    }
                time.sleep(0.002)
            report.update(samples=samples, drift_samples=drift, first_drift=first_drift)
            report["status_while_held"] = device.status()
            started = time.monotonic_ns()
            if drift:
                # Nothing was published, so closing is safe; skip the equality proof.
                t = device._transport
                t.claim(device.claim_timeout)
                power.close()
                t.release_unsubmitted()
            else:
                device._release_power(power)
            report["release_ms"] = (time.monotonic_ns() - started) / 1e6
        report["final_health"] = settled_health(snapshot, baseline)
    report["service_after"] = service()
    report["all_checks_passed"] = not drift and report["service_after"] == report["service_before"]
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: v for k, v in report.items() if "health" not in k}), flush=True)
    return int(not report["all_checks_passed"])


if __name__ == "__main__":
    raise SystemExit(main())
