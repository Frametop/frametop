"""Compile trusted freestanding C jobs against the packaged DSP ABI."""

from dataclasses import dataclass
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

from .errors import CompilerError
from .job import Job

SDK = Path(__file__).with_name("sdk")
BUILD_FLAGS = (
    "--target=hexagon",
    "-mcpu=hexagonv75",
    "-mhvx",
    "-mhvx-length=128b",
    "-Os",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-stack-protector",
    "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables",
    "-ffp-contract=off",
    "-mllvm",
    "-spill-func-threshold=100",
    "-mllvm",
    "-spill-func-threshold-Os=100",
    "-G0",
)


@dataclass(frozen=True)
class Toolchain:
    clang: str
    linker: str
    objcopy: str

    @classmethod
    def discover(cls):
        found = []
        for variable, default in (
            ("CLANG", "clang"),
            ("LD_LLD", "ld.lld"),
            ("LLVM_OBJCOPY", "llvm-objcopy"),
        ):
            requested = os.environ.get(variable, default)
            path = shutil.which(requested)
            if path is None:
                raise CompilerError(
                    f"LLVM tool {requested!r} not found; install a Hexagon-capable LLVM "
                    f"toolchain or set {variable} to its executable path"
                )
            found.append(path)
        result = cls(*found)
        if "hexagon" not in _run([result.clang, "--print-targets"]).lower():
            raise CompilerError("selected clang has no Hexagon target; see CONTRIBUTING.md")
        return result

    def versions(self):
        return {
            name: _run([tool, "--version"]).splitlines()[0]
            for name, tool in (
                ("clang", self.clang),
                ("linker", self.linker),
                ("objcopy", self.objcopy),
            )
        }


def _run(command):
    try:
        result = subprocess.run(command, check=True, text=True, capture_output=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        detail = getattr(exc, "stderr", "") or str(exc)
        raise CompilerError(f"{Path(command[0]).name} failed: {detail.strip()}") from exc
    return result.stdout


def build_job(source, output=None, *, requires_hmx=False, build_dir=None, toolchain=None):
    """Build a fixed-address FJOB. Compilation never opens a device."""
    source = Path(source).resolve()
    if not source.is_file():
        raise CompilerError(f"kernel source does not exist: {source}")
    tools = toolchain or Toolchain.discover()
    if build_dir is None:
        with tempfile.TemporaryDirectory(prefix="framenpu-build-") as directory:
            return build_job(
                source, output, requires_hmx=requires_hmx, build_dir=directory, toolchain=tools
            )
    directory = Path(build_dir)
    directory.mkdir(parents=True, exist_ok=True)
    objects = []
    for index, src in enumerate((SDK / "entry.S", SDK / "hmx.S", source)):
        # A developer source may have the same basename as an SDK source.
        obj = directory / f"{index:02d}-{src.name}.o"
        _run(
            [tools.clang, *BUILD_FLAGS, "-I", str(SDK / "include"), "-c", str(src), "-o", str(obj)]
        )
        objects.append(str(obj))
    elf = directory / "kernel.elf"
    _run([tools.linker, "-T", str(SDK / "link.ld"), *objects, "-o", str(elf)])
    binary = directory / "kernel.bin"
    _run([tools.objcopy, "-O", "binary", "--only-section=.payload", str(elf), str(binary)])
    job = Job(binary.read_bytes(), source.stem, requires_hmx, supports_direct=True)
    if output is not None:
        Path(output).parent.mkdir(parents=True, exist_ok=True)
        job.save(output)
    return job


def build_dispatch(directory, *, toolchain=None):
    """Compile the fixed ABI 9 dispatch window, without opening a device."""
    tools = toolchain or Toolchain.discover()
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    obj, elf, binary = (directory / name for name in ("dispatch.o", "dispatch.elf", "dispatch.bin"))
    _run([tools.clang, *BUILD_FLAGS, "-c", str(SDK / "dispatch.S"), "-o", str(obj)])
    _run([tools.linker, "-T", str(SDK / "dispatch.ld"), str(obj), "-o", str(elf)])
    _run([tools.objcopy, "-O", "binary", "--only-section=.text", str(elf), str(binary)])
    from .profile import PROFILE

    code = binary.read_bytes()
    if len(code) != PROFILE.dispatch_size:
        raise CompilerError("dispatcher does not fit the verified firmware window")
    return code
