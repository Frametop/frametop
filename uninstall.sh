#!/usr/bin/env bash
set -euo pipefail
app=$HOME/.local/share/framehands/app
units=$HOME/.config/systemd/user
[[ ! -L $app ]] || { echo 'FrameHands app path is a symlink; refusing to uninstall' >&2; exit 1; }
purge_models=0 purge_config=0 yes=0
for arg in "$@"; do
  case $arg in
    --models) purge_models=1 ;;
    --config) purge_config=1 ;;
    --yes) yes=1 ;;
    *) echo 'usage: ./uninstall.sh [--models] [--config] [--yes]' >&2; exit 2 ;;
  esac
done
if (( purge_config && ! yes )); then
  read -r -p 'Delete ~/.config/framehands/config? [y/N] ' answer || answer=
  [[ $answer == y || $answer == Y ]] || purge_config=0
fi
if (( purge_models && ! yes )); then
  read -r -p 'Delete cached FrameHands models? [y/N] ' answer || answer=
  [[ $answer == y || $answer == Y ]] || purge_models=0
fi
for unit in framehands-dashboard.service framehands-input.service framehands-view.service framehands-track.service framehands-camd.service; do
  file=$units/$unit
  if [[ -f $file ]] && grep -Fq "$app/" "$file"; then
    systemctl --user disable --now "$unit" 2>/dev/null || true
    rm -f "$file"
  fi
done
systemctl --user daemon-reload 2>/dev/null || true
if [[ -x $app/dashboard/build/framehands-dashboard ]]; then
  "$app/dashboard/build/framehands-dashboard" --unregister 2>/dev/null || true
fi
reg=/opt/steamvr/bin/linuxarm64/vrpathreg
if [[ -x $reg && -f $app/pointer/driver/fh_pointer/driver.vrdrivermanifest ]]; then
  env XDG_CONFIG_HOME="$HOME/.config" LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 \
    "$reg" removedriver "$app/pointer/driver/fh_pointer" || true
fi
link=$HOME/.local/bin/framehands
if [[ -L $link && $(readlink "$link") == "$app/framehands" ]]; then rm -f "$link"; fi
bin=$app/hands/build/fh-camd
if [[ -f $bin ]] && command -v getcap >/dev/null && getcap "$bin" | grep -q cap_sys_ptrace; then
  if [[ $(id -u) == 0 ]]; then setcap -r "$bin"; else sudo setcap -r "$bin"; fi
fi
if [[ -d $app ]]; then rm -rf -- "$app"; fi
if (( purge_models )); then rm -rf -- "$HOME/.cache/framehands/models"; fi
if (( purge_config )); then rm -rf -- "$HOME/.config/framehands"; fi
echo 'FrameHands removed. SteamVR and Frametop were not changed.'
