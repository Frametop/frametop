#!/usr/bin/env bash
# Near hand input and the hand matte, offline (screens/tests/handinput_test.cpp): the unit tests,
# then each publish dump given (ft-handreplay --publish-dump: recorded hands as ft-hands
# published them) against panels placed around the hands.
#   scripts/test-handinput.sh [DUMP...]
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
dumps=$(printf ' %q' "$@")
"$root/scripts/frame.sh" -C screens "mkdir -p build && g++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror \
  tests/handinput_test.cpp handcut.cpp handinput.cpp palmmenu.cpp grabthat.cpp steampanels.cpp -o build/handinput-test && build/handinput-test${dumps:+ && build/handinput-test --replay$dumps}"
