#!/usr/bin/env bash
# Builds what ft-npud, NPU hand tracking's root service, runs from: build/tree, laid out as
# /var/lib/ft-npud is once npu/install.sh has put it in place (README.md):
#   app/      the daemon (app/, vendored), with its four C helpers built beside their sources
#   python/   a standalone CPython (python-build-standalone) with numpy, both checked by sha256: the
#             daemon doesn't use the host's python3, so a SteamOS update that moves it (3.12 to 3.14 on
#             0.5) can't break the service
#   graphs/   landmarks.npz, the hand model it serves (graphs/hdq.npz, checked by sha256)
#   ft-npud.service  the unit, which install.sh fills in and installs
# In the dev container (the image build runs it in the image: FRAME_IN_BOX=1). Downloads stay in
# build/dl, so a rebuild fetches nothing. The new tree replaces build/tree only once it's complete.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C npu 'set -euo pipefail
PY_TAG=20261026 PY_VER=3.14.10
PY_TGZ=cpython-$PY_VER+$PY_TAG-aarch64-unknown-linux-gnu-install_only_stripped.tar.gz
PY_SHA=90f48337bc3dd4ae6eda81e001e4c981236d403b6b5a07778bbdf94ee423dc00
NP_WHL=numpy-2.5.5-cp314-cp314-manylinux_2_27_aarch64.manylinux_2_28_aarch64.whl
NP_URL=https://files.pythonhosted.org/packages/26/5b/5cd4b88a64bb57ca6d3bd6a9e057db445544a2179a2ebf985660e0454648/$NP_WHL
NP_SHA=cc6eb7502facce90349b4097a26821af224aaa0ab36e78304711cdb544352882
GRAPH_SHA=4625773b31264e92a4ee5c4ea2ca72eb62991c534171f64e395b6ecec58619b7
fetch() {   # URL FILE SHA256: into build/dl, once
  [ -f "build/dl/$2" ] && echo "$3  build/dl/$2" | sha256sum -c --status && return
  curl -fsSL --retry 3 -o "build/dl/$2.part" "$1"
  echo "$3  build/dl/$2.part" | sha256sum -c --quiet
  mv "build/dl/$2.part" "build/dl/$2"
}
mkdir -p build/dl
fetch "https://github.com/astral-sh/python-build-standalone/releases/download/$PY_TAG/$PY_TGZ" "$PY_TGZ" "$PY_SHA"
fetch "$NP_URL" "$NP_WHL" "$NP_SHA"
echo "$GRAPH_SHA  graphs/hdq.npz" | sha256sum -c --quiet
new=build/tree.new
rm -rf "$new"
mkdir -p "$new/graphs" "$new/app/work"
tar -C app -cf - --exclude=__pycache__ --exclude="*.c" . | tar -C "$new/app" -xf -
# but the C sources of the DSP kernel: framenpu checks the kernel it ships against their hashes (provenance.py)
cp app/framenpu/src/framenpu/sdk/*.c "$new/app/framenpu/src/framenpu/sdk/"
# The C helpers, for any aarch64 the Frame has (not -march=native: the image is built on another machine)
cc() { gcc -shared -fPIC -Wall -o "$@"; }
cc "$new/app/npud/deguard.so" -O2 app/npud/deguard.c
cc "$new/app/npud/ticks.so" -O2 app/npud/ticks.c
cc "$new/app/npud/v30a_hostc.so" -O2 app/npud/v30a_hostc.c
cc "$new/app/work/hostc.so" -O3 -ffast-math -march=armv8.2-a app/probe/hostc.c -lm
# they run on the host, against its glibc: none may need a symbol newer than 2.34 (far older than any SteamOS release)
for so in "$new"/app/npud/*.so "$new"/app/work/hostc.so; do
  v=$(objdump -T "$so" | grep -o "GLIBC_[0-9.]*" | sort -V | tail -1)
  [ -z "$v" ] || [ "$(printf "%s\nGLIBC_2.34\n" "$v" | sort -V | tail -1)" = GLIBC_2.34 ] ||
    { echo "npu/build.sh: $so needs $v" >&2; exit 1; }
done
tar -C "$new" -xzf "build/dl/$PY_TGZ"   # python/
py=$new/python/bin/python3
pyv=$("$py" -I -c "import sys; print(\"python%d.%d\" % sys.version_info[:2])")
site=$new/python/lib/$pyv/site-packages
# what the daemon never imports: the test suite, Tk, IDLE, pip, headers (about 40 MB)
rm -rf "$new/python/lib/$pyv/test" "$new/python/lib/$pyv/idlelib" "$new/python/lib/$pyv/tkinter" \
  "$new/python/lib/$pyv/turtledemo" "$new/python/lib/$pyv/ensurepip" "$new/python/include" "$new/python/share" \
  "$new"/python/lib/libtcl* "$new"/python/lib/libtk* "$new"/python/lib/tcl* "$new"/python/lib/tk* "$site"/pip*
"$py" -I -B -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "build/dl/$NP_WHL" "$site"
"$py" -I -B -c "import numpy; assert numpy.__version__ == \"2.5.5\", numpy.__version__"
find "$new" -depth -name __pycache__ -exec rm -rf {} +
cp graphs/hdq.npz "$new/graphs/landmarks.npz"
cp ft-npud.service "$new/ft-npud.service"
chmod -R u=rwX,go=rX "$new"
# the tree, run by its own Python as the daemon runs it: its memory seals (the fcntl of that CPython has no
# seal names: transport.py), and the kernel checks out against its sources (dispatch.py does this at each start)
"$py" -I -B -c "import sys; sys.path[:0] = sys.argv[1:]
import transport; transport.Memory().map.close()
from framenpu.provenance import verify_kernel; verify_kernel()" "$new/app/npud" "$new/app/framenpu/src" ||
  { echo "npu/build.sh: the tree fails its own Python (above)" >&2; exit 1; }
rm -rf build/tree.old
[ ! -d build/tree ] || mv build/tree build/tree.old
mv "$new" build/tree
rm -rf build/tree.old
echo "built build/tree ($(du -sm build/tree | cut -f1) MB: ft-npud, CPython $PY_VER, numpy 2.5.5, hdq)"'
