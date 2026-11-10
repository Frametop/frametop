#!/usr/bin/env bash
# Build ft-model-import (avatar/import, avatar/build/ft-model-import) in the dev container on
# the Frame: it turns a VRM, glTF or FBX model into Frametop's own model file (modelfile.h).
# The formats are read with cgltf and ufbx, and a mesh is reduced with meshoptimizer's
# simplifier: each at a pinned commit and checked by sha256 (scripts/modeldeps.sh). A VRM's
# own fields (its bones, its licence, its expressions) are JSON, read with jsoncpp. A model's
# pictures are decoded with stb_image (scripts/stb.sh, pinned the same way).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C avatar 'set -e; mkdir -p build/deps
. ../scripts/modeldeps.sh
. ../scripts/stb.sh
modeldeps_fetch build/deps
stb_fetch build/deps stb_image.h
cxx="g++ -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers"
# the libraries: built once (they are pinned), without the warnings our own code is held to
[ build/ufbx.o -nt build/deps/ufbx.c ] || gcc -std=c11 -O2 -w -c -o build/ufbx.o build/deps/ufbx.c
[ build/simplifier.o -nt build/deps/simplifier.cpp ] || g++ -std=c++17 -O2 -w -c -o build/simplifier.o build/deps/simplifier.cpp
[ build/allocator.o -nt build/deps/allocator.cpp ] || g++ -std=c++17 -O2 -w -c -o build/allocator.o build/deps/allocator.cpp
[ build/image.o -nt import/image.cpp ] && [ build/image.o -nt build/deps/stb_image.h ] && [ build/image.o -nt import/scene.h ] && [ build/image.o -nt vec.h ] ||
  $cxx -w -isystem build/deps -c -o build/image.o import/image.cpp
$cxx -Wno-unused-function -isystem build/deps $(pkg-config --cflags jsoncpp) -c -o build/gltfscene.o import/gltfscene.cpp
$cxx -isystem build/deps $(pkg-config --cflags jsoncpp) -c -o build/gltfvrm.o import/gltfvrm.cpp
$cxx -isystem build/deps -c -o build/gltflook.o import/gltflook.cpp
$cxx -isystem build/deps -c -o build/fbxscene.o import/fbxscene.cpp
$cxx -isystem build/deps -c -o build/fbxlook.o import/fbxlook.cpp
$cxx -isystem build/deps -c -o build/reduce.o import/reduce.cpp
$cxx -o build/ft-model-import import/main.cpp import/handbones.cpp import/handpart.cpp import/bodybones.cpp import/bodypart.cpp \
  import/bodylook.cpp import/shapenames.cpp modelfile.cpp bodyfile.cpp \
  build/gltfscene.o build/gltfvrm.o build/gltflook.o build/fbxscene.o build/fbxlook.o build/image.o build/reduce.o \
  build/ufbx.o build/simplifier.o build/allocator.o $(pkg-config --libs jsoncpp) -lm
strip --strip-unneeded build/ft-model-import
echo "built build/ft-model-import"'
