#!/usr/bin/env bash
# Fetch the two STB headers used by the countdown renderer, pinned and checked by
# scripts/stb.sh (the commit and sha256s the other builds use).
# Also usable on a development PC for the fake-OpenVR tests; no Frame or VR access.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
include="$here/build/include"
. "$here/../../scripts/stb.sh"
stb_fetch "$include" stb_truetype.h stb_image_write.h
