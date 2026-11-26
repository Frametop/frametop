"""Verify an sdist rebuild and a real wheel installation, without hardware."""

import argparse
from email.parser import BytesParser
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import zipfile


def run(command, *, cwd, env):
    result = subprocess.run(
        [str(x) for x in command], cwd=cwd, env=env, text=True, capture_output=True
    )
    if result.returncode:
        raise RuntimeError(f"distribution command failed:\n{result.stdout}\n{result.stderr}")
    return result.stdout


def unpack_source(archive, destination):
    with tarfile.open(archive) as stream:
        for member in stream.getmembers():
            path = destination / member.name
            if not path.is_relative_to(destination) or ".." in Path(member.name).parts:
                raise ValueError("invalid source archive path")
            if member.isdir():
                path.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(stream.extractfile(member).read())
            else:
                raise ValueError("source archives must not contain links or special files")
    roots = list(destination.iterdir())
    if len(roots) != 1:
        raise ValueError("expected one source archive root")
    return roots[0]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dist", type=Path, default=Path("dist"))
    args = parser.parse_args(argv)
    archives = sorted(args.dist.resolve().glob("*.tar.gz"))
    wheels = sorted(args.dist.resolve().glob("*.whl"))
    if len(archives) != 1 or len(wheels) != 1:
        parser.error("dist must contain exactly one wheel and one source archive")
    with zipfile.ZipFile(wheels[0]) as wheel:
        names = wheel.namelist()
        metadata = BytesParser().parsebytes(
            wheel.read(next(n for n in names if n.endswith(".dist-info/METADATA")))
        )
        if metadata.get("License-Expression") != "MIT":
            raise ValueError("wheel license metadata is missing")
        for suffix in (
            "sdk/entry.S",
            "sdk/hmx.S",
            "sdk/runtime.c",
            "sdk/link.ld",
            "sdk/include/framenpu.h",
            "sdk/include/fnpu_readback.h",
            "sdk/dispatch.S",
            "sdk/dispatch.ld",
            "sdk/dispatch_patch.c",
            "data/dispatch.bin",
            "data/dispatch_patch.bin",
            "data/runtime.bin",
            "data/runtime.json",
            "resident.py",
            "resident_gemm.py",
            "convolution.py",
            "landmark.py",
            "resident_landmark.py",
        ):
            if f"framenpu/{suffix}" not in names:
                raise ValueError(f"wheel is missing SDK/payload resource: {suffix}")
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", PYTHONNOUSERSITE="1")
    env.pop("PYTHONPATH", None)
    with tempfile.TemporaryDirectory(prefix="framenpu-release-") as temporary:
        root = Path(temporary)
        source = unpack_source(archives[0], root / "source")
        run([sys.executable, "tools/check_repository.py"], cwd=source, env=env)
        run(
            [
                sys.executable,
                "-m",
                "build",
                "--wheel",
                "--no-isolation",
                "--outdir",
                root / "wheel",
            ],
            cwd=source,
            env=env,
        )
        rebuilt = next((root / "wheel").glob("*.whl"))
        run([sys.executable, "-m", "venv", root / "venv"], cwd=root, env=env)
        bindir = root / "venv" / ("Scripts" if os.name == "nt" else "bin")
        python = bindir / ("python.exe" if os.name == "nt" else "python")
        cli = bindir / ("framenpu.exe" if os.name == "nt" else "framenpu")
        work = root / "unrelated-working-directory"
        work.mkdir()
        run([python, "-m", "pip", "install", "--no-index", "--no-deps", rebuilt], cwd=work, env=env)
        version = run([cli, "--version"], cwd=work, env=env).strip()
        run(
            [
                python,
                "-c",
                "import framenpu.convolution, framenpu.landmark, framenpu.resident_landmark; from framenpu.mercury_runtime import MercurySession; from framenpu.provenance import verify_kernel; verify_kernel(); import sys; assert not any(n in sys.modules for n in ('mercury_layout', 'teacher_resident_layout', 'teacher_tail_layout', 'hmx_block_layout'))",
            ],
            cwd=work,
            env=env,
        )
        created = work / "generated.fnpu"
        run([python, source / "examples/make_xor.py", created], cwd=work, env=env)
        plan = json.loads(
            run([cli, "inspect-model", created, "--batch-size", "4"], cwd=work, env=env)
        )
        if plan["name"] != "xor" or len(plan["plan"]) != 2:
            raise ValueError("installed-wheel model example failed")
        inventory = json.loads(
            run([cli, "inspect-ncnn", source / "examples/dense.param"], cwd=work, env=env)
        )
        if inventory["layers"] != 2 or inventory["native_ncnn_loading"] is not False:
            raise ValueError("installed-wheel ncnn inspection failed")
        resident = json.loads(
            run(
                [cli, "plan-resident", source / "examples/resident_residual.json"],
                cwd=work,
                env=env,
            )
        )
        if resident["execution_supported"] is not False or resident["required_bytes"] != 187392:
            raise ValueError("installed-wheel resident storage planning failed")
        run(
            [
                python,
                "-c",
                "from framenpu.resident_gemm import proof_layout; assert proof_layout()['required_bytes'] == 32768",
            ],
            cwd=work,
            env=env,
        )
        for example in ("run_custom.py", "run_xor.py"):
            run([python, source / "examples" / example, "--help"], cwd=work, env=env)
        print(
            f"Distribution checks passed: {version}; sdist rebuild, isolated wheel installation, "
            "CLI, model, resident planner, SDK resources and examples from an unrelated directory"
        )


if __name__ == "__main__":
    main()
