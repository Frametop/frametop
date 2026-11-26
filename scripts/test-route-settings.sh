#!/usr/bin/env bash
# Per-route stream settings and direct-address handoff, offline: no headset needed.
# Requires a C++17 compiler and nlohmann/json, as ft-stream does in the build container.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$root/build/tests"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror "$root/stream/test/route-settings-test.cpp" \
    -o "$root/build/tests/route-settings-test"
"$root/build/tests/route-settings-test"
