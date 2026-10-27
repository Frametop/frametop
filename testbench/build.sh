#!/usr/bin/env bash
# Build the testbench's native parts on the Frame (dev container): the input proxy and
# loader SteamVR preloads, and frame-observe. All three run on the SteamOS host, so like
# the ft_pointer driver they may only need glibc symbols the host has (2.39).
# Usage: testbench/build.sh
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C testbench 'set -e
bash scripts/build-input.sh
sh scripts/build-observe.sh
for f in build/libframe_input.so build/libframe_loader.so build/frame-observe; do
  max=$(objdump -T "$f" | grep -oE "GLIBC_[0-9.]+" | sort -uV | tail -1)
  [ -n "$max" ] || { echo "$f: objdump found no glibc symbols" >&2; exit 1; }
  echo "$f: newest glibc symbol $max"
  [ "$(printf "%s\n" "$max" GLIBC_2.39 | sort -V | tail -1)" = GLIBC_2.39 ] || { echo "$f needs newer glibc than the host has" >&2; exit 1; }
done'
