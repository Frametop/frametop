"""Opt-in HMX qualification with per-job restoration checks and timing comparison."""

import argparse
import hashlib
import json
import mmap
import os
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Device, RecoveryRequired
from framenpu.dispatch import DirectDispatcher, _firmware_original
from framenpu.profile import PROFILE
from framenpu.provenance import verify_kernel
from framenpu.transport import Transport
import hardware_dispatch
import hardware_smoke


class PowerSnapshot:
    """Read-only views of configuration touched by HmxPower; no power writes."""

    REGIONS = {
        0x32341000: (0, 4, 0x10, 0x14, 0x18),
        0x32348000: (0x180, 0x184, 0x1A0, 0x200),
        0x323C0000: (0, 8, 0x404, 0x410, 0x460, 0x464, 0x4BC, 0x4C0, *range(0x550, 0x680, 4)),
    }

    def __enter__(self):
        self.maps = {}
        self.fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
        try:
            for address in self.REGIONS:
                self.maps[address] = mmap.mmap(
                    self.fd, 4096, flags=mmap.MAP_SHARED, prot=mmap.PROT_READ, offset=address
                )
        except BaseException:
            self.__exit__(None, None, None)
            raise
        return self

    def __exit__(self, *exc):
        for view in self.maps.values():
            view.close()
        os.close(self.fd)

    def read(self):
        # Last-PC at RSC+0x408 is status, not restored configuration.
        return {
            f"{address:08x}": {
                f"{offset:03x}": struct.unpack_from("<I", self.maps[address], offset)[0]
                for offset in offsets
            }
            for address, offsets in self.REGIONS.items()
        }


def health(power, expected_power):
    current = power.read()
    if current != expected_power:
        raise RecoveryRequired("HMX configuration differs after the completed session")
    with Device() as observer:
        state = observer.status()
        if state["control"] != [1, 0, 1] or state["callback"] or state["poisoned"]:
            raise RecoveryRequired(f"unexpected post-session control: {state}")
        if DirectDispatcher.window(observer._transport) != _firmware_original():
            raise RecoveryRequired("dispatcher was not restored")
        image = Path(PROFILE.firmware_path).read_bytes()
        prefix = bytes(observer._transport.memory[:0x1C000])
        if (
            prefix
            != image[PROFILE.firmware_load_offset : PROFILE.firmware_load_offset + len(prefix)]
        ):
            raise RecoveryRequired("firmware code prefix changed")
    return {
        "state": state,
        "firmware_code_prefix_bytes": len(prefix),
        "firmware_code_prefix_sha256": hashlib.sha256(prefix).hexdigest(),
        "power_configuration_sha256": hashlib.sha256(
            json.dumps(current, sort_keys=True).encode()
        ).hexdigest(),
        "power_restored": True,
    }


def audited_device(power, counters):
    class AuditedTransport(Transport):
        def claim(self, timeout):
            super().claim(timeout)
            try:
                p = PROFILE
                self.saved = [
                    (offset, bytes(self.memory[offset : offset + size]))
                    for offset, size in (
                        (p.code, p.code_size),
                        (p.header, 256),
                        (p.status, 192),
                        (p.backup, 8192),
                        (p.arena, p.arena_size),
                    )
                ]
                self.saved_power = power.read()
            except BaseException:
                self.release_unsubmitted()
                raise

        def publish(self, *, direct=False):
            self.hmx_job = bool(self.get(PROFILE.header + 12) & 1)
            super().publish(direct=direct)

        def acknowledge(self, timeout):
            if (
                any(bytes(self.memory[o : o + len(data)]) != data for o, data in self.saved)
                or self.get(PROFILE.callback) != 0
                or power.read() != self.saved_power
            ):
                self.poisoned = True
                raise RecoveryRequired("per-job DDR, callback or HMX restoration failed")
            super().acknowledge(timeout)
            counters["restored_jobs"] += 1
            counters["restored_hmx_jobs"] += int(self.hmx_job)

    class AuditedDevice(Device):
        def __init__(self, **kwargs):
            super().__init__(_transport=AuditedTransport(), **kwargs)

    return AuditedDevice


def qualified_run(iterations):
    manifest = verify_kernel()
    counters = {"restored_jobs": 0, "restored_hmx_jobs": 0}
    with PowerSnapshot() as power:
        baseline = power.read()
        initial = health(power, baseline)

        AuditedDevice = audited_device(power, counters)

        print("HMX_STAGE minimal_direct_gemm", flush=True)
        left = [float(i == j) for i in range(32) for j in range(32)]
        right = [(i % 17 - 8) / 4 for i in range(1024)]
        packed = struct.pack("<2048e", *(left + right)) + bytes(128)
        with AuditedDevice(dispatch="direct") as device:
            result = device.submit(
                device.builtin(hmx=True),
                packed,
                output_size=4096,
                opcode=1,
                params=(32, 32, 32, 0),
            )
            if result.output != struct.pack("<1024f", *right):
                raise AssertionError("initial HMX GEMM differs from exact reference")
            initial_ssr = [row[7] for row in result.workers]
        print(
            "HMX_MINIMAL",
            json.dumps(
                {
                    "checked_values": 1024,
                    "max_abs_error": 0,
                    "kernel_reference_ticks": result.kernel_reference_ticks,
                    "health": health(power, baseline),
                }
            ),
            flush=True,
        )

        # Audit uploads/restoration during correctness runs. Use the ordinary
        # Device for timing, so readback audits do not inflate comparisons.
        previous = hardware_smoke.Device
        hardware_smoke.Device = AuditedDevice
        try:
            for mode in ("direct", "benchmark"):
                print(f"HMX_STAGE numerical_suite_{mode}", flush=True)
                hardware_smoke.run(dispatch=mode)
                print(
                    "HMX_HEALTH",
                    json.dumps(
                        {
                            "after": f"numerical_suite_{mode}",
                            "health": health(power, baseline),
                        }
                    ),
                    flush=True,
                )
        finally:
            hardware_smoke.Device = previous

        for mode in ("benchmark", "direct"):
            print(f"HMX_STAGE timing_{mode}", flush=True)
            hardware_dispatch.run(mode, iterations, True)
            health(power, baseline)

        with AuditedDevice(dispatch="direct") as device:
            result = device.submit(device.builtin(hmx=False), opcode=4)
            final_ssr = [row[7] for row in result.workers]
        if final_ssr != initial_ssr or final_ssr != [0x81840200, 0x89840200, 0x91840200]:
            raise RecoveryRequired("worker SSR did not return to its baseline")
        final = health(power, baseline)
        print(
            "HMX_QUALIFIED",
            json.dumps(
                {
                    "runtime_bytes": manifest["bytes"],
                    "runtime_sha256": manifest["sha256"],
                    "audits": counters,
                    "worker_ssr": [hex(x) for x in final_ssr],
                    "initial_health": initial,
                    "final_health": final,
                    "power_configuration": baseline,
                    "service": subprocess.run(
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
                        capture_output=True,
                        text=True,
                        check=True,
                    ).stdout,
                }
            ),
            flush=True,
        )


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-on-device", action="store_true")
    parser.add_argument("--iterations", type=int, default=100)
    args = parser.parse_args(argv)
    if not args.run_on_device:
        parser.error("--run-on-device is required; this changes DSP state and HMX power")
    if os.geteuid() != 0:
        parser.error("run with sudo for /dev/mem access")
    if not 1 <= args.iterations <= 1000:
        parser.error("--iterations must be between 1 and 1000")
    qualified_run(args.iterations)


if __name__ == "__main__":
    main()
