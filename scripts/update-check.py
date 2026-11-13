#!/usr/bin/env python3
"""Read-only SteamOS interface checks after an update. Never starts SteamVR."""
from pathlib import Path
import platform
import sys

ROOT = Path(__file__).resolve().parents[1]
checks = {
    "Steam Frame architecture": platform.machine() == "aarch64",
    "SteamVR OpenVR ARM64 API": Path("/opt/steamvr/bin/linuxarm64/libopenvr_api.so").is_file(),
    "XRService camera calibration": Path("/persist/xrservice.json").is_file(),
    "device camera calibration": Path("/persist/device_config.json").is_file(),
    "v4l2 dequeue tracepoint": Path("/sys/kernel/tracing/events/v4l2/v4l2_dqbuf/format").is_file(),
}
for name, ok in checks.items():
    print(f"{'ok' if ok else 'MISSING'}: {name}")
if any(not ok for ok in checks.values()):
    print("Retest fh-camd camera attachment and calibration before using FrameHands.")
print("Retest the OpenVR DMA-BUF hand overlay and Room View detection on the headset after a SteamVR update.")
sys.exit(0 if all(checks.values()) else 1)
