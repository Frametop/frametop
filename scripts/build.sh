#!/usr/bin/env bash
# Native ARM64 build. Run on a Linux ARM64 machine with the dependencies in README.md.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
[ "$(uname -m)" = aarch64 ] || { echo 'FrameHands builds only on aarch64' >&2; exit 1; }
"$root/hands/models.sh" fetch
make -C "$root/hands" check
make -C "$root/hands" all
bash "$root/renderer/build.sh"
bash "$root/pointer/build.sh"
