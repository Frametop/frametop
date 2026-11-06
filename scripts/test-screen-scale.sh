#!/usr/bin/env bash
# Screen configure dimensions, offline: no wlroots or headset needed.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$root/build/tests"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror "$root/screens/tests/screen-scale-test.c" \
    -lm -o "$root/build/tests/screen-scale-test"
"$root/build/tests/screen-scale-test"
