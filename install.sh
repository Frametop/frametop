#!/usr/bin/env bash
# Install FrameHands only into the user's home. No Distrobox, Podman, or host packages.
set -euo pipefail
root=$(cd "$(dirname "$0")" && pwd)
app=$HOME/.local/share/framehands/app
units=$HOME/.config/systemd/user
tmp=$(mktemp -d)
stage=
cleanup() {
  rm -rf -- "$tmp"
  if [[ -n $stage && -d $stage ]]; then rm -rf -- "$stage"; fi
}
trap cleanup EXIT

fail() { echo "FrameHands install: $*" >&2; exit 1; }
[[ $(uname -m) == aarch64 ]] || fail 'aarch64 Steam Frame required'
grep -qi steamos /etc/os-release || fail 'SteamOS was not detected'
[[ -f /opt/steamvr/bin/linuxarm64/libopenvr_api.so ]] || fail 'SteamVR ARM64 OpenVR library is missing'
[[ -r /persist/xrservice.json && -r /persist/device_config.json ]] ||
  fail 'Steam Frame camera calibration files are missing'
command -v systemctl >/dev/null || fail 'systemd user services are required'
command -v getcap >/dev/null || fail 'getcap is required to verify camera capabilities'
reg=/opt/steamvr/bin/linuxarm64/vrpathreg
[[ -x $reg ]] || fail 'SteamVR driver registration tool is missing'
if [[ $(id -u) != 0 ]]; then command -v sudo >/dev/null || fail 'sudo is required to set camera capabilities'; fi
[[ ! -L $app ]] || fail 'FrameHands app path is a symlink; refusing to replace it'
for unit in framehands-camd.service framehands-track.service framehands-view.service framehands-input.service; do
  file=$units/$unit
  if [[ -f $file ]] && ! grep -Fq "$app/" "$file"; then
    fail "$file exists and does not belong to this FrameHands install"
  fi
done
for unit in frametop-camd.service frametop-hands.service frametop-handview.service frametop-pointer.service; do
  if systemctl --user is-active --quiet "$unit"; then
    fail "$unit is running. Stop Frametop hand tracking before installing FrameHands; it will not be changed."
  fi
done

source_dir=$root
if [[ ! -x $root/hands/build/fh-camd || ! -x $root/hands/build/fh-hands || ! -x $root/renderer/build/framehands-view || ! -x $root/pointer/build/fh-input || ! -f $root/pointer/build/driver_fh_pointer.so ]]; then
  command -v git >/dev/null || fail 'prebuilt binaries are missing; install from a FrameHands release archive'
  remote=$(git -C "$root" remote get-url origin 2>/dev/null || true)
  case $remote in
    https://github.com/*) repo=${remote#https://github.com/} ;;
    git@github.com:*) repo=${remote#git@github.com:} ;;
    *) fail 'cannot find the fork release; download its framehands-aarch64.tar.gz and run install.sh from it' ;;
  esac
  repo=${repo%.git}
  [[ $repo =~ ^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$ ]] || fail 'unexpected GitHub remote'
  url=https://github.com/$repo/releases/latest/download
  echo "Downloading prebuilt FrameHands from $repo"
  curl -fL --retry 3 -o "$tmp/framehands-aarch64.tar.gz" "$url/framehands-aarch64.tar.gz" ||
    fail 'no prebuilt release is available for this fork'
  curl -fL --retry 3 -o "$tmp/SHA256SUMS" "$url/SHA256SUMS" || fail 'release checksum is missing'
  (cd "$tmp" && grep -E '^[0-9a-f]{64}  framehands-aarch64.tar.gz$' SHA256SUMS | sha256sum -c --status) ||
    fail 'release checksum failed'
  if tar -tzf "$tmp/framehands-aarch64.tar.gz" | grep -Eq '(^/|(^|/)\.\.(/|$))'; then
    fail 'release archive has unsafe paths'
  fi
  if tar -tvzf "$tmp/framehands-aarch64.tar.gz" | grep -Eq '^[lh]'; then
    fail 'release archive contains links'
  fi
  mkdir "$tmp/release"
  tar -xzf "$tmp/framehands-aarch64.tar.gz" -C "$tmp/release"
  source_dir=$tmp/release
fi

for bin in hands/build/fh-camd hands/build/fh-hands renderer/build/framehands-view pointer/build/fh-input; do
  [[ -x $source_dir/$bin ]] || fail "$bin is missing from the release"
done
[[ -f $source_dir/pointer/build/driver_fh_pointer.so ]] || fail 'the SteamVR input driver is missing'
[[ -f $source_dir/hands/models/student/hand.ncnn.bin ]] ||
  fail 'the pinned fine-tuned models are missing from the release'
[[ -f $source_dir/hands/models/ncnn/hand.ncnn.bin ]] || fail 'the stock fallback model is missing'

stage=$HOME/.local/share/framehands/app.new.$$
[[ ! -e $stage && ! -L $stage ]] || fail 'temporary install path already exists'
mkdir -m 700 "$stage"
mkdir -p "$stage/hands/build" "$stage/hands/camd" "$stage/renderer/build" "$stage/renderer/assets" "$stage/pointer/build" "$stage/pointer/driver/fh_pointer/bin/linuxarm64" "$stage/config" "$stage/services"
cp -a "$source_dir/hands/build/fh-camd" "$source_dir/hands/build/fh-hands" "$stage/hands/build/"
cp -a "$source_dir/hands/camd/LICENSE.FrameEyeCameraFeed" "$stage/hands/camd/"
cp -a "$source_dir/hands/models" "$stage/hands/"
cp -a "$source_dir/renderer/build/framehands-view" "$stage/renderer/build/"
cp -a "$source_dir/renderer/assets/hands" "$stage/renderer/assets/"
cp -a "$source_dir/pointer/build/fh-input" "$stage/pointer/build/"
cp -a "$source_dir/pointer/driver/fh_pointer/". "$stage/pointer/driver/fh_pointer/"
cp -a "$source_dir/pointer/build/driver_fh_pointer.so" "$stage/pointer/driver/fh_pointer/bin/linuxarm64/"
cp -a "$source_dir/config/default.conf" "$stage/config/"
cp -a "$source_dir/services/". "$stage/services/"
cp -a "$source_dir/framehands" "$stage/framehands"
cp -a "$source_dir/LICENSE" "$stage/LICENSE"
chmod 755 "$stage/framehands"
mkdir -p "$HOME/.config/framehands" "$HOME/.local/bin" "$units"
chmod 700 "$HOME/.config/framehands"
if [[ ! -e $HOME/.config/framehands/config ]]; then
  cp "$stage/config/default.conf" "$HOME/.config/framehands/config"
  chmod 600 "$HOME/.config/framehands/config"
fi

# Set capabilities only on our copy. ft-camd drops them after attaching to XRService.
caps=cap_sys_ptrace,cap_perfmon,cap_dac_read_search+ep
if [[ $(id -u) == 0 ]]; then
  setcap "$caps" "$stage/hands/build/fh-camd"
else
  sudo setcap "$caps" "$stage/hands/build/fh-camd" || fail 'camera capabilities could not be set'
fi
getcap "$stage/hands/build/fh-camd" | grep -q cap_sys_ptrace || fail 'camera capabilities were not applied'

# From here, only FrameHands units are changed. A failed upgrade restores the old app.
systemctl --user stop framehands-input.service framehands-view.service framehands-track.service framehands-camd.service 2>/dev/null || true
old=$HOME/.local/share/framehands/app.old.$$
if [[ -e $app ]]; then mv "$app" "$old"; fi
mv "$stage" "$app"
for unit in framehands-camd.service framehands-track.service framehands-view.service framehands-input.service; do
  sed "s|@ROOT@|$app|g" "$app/services/$unit" > "$units/$unit"
done
ln -sfn "$app/framehands" "$HOME/.local/bin/framehands"
systemctl --user daemon-reload
mkdir -p "$tmp/no-open"
printf '#!/bin/sh\nexit 0\n' > "$tmp/no-open/xdg-open"
chmod 755 "$tmp/no-open/xdg-open"
register_driver() {
  env PATH="$tmp/no-open:$PATH" XDG_CONFIG_HOME="$HOME/.config" \
    LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 "$reg" adddriver "$app/pointer/driver/fh_pointer"
}
if ! register_driver || ! "$app/framehands" sync; then
  echo 'FrameHands install: service activation failed; restoring the previous install' >&2
  systemctl --user stop framehands-input.service framehands-view.service framehands-track.service framehands-camd.service 2>/dev/null || true
  rm -rf -- "$app"
  if [[ -d $old ]]; then
    mv "$old" "$app"
    for unit in framehands-camd.service framehands-track.service framehands-view.service framehands-input.service; do
      sed "s|@ROOT@|$app|g" "$app/services/$unit" > "$units/$unit"
    done
    ln -sfn "$app/framehands" "$HOME/.local/bin/framehands"
    systemctl --user daemon-reload
    "$app/framehands" sync || true
  else
    env XDG_CONFIG_HOME="$HOME/.config" LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 \
      "$reg" removedriver "$app/pointer/driver/fh_pointer" 2>/dev/null || true
    for unit in framehands-camd.service framehands-track.service framehands-view.service framehands-input.service; do rm -f "$units/$unit"; done
    rm -f "$HOME/.local/bin/framehands"
    systemctl --user daemon-reload
  fi
  exit 1
fi
if [[ -d $old ]]; then
  old_bin=$old/hands/build/fh-camd
  if [[ -f $old_bin ]] && getcap "$old_bin" | grep -q cap_sys_ptrace; then
    if [[ $(id -u) == 0 ]]; then setcap -r "$old_bin"; else sudo setcap -r "$old_bin"; fi
  fi
  rm -rf "$old"
fi
echo 'FrameHands installed. Restart SteamVR normally to load its input driver, then run: ~/.local/bin/framehands status'
