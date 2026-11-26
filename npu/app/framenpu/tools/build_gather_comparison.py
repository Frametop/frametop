"""Build identical probes with the qualified baseline and current gather helpers."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
from framenpu.compiler import BUILD_FLAGS, Toolchain, build_job

BASELINE = "95960a2049e4b438a24893fc99639191ffdbd443"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    toolchain = Toolchain.discover()
    record = {
        "baseline_revision": BASELINE,
        "toolchain": toolchain.versions(),
        "flags": BUILD_FLAGS,
    }
    for name in ("baseline", "optimized"):
        folder = args.output / (name + "-source")
        folder.mkdir(parents=True, exist_ok=True)
        sources = {}
        for file in (
            "resident_gather_bounds.c",
            "resident_graph_gather.h",
            "resident_graph.h",
            "landmark_common.h",
        ):
            data = (ROOT / "tests" / file).read_bytes()
            if name == "baseline" and file != "resident_gather_bounds.c":
                original = subprocess.check_output(
                    ["git", "show", f"{BASELINE}:tests/{file}"], cwd=ROOT
                )
                if file != "resident_graph_gather.h" and original != data:
                    raise ValueError("comparison must change only the gather helper")
                data = original
            (folder / file).write_bytes(data)
            sources[file] = hashlib.sha256(data).hexdigest()
        job = build_job(
            folder / "resident_gather_bounds.c",
            args.output / (name + ".fjob"),
            requires_hmx=True,
            build_dir=args.output / (name + "-build"),
            toolchain=toolchain,
        )
        record[name] = {
            "bytes": len(job.code),
            "sha256": hashlib.sha256(job.code).hexdigest(),
            "sources": sources,
        }
    (args.output / "comparison.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
