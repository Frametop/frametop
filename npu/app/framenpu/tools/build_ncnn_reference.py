"""Build a strict CPU oracle from FrameTop's hash-pinned external ncnn archive."""

import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REVISION = "e54f7b1f88434e1d844ea0551b880a1cfb079ce1"
ARCHIVE_SHA = "754659d6fe65545cf2ef4483ffb84526fea631f8764c44b150f1601d0fb4004b"
URL = "https://github.com/Tencent/ncnn/releases/download/20260526/ncnn-20260526-full-source.zip"
OPTIONS = [
    "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
    "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=-O2 -DNDEBUG -ffp-contract=off -fno-fast-math",
    "-DCMAKE_INSTALL_LIBDIR=lib",
    "-DNCNN_VULKAN=OFF",
    "-DNCNN_OPENMP=OFF",
    "-DNCNN_INT8=OFF",
    "-DNCNN_BUILD_TOOLS=OFF",
    "-DNCNN_BUILD_EXAMPLES=OFF",
    "-DNCNN_BUILD_BENCHMARK=OFF",
    "-DNCNN_BUILD_TESTS=OFF",
    "-DNCNN_RUNTIME_CPU=OFF",
    "-DNCNN_AVX=OFF",
    "-DNCNN_AVX2=OFF",
    "-DNCNN_AVX512=OFF",
    "-DNCNN_SSE2=OFF",
]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args(argv)
    if not 1 <= args.jobs <= 32:
        parser.error("jobs must be 1..32")
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    archive = work / "ncnn-20260526-full-source.zip"
    if not archive.exists():
        with urllib.request.urlopen(URL) as response, archive.open("wb") as output:
            while chunk := response.read(1024 * 1024):
                output.write(chunk)
    if digest(archive) != ARCHIVE_SHA:
        parser.error("ncnn archive SHA-256 differs")
    source = work / "ncnn"
    if not source.exists():
        with zipfile.ZipFile(archive) as stream:
            if any(Path(n).is_absolute() or ".." in Path(n).parts for n in stream.namelist()):
                parser.error("invalid archive path")
            stream.extractall(source)
    # Existing source files must still equal the pinned archive; a cached build
    # directory never substitutes for source provenance.
    with zipfile.ZipFile(archive) as stream:
        for entry in stream.infolist():
            if not entry.is_dir() and hashlib.sha256(stream.read(entry)).hexdigest() != digest(
                source / entry.filename
            ):
                parser.error("cached ncnn source differs from the pinned archive")
    build = work / "build"
    install = work / "install"
    with (work / "build-oracle.log").open("w") as log:
        subprocess.run(
            [
                "cmake",
                "-S",
                str(source),
                "-B",
                str(build),
                "-G",
                "Ninja",
                f"-DCMAKE_INSTALL_PREFIX={install}",
                *OPTIONS,
            ],
            check=True,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        subprocess.run(
            ["cmake", "--build", str(build), "--target", "install", "-j", str(args.jobs)],
            check=True,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
    record = {
        "ncnn_revision": REVISION,
        "source_url": URL,
        "archive_sha256": ARCHIVE_SHA,
        "build_options": OPTIONS,
        "architecture": platform.machine(),
        "compiler": subprocess.check_output(["g++", "--version"], text=True).splitlines()[0],
        "library_sha256": digest(install / "lib/libncnn.a"),
        "tools": {},
    }
    for name in ("ncnn_reference", "ncnn_layer_reference"):
        source = ROOT / "tools" / (name + ".cpp")
        output = work / name
        subprocess.run(
            [
                "g++",
                "-std=c++17",
                "-O2",
                "-ffp-contract=off",
                f"-I{install / 'include/ncnn'}",
                str(source),
                str(install / "lib/libncnn.a"),
                "-lpthread",
                "-o",
                str(output),
            ],
            check=True,
        )
        record["tools"][name] = {
            "source_sha256": digest(source),
            "executable_sha256": digest(output),
        }
    (work / "reference_build.json").write_text(json.dumps(record, indent=2) + "\n")
    print("Built pinned strict ncnn CPU reference and layer oracle")


if __name__ == "__main__":
    main()
