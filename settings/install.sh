#!/usr/bin/env bash
# Install (or remove) Frametop Settings' menu entry on the Frame. Install also removes the entries
# of the settings apps it replaced (Frametop Input Settings, Display Settings, Remote Displays and
# Remote Access), so the menus list just the one app.
# Usage: settings/install.sh [install|uninstall]
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
. "$root/scripts/install-lib.sh"
"$root/scripts/sync.sh" >/dev/null
apps=.local/share/applications
retired="ft-input-settings.desktop ft-display-settings.desktop ft-remote-displays.desktop ft-remote-settings.desktop"
case ${1:-install} in
  install)
    on_frame "chmod +x settings/ft-settings session/remote-ctl.sh
cd ~/$apps 2>/dev/null && rm -f $retired || true"
    install_menu_entry "$root/settings/ft-settings.desktop"
    echo "installed: Frametop Settings" ;;
  uninstall)
    on_frame "rm -f ~/$apps/ft-settings.desktop; echo removed" ;;
  *) echo "usage: $0 [install|uninstall]" >&2; exit 2 ;;
esac
