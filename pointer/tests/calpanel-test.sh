#!/usr/bin/env bash
# Offline test of the gaze calibration panel's answers (pointer/tests/calpanel-test.cpp): builds
# it in the dev container and runs it there. Nothing reaches SteamVR, so it's safe next to it.
#
#   pointer/tests/calpanel-test.sh
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C pointer 'set -e; mkdir -p tests/build
g++ -std=c++17 -O2 -Wall -o tests/build/calpanel-test tests/calpanel-test.cpp
tests/build/calpanel-test'
