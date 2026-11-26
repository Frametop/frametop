"""Source provenance for the bundled, SDK-generated DSP payload."""

import hashlib
from pathlib import Path

from .compiler import BUILD_FLAGS, SDK
from .profile import JOB_ABI


def source_hashes():
    package = SDK.parent
    names = (
        "compiler.py",
        "sdk/entry.S",
        "sdk/hmx.S",
        "sdk/link.ld",
        "sdk/include/framenpu.h",
        "sdk/include/fnpu_readback.h",
        "sdk/runtime.c",
        "sdk/dispatch.S",
        "sdk/dispatch.ld",
        "sdk/dispatch_patch.c",
    )
    return {name: hashlib.sha256((package / name).read_bytes()).hexdigest() for name in names}


def kernel_manifest(code, tools):
    return {
        "format": "framenpu-kernel",
        "abi": JOB_ABI,
        "target": "hexagonv75",
        "bytes": len(code),
        "sha256": hashlib.sha256(code).hexdigest(),
        "compiler_flags": list(BUILD_FLAGS),
        "toolchain": tools.versions(),
        "source_sha256": source_hashes(),
    }


def verify_kernel(package=None):
    """Check the binary and source hashes without running LLVM or hardware."""
    import json

    package = Path(package) if package is not None else SDK.parent
    manifest = json.loads((package / "data/runtime.json").read_text())
    code = (package / "data/runtime.bin").read_bytes()
    if (
        manifest.get("abi") != JOB_ABI
        or manifest.get("target") != "hexagonv75"
        or manifest.get("bytes") != len(code)
        or manifest.get("sha256") != hashlib.sha256(code).hexdigest()
    ):
        raise ValueError("bundled kernel identity does not match runtime.json")
    expected = (
        source_hashes()
        if package == SDK.parent
        else {
            name: hashlib.sha256((package / name).read_bytes()).hexdigest()
            for name in source_hashes()
        }
    )
    if manifest.get("source_sha256") != expected:
        raise ValueError("SDK/compiler sources changed; run python tools/build_kernels.py")
    if manifest.get("compiler_flags") != list(BUILD_FLAGS):
        raise ValueError("compiler flags changed; regenerate the bundled kernel")
    for name in ("dispatch.bin", "dispatch_patch.bin"):
        payload = (package / "data" / name).read_bytes()
        info = manifest.get("additional_payloads", {}).get(name, {})
        if info != {"bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}:
            raise ValueError(f"bundled {name} identity does not match runtime.json")
    return manifest
