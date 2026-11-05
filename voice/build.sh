#!/usr/bin/env bash
# Build Frametop Voice on the Frame, in the dev container:
#   build/bin/whisper-server   whisper.cpp's server on the GPU (Vulkan), for wake word commands,
#                              from whisper.cpp at a fixed commit, linked statically. It needs
#                              glslc and the SPIR-V headers (setup/dev-container.sh, pack/Containerfile).
#   build/venv                 Python with sherpa-onnx, sentencepiece and numpy (requirements.txt)
#                              for ft-voiced, remade when requirements.txt changes. In the image
#                              (pack/Containerfile) they're in its locked venv already (uv.lock's
#                              voice group, the same versions), so build/venv only points at it.
# The models aren't built: voice/models.sh fetches them on install.
# Usage: voice/build.sh
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C voice 'set -e; mkdir -p build/bin
commit=d09f61a708f3487afa956ff578e60eae5e7a233c  # whisper.cpp master, 2026-09-24 (frame-voice used it)
src=build/whisper.cpp
if [ "$(git -C $src rev-parse HEAD 2>/dev/null)" != $commit ]; then
  rm -rf $src
  git init -q $src
  git -C $src fetch -q --depth 1 https://github.com/ggml-org/whisper.cpp.git $commit
  git -C $src checkout -q FETCH_HEAD
fi
# For the Frame: Armv9.2 with SVE2, i8mm, dot product and fp16 (Cortex-X925 and A720).
cmake -S $src -B $src/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DGGML_VULKAN=ON -DGGML_NATIVE=OFF -DGGML_CPU_ARM_ARCH=armv9.2-a+dotprod+i8mm+sve2+fp16 \
  -DCMAKE_C_FLAGS=-mtune=cortex-a720 -DCMAKE_CXX_FLAGS=-mtune=cortex-a720 \
  -DWHISPER_BUILD_TESTS=OFF -DWHISPER_SDL2=OFF >/dev/null
cmake --build $src/build --target whisper-server
install -m 755 $src/build/bin/whisper-server build/bin/whisper-server
echo "built build/bin/whisper-server (whisper.cpp ${commit:0:7})"
if [ "${FRAME_IN_BOX:-0}" = 1 ] && [ -x /opt/frametop/venv/bin/python ]; then
  rm -rf build/venv
  python3 -m venv --without-pip build/venv
  /opt/frametop/venv/bin/python -c "import sysconfig; print(sysconfig.get_path(\"purelib\"))" \
    >"$(build/venv/bin/python -c "import sysconfig; print(sysconfig.get_path(\"purelib\"))")/frametop-image.pth"
elif ! cmp -s requirements.txt build/venv/requirements.done; then
  rm -rf build/venv
  python3 -m venv build/venv
  build/venv/bin/pip install -q --disable-pip-version-check -r requirements.txt
  cp requirements.txt build/venv/requirements.done
fi
v=$(build/venv/bin/python -c "import numpy, sentencepiece, sherpa_onnx; print(\"sherpa-onnx\", sherpa_onnx.__version__, \"sentencepiece\", sentencepiece.__version__, \"numpy\", numpy.__version__)")
echo "build/venv: $v"'
