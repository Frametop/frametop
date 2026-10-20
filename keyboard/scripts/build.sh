#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$project_dir"
preset=${1:-host}
case "$preset" in
    host) ;;
    frame-arm64)
        if [[ -z ${FRAMEKEYBOARD_SYSROOT:-} || ! -d $FRAMEKEYBOARD_SYSROOT/usr/include ]]; then
            echo 'Set FRAMEKEYBOARD_SYSROOT to your Frame sysroot. See docs/building.md.' >&2
            exit 1
        fi
        ;;
    *) echo 'Usage: scripts/build.sh [host|frame-arm64]' >&2; exit 2 ;;
esac
cmake --preset "$preset"
cmake --build --preset "$preset"
file "build/$preset/framekeyboard"
if [[ "$preset" == frame-arm64 ]]; then
    readelf -h "build/$preset/framekeyboard" | grep -q 'Machine:.*AArch64' || {
        echo 'Expected an AArch64 deployment artifact.' >&2; exit 1;
    }
fi
