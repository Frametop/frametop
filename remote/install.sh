#!/usr/bin/env bash
# Install (or remove) Frametop Remote Access in the desktop's app menu.
# Usage: remote/install.sh [install|uninstall]
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
. "$root/scripts/install-lib.sh"
"$root/scripts/sync.sh" >/dev/null
apps=.local/share/applications
case ${1:-install} in
  install)
    install_menu_entry "$root/remote/ft-remote-settings.desktop"
    on_frame "chmod +x remote/ft-remote-settings session/remote-ctl.sh"
    echo "installed: Frametop Remote Access" ;;
  uninstall)
    on_frame "rm -f ~/$apps/ft-remote-settings.desktop; echo removed" ;;
  *) echo "usage: $0 [install|uninstall]" >&2; exit 2 ;;
esac
