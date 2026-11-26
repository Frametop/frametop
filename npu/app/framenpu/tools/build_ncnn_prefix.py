"""Build the in-process CPU-prefix bridge against the pinned device ncnn build."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
NCNN_SHA = "71acddf69b6cc44ca845d48f3624a4125ce714fc6c7bca207b9a9c8de77c8651"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--ncnn", required=True, type=Path, help="directory containing ncnn/ and build/")
    p.add_argument("--output", required=True, type=Path)
    args = p.parse_args()
    lib = args.ncnn / "build/src/libncnn.a"
    if digest(lib) != NCNN_SHA:
        p.error("ncnn library differs from the qualified CPU baseline")
    args.output.mkdir(parents=True, exist_ok=True)
    source = ROOT / "tools/ncnn_prefix_bridge.cpp"
    output = args.output / "libncnn_prefix.so"
    command = [
        "c++",
        "-O3",
        "-DNDEBUG",
        "-fPIC",
        "-shared",
        str(source),
        "-I",
        str(args.ncnn / "ncnn/src"),
        "-I",
        str(args.ncnn / "build/src"),
        str(lib),
        "-fopenmp",
        "-pthread",
        "-o",
        str(output),
    ]
    subprocess.run(command, check=True)
    record = {
        "format": "framenpu-ncnn-prefix-build",
        "source_sha256": digest(source),
        "ncnn_library_sha256": digest(lib),
        "library_sha256": digest(output),
        "compiler": subprocess.check_output(["c++", "--version"], text=True).splitlines()[0],
        "flags": ["-O3", "-DNDEBUG", "-fPIC", "-shared", "-fopenmp", "-pthread"],
    }
    (args.output / "prefix.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
