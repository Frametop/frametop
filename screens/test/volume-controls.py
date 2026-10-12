#!/usr/bin/env python3
"""Isolated CPU geometry + actual vr.cpp adapter tests; never start SteamVR.

python3 screens/test/volume-controls.py --openvr-header /path/to/openvr.h
Outputs live under build/native-controls-cpu by default, never screens/build.
--native-deps keeps the full handcut/keyboard link for a provisioned dev container.
"""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile
import os

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--openvr-header", type=Path, default=root / "screens/build/include/openvr.h")
parser.add_argument("--output", type=Path, default=root / "build/native-controls-cpu")
parser.add_argument("--native-deps", action="store_true")
parser.add_argument("--openvr-lib", type=Path, default=Path("/opt/steamvr/bin/linuxarm64"))
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
output = args.output.resolve()
# Keep build artifacts inside this checkout's ignored build directory, including
# when the caller gives a relative path or a symlink.
if not output.is_relative_to(root / "build"):
    parser.error("--output must be inside this checkout's build directory")
output.mkdir(parents=True, exist_ok=True)
flags = ["-std=c++17", "-O0", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
         "-Wno-missing-field-initializers", "-ffunction-sections", "-fdata-sections"]
if args.sanitize:
    flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]

def run_test(name, sources, extra=()):
    exe = output / name
    subprocess.run(["g++", *flags, *map(str, sources), *extra, "-Wl,--gc-sections", "-o", str(exe)], check=True)
    with tempfile.TemporaryDirectory(prefix="np-test-", dir=os.environ.get("TMPDIR")) as runtime:
        subprocess.run([str(exe)], check=True, env={**os.environ, "XDG_RUNTIME_DIR": runtime})
    print(f"PASS {name}: exit 0", flush=True)

for source in ("passthrough", "volume-translate", "volume-rotation"):
    run_test(source + "-tests", [root / f"screens/test/{source}.cpp", root / "screens/passthrough.cpp"])

header = args.openvr_header.resolve()
if not header.is_file():
    parser.error("OpenVR header missing; pass --openvr-header with the checkout's verified runtime header")
# Copy into our isolated build tree, not the canonical native build include path.
include = output / "include"
include.mkdir(exist_ok=True)
(include / "openvr.h").write_bytes(header.read_bytes())
text = re.sub(r"/\*.*?\*/|//[^\n]*", "", header.read_text(), flags=re.S)
defaults = ["#pragma once\n#include <openvr.h>\nnamespace vr {\n"]
for name in ("IVROverlay", "IVRSystem", "IVRApplications", "IVRRenderModels"):
    interface = re.search(r"class " + name + r"\s*\{(.*?)\n\s*\};", text, re.S)
    if interface is None:
        parser.error(f"OpenVR header lacks {name}")
    body = interface.group(1)
    defaults.append("struct Default" + name + " : " + name + " {\n")
    for match in re.finditer(r"virtual\s+(.*?)\s*=\s*0\s*;", body, re.S):
        signature = match.group(1).strip()
        defaults.append(signature + " override { " + ("" if signature.startswith("void ") else "return {}; ") + "}\n")
    defaults.append("};\n")
defaults.append("}\n")
(output / "volume-openvr-defaults.h").write_text("".join(defaults))
extra = ["-isystem", str(include), "-I" + str(output)]
sources = [root / "screens/test/volume-controls.cpp", root / "screens/passthrough.cpp",
           root / "screens/native_portal.cpp"]
if args.native_deps:
    extra += ["-I" + str(header.parent)]  # bundled stb_truetype.h for keyboard.cpp
    extra += subprocess.check_output(["pkg-config", "--cflags", "--libs", "egl", "glesv2", "gbm", "libdrm"], text=True).split()
    sources += [root / "screens/handcut.cpp", root / "screens/keyboard.cpp"]
    extra += ["-L" + str(args.openvr_lib), "-lopenvr_api", "-Wl,-rpath," + str(args.openvr_lib)]
else:
    # vr.cpp only needs this standard public fourcc constant to compile. GPU paths
    # abort in volume-boundary.cpp, so no DRM/GBM/EGL headers or libraries are needed.
    stubs = output / "cpu-include"
    stubs.mkdir(exist_ok=True)
    (stubs / "drm_fourcc.h").write_text("#pragma once\n#define DRM_FORMAT_ABGR8888 0x34324241u\n")
    extra += ["-I" + str(stubs)]
    # Keep the real keyboard event pump; stub only unrelated drawing/pose work.
    keyboard = (root / "screens/keyboard.cpp").read_text()
    begin = keyboard.index("void Poll(void (*handle)")
    end = keyboard.index("\nvoid Destroy()", begin)
    (output / "keyboard-poll.inc").write_text(keyboard[begin:end])
    extra += ["-DFT_CPU_KEYBOARD"]
    sources += [root / "screens/test/volume-boundary.cpp"]
run_test("volume-controls-vr-tests", sources, extra)
if not args.native_deps:
    for mode in ("--keyboard-release", "--catcher-ownership", "--desktop-buttons"):
        subprocess.run([str(output / "volume-controls-vr-tests"), mode], check=True)
