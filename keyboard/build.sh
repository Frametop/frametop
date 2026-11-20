#!/usr/bin/env bash
# Put Full Keyboard (TaiKeid's, unmodified: FRAMETOP.md) in keyboard/build/release: the newest stable
# release, checked against its SHA256SUMS, or FULLKB_VERSION=x.y.z for an exact one. There is
# nothing to compile: Frametop runs the release's own program in hosted mode (keyboard/host).
# Runs on the host or in the box; it only needs python3 and the network.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
exec python3 -I "$root/keyboard/fetch.py" "$@"
