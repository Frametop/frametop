"""Opt-in depthwise component timing through the ordinary graph qualifier."""

import argparse
from dataclasses import replace
import json
import os
from pathlib import Path
import struct
import sys

import hardware_resident_landmark as qualifier
from framenpu import RecoveryRequired


def decode_profile_result(result):
    """Validate timing separately from numerical/native completion checks.

    A nonempty interval may round to zero at the reference timer's resolution.
    Zero segments, however, must never accumulate nonzero elapsed ticks.
    """
    try:
        words = struct.unpack("<64I", result.output)
        ticks = words[26] | words[27] << 32
        wrapper = result.kernel_reference_ticks
        if (
            words[29] != 0x44575031
            or wrapper is None
            or wrapper <= 0
            or ticks > wrapper
            or (words[28] == 0 and ticks != 0)
        ):
            raise ValueError("invalid profiling ticks, count or magic")
        clean = result.output[:104] + bytes(16) + result.output[120:]
        sample = {
            "start_layer": words[18],
            "end_layer": words[51],
            "ticks": ticks,
            "segments": words[28],
            "wrapper_ticks": wrapper,
        }
        return replace(result, output=clean), sample
    except Exception as exc:
        raise RecoveryRequired("depthwise timing metadata invalid; retain resources") from exc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", type=int, required=True, choices=range(1, 13))
    parser.add_argument("--profile-output", type=Path, required=True)
    parser.add_argument("--jobs", type=Path, required=True)
    args, remaining = parser.parse_known_args()
    if "--run-on-device" not in remaining or os.geteuid() != 0:
        parser.error("--run-on-device and Frame root are required")
    build = json.loads((args.jobs / "link.json").read_text())
    if build.get("profile_dw") != args.mode or "--linked" not in remaining:
        parser.error("linked profiling image mode differs")
    original = qualifier.audited_device
    samples = []

    def factory(power, counters):
        class ProfileDevice(original(power, counters)):
            def _submit(self, job, data, **kwargs):
                if kwargs.get("opcode") != 32:
                    return super()._submit(job, data, **kwargs)
                validate = kwargs["_completion_validator"]

                def complete(result):
                    clean, sample = decode_profile_result(result)
                    # All ordinary checks run before recording a sample.
                    validate(clean)
                    samples.append(sample)

                kwargs["_completion_validator"] = complete
                return super()._submit(job, data, **kwargs)

        return ProfileDevice

    qualifier.audited_device = factory
    sys.argv = [sys.argv[0], "--jobs", str(args.jobs), *remaining]
    try:
        return qualifier.main()
    finally:
        args.profile_output.write_text(
            json.dumps({"mode": args.mode, "build": build, "samples": samples}, indent=2) + "\n"
        )


if __name__ == "__main__":
    raise SystemExit(main())
