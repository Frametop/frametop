#!/usr/bin/env bash
# NPU hand tracking: installs ft-npud, the root service that runs the hand model on the headset's NPU
# (README.md), or takes it out. Hand tracking (ft-hands) uses it by itself once it runs, and the CPU
# models whenever it doesn't (HANDS_NPU, Frametop Settings' Hands page).
# Needs host sudo (frame_sudo in scripts/_env.sh asks for the password), on the Frame or from a PC.
#   npu/install.sh install    a clone builds the tree first (npu/build.sh); a release brings it built.
#                             Exit 3: installed, but ft-npud hasn't started yet (it keeps trying)
#   npu/install.sh uninstall
#   npu/install.sh status     what's installed, checked against its MANIFEST (no sudo)
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
root_sh=$FRAME_REPO/npu/root.sh
q() { printf %q "$1"; }

case ${1:-install} in
  install)
    if [ "$FRAME_RELEASE" = 0 ]; then
      "$root/npu/build.sh"
    else
      [ -x "$root/npu/build/tree/python/bin/python3" ] || { echo "ft-npud: this release has no npu/build/tree" >&2; exit 1; }
    fi
    uid=$(on_frame 'id -u')
    frame_sudo "/usr/bin/bash $(q "$root_sh") install $(q "$FRAME_REPO/npu/build/tree") $uid" ;;
  uninstall)
    frame_sudo "/usr/bin/bash $(q "$root_sh") uninstall" ;;
  status)
    on_frame "/usr/bin/bash $(q "$root_sh") status" ;;
  *) echo "usage: $0 [install|uninstall|status]" >&2; exit 2 ;;
esac
