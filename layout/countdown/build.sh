#!/usr/bin/env bash
# Build the short-lived reset countdown in the Frame's dev container, or inside
# the release image (FRAME_IN_BOX=1). Output stays in layout/countdown/build/.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C layout/countdown 'set -e
./deps.sh
. ../../scripts/openvr.sh
g++ -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers $OPENVR_CFLAGS \
  -o build/ft-reset-timer ft-reset-timer.cpp $OPENVR_LIBS -pthread
strip --strip-unneeded build/ft-reset-timer
build/ft-reset-timer --check
echo "built build/ft-reset-timer"'
