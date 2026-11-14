# Sourced by avatar/build.sh and the tests: the libraries ft-model-import reads models with, each
# file at one pinned commit and checked by its sha256 (hashed from that commit's files):
#   cgltf          glTF, GLB and VRM (MIT, one header)                       v1.15
#   ufbx           FBX (MIT or public domain, one source file)               v0.23.1
#   meshoptimizer  reducing a mesh (MIT; only its simplifier is built)       v1.3
#   modeldeps_fetch DIR   fetches each file into DIR once; a copy that doesn't match is fetched
#                         again, and a download that doesn't match fails.
modeldeps_files() {
  local cgltf=https://raw.githubusercontent.com/jkuhlmann/cgltf/360db1a95480fe102ae9c69b27c5d101167ff5ba
  local ufbx=https://raw.githubusercontent.com/ufbx/ufbx/26a482ae66871d7de36eb722aa060bce95bce274
  local meshopt=https://raw.githubusercontent.com/zeux/meshoptimizer/9e1f07b159d3cb777f1c67ed31fc11fd117986f4/src
  cat <<EOF
cgltf.h $cgltf/cgltf.h e378a21c084bf1f288bb799de827bb26906efb024255f1ecf1705ea13f11c6ec
ufbx.h $ufbx/ufbx.h f787be529af577efc04f3e3e735844a08556718dbb613d1211c2b313d72895dc
ufbx.c $ufbx/ufbx.c 4a956c26a708e40d82ecb0aeaee54da709d5ba7c144c9642174970059bda7e1d
meshoptimizer.h $meshopt/meshoptimizer.h e3688ef553f603391747895a5c221ef1528574cc7f8173782417a96cb531f61b
simplifier.cpp $meshopt/simplifier.cpp 719303b6970398631e30724e8eebc6cd2e7e08b67fe8ab7cbf7eecafdaa08616
allocator.cpp $meshopt/allocator.cpp d2cc48691fe2f4c6d097bf7a766389cfb7f83ca14a5f747e3944332656d02254
EOF
}

modeldeps_digest() {
  local d
  if command -v sha256sum >/dev/null 2>&1; then d=$(sha256sum "$1"); else d=$(shasum -a 256 "$1"); fi
  echo "${d%% *}"
}

modeldeps_fetch() {
  local dir=$1 name url wanted
  mkdir -p "$dir"
  while read -r name url wanted; do
    [ -f "$dir/$name" ] && [ "$(modeldeps_digest "$dir/$name")" = "$wanted" ] && continue
    curl -fsSL "$url" -o "$dir/$name.part" || return
    if [ "$(modeldeps_digest "$dir/$name.part")" != "$wanted" ]; then
      echo "$name doesn't match its pinned sha256" >&2
      rm -f "$dir/$name.part"
      return 1
    fi
    mv "$dir/$name.part" "$dir/$name"
  done < <(modeldeps_files)
}
