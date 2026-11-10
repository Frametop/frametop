#!/usr/bin/env bash
# Build ft-model-import (avatar/import, avatar/build/ft-model-import) in the dev container on
# the Frame: it turns a VRM, glTF or FBX model into Frametop's own model file (modelfile.h).
# The formats are read with cgltf and ufbx, and a mesh is reduced with meshoptimizer's
# simplifier: each at a pinned commit and checked by sha256 (scripts/modeldeps.sh). A VRM's
# own fields (its bones, its licence) are JSON, read with jsoncpp.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C avatar 'set -e; mkdir -p build/deps
. ../scripts/modeldeps.sh
modeldeps_fetch build/deps
cxx="g++ -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers"
# the libraries: built once (they are pinned), without the warnings our own code is held to
[ build/ufbx.o -nt build/deps/ufbx.c ] || gcc -std=c11 -O2 -w -c -o build/ufbx.o build/deps/ufbx.c
[ build/simplifier.o -nt build/deps/simplifier.cpp ] || g++ -std=c++17 -O2 -w -c -o build/simplifier.o build/deps/simplifier.cpp
[ build/allocator.o -nt build/deps/allocator.cpp ] || g++ -std=c++17 -O2 -w -c -o build/allocator.o build/deps/allocator.cpp
[ build/gltfscene.o -nt import/gltfscene.cpp ] && [ build/gltfscene.o -nt build/deps/cgltf.h ] && [ build/gltfscene.o -nt import/scene.h ] ||
  $cxx -Wno-unused-function -isystem build/deps $(pkg-config --cflags jsoncpp) -c -o build/gltfscene.o import/gltfscene.cpp
$cxx -isystem build/deps -c -o build/fbxscene.o import/fbxscene.cpp
$cxx -isystem build/deps -c -o build/reduce.o import/reduce.cpp
$cxx -o build/ft-model-import import/main.cpp import/handbones.cpp import/handpart.cpp modelfile.cpp \
  build/gltfscene.o build/fbxscene.o build/reduce.o build/ufbx.o build/simplifier.o build/allocator.o \
  $(pkg-config --libs jsoncpp) -lm
strip --strip-unneeded build/ft-model-import
echo "built build/ft-model-import"'
