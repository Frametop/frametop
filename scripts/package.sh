#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/dist}
mkdir -p "$out"
out=$(cd "$out" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/hands/build" "$tmp/hands/camd" "$tmp/hands/models" "$tmp/renderer/build" "$tmp/renderer/assets" "$tmp/config" "$tmp/services"
cp "$root/hands/build/fh-camd" "$root/hands/build/fh-hands" "$tmp/hands/build/"
cp "$root/hands/camd/LICENSE.FrameEyeCameraFeed" "$tmp/hands/camd/"
cp -a "$root/hands/models/ncnn" "$root/hands/models/student" "$tmp/hands/models/"
cp "$root/hands/models/NOTICE" "$tmp/hands/models/"
cp "$root/hands/models.pin" "$tmp/hands/"
cp "$root/renderer/build/framehands-view" "$tmp/renderer/build/"
cp -a "$root/renderer/assets/hands" "$tmp/renderer/assets/"
cp "$root/config/default.conf" "$tmp/config/"
cp "$root/services/"*.service "$tmp/services/"
cp "$root/framehands" "$root/install.sh" "$root/uninstall.sh" "$root/README.md" "$root/LICENSE" "$tmp/"
tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
    -C "$tmp" -czf "$out/framehands-aarch64.tar.gz" .
(cd "$out" && sha256sum framehands-aarch64.tar.gz > SHA256SUMS)
echo "$out/framehands-aarch64.tar.gz"
