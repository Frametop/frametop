"""Rebuild SDK payloads, or verify them without modifying checked-in output."""

import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu import Job
from framenpu.compiler import SDK, Toolchain, build_job, build_dispatch
from framenpu.provenance import kernel_manifest, verify_kernel


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--hmx", action="store_true")
    parser.add_argument(
        "--check", action="store_true", help="recompile and compare outputs without rewriting them"
    )
    args = parser.parse_args(argv)
    if args.source:
        if not args.output or args.check:
            parser.error("--source requires --output and cannot be used with --check")
        job = build_job(args.source, args.output, requires_hmx=args.hmx)
        print(f"{job.name}: {len(job.code)} bytes -> {args.output}")
        return
    if args.output or args.hmx:
        parser.error("--output and --hmx require --source")
    tools = Toolchain.discover()
    with tempfile.TemporaryDirectory(prefix="framenpu-kernels-") as directory:
        runtime = build_job(
            SDK / "runtime.c",
            requires_hmx=True,
            build_dir=Path(directory) / "runtime",
            toolchain=tools,
        )
        custom = build_job(
            ROOT / "examples/vector_add.c", build_dir=Path(directory) / "custom", toolchain=tools
        )
        dispatch = build_dispatch(Path(directory) / "dispatch", toolchain=tools)
        patcher = build_job(
            SDK / "dispatch_patch.c", build_dir=Path(directory) / "patcher", toolchain=tools
        )
    extra = {"dispatch.bin": dispatch, "dispatch_patch.bin": patcher.code}
    data = ROOT / "src/framenpu/data"
    if args.check:
        manifest = verify_kernel()
        if runtime.code != (data / "runtime.bin").read_bytes():
            parser.exit(
                1, "runtime differs; use the recorded LLVM toolchain or rebuild and validate\n"
            )
        if custom != Job.load(ROOT / "examples/vector_add.fjob"):
            parser.exit(1, "custom example differs; regenerate and validate it\n")
        for name, code in extra.items():
            if (data / name).read_bytes() != code:
                parser.exit(1, f"{name} differs; regenerate and validate it\n")
        print(f"Verified runtime ({manifest['bytes']} bytes) and custom example")
    else:
        data.mkdir(parents=True, exist_ok=True)
        (data / "runtime.bin").write_bytes(runtime.code)
        manifest = kernel_manifest(runtime.code, tools)
        manifest["supports_direct"] = True
        manifest["additional_payloads"] = {
            name: {"bytes": len(code), "sha256": hashlib.sha256(code).hexdigest()}
            for name, code in extra.items()
        }
        for name, code in extra.items():
            (data / name).write_bytes(code)
        (data / "runtime.json").write_text(json.dumps(manifest, indent=2) + "\n")
        custom.save(ROOT / "examples/vector_add.fjob")
        print(
            f"Built runtime ({len(runtime.code)} bytes) and custom example ({len(custom.code)} bytes)"
        )


if __name__ == "__main__":
    main()
