#!/usr/bin/env bash
# Install (or remove) the Frametop menu entry on the Frame (menu/ft_menu.py): one entry in
# SteamVR's Launch a program list for Frametop's apps and commands, which leave that list.
# Install also moves the optional apps' entries an older version put there (the recorders, Gaze
# Bench, Gaze Probe) into the menu in place: they keep what they run, and their own installers
# (which build, or ask for the password) write the rest of the new entry when they run next.
# Usage: menu/install.sh [install|uninstall]
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
. "$root/scripts/install-lib.sh"
"$root/scripts/sync.sh" >/dev/null
optional="frametop-handrec.desktop frametop-eyerec.desktop frametop-gazebench.desktop ft-gazeprobe.desktop"
case ${1:-install} in
  install)
    on_frame "chmod +x menu/ft-menu"
    install_menu_entry "$root/menu/ft-menu.desktop"
    on_frame "for f in $optional; do
  f=$FT_APPS_DIR/\$f
  [ -f \$f ] && ! grep -q '^NotShowIn=' \$f || continue
  sed -i '/^\\[Desktop Entry\\]\$/a NotShowIn=gamescope;\\nX-Frametop-Menu=tools' \$f
done"
    echo "installed: the Frametop menu (Launch a program -> Frametop)" ;;
  uninstall)
    on_frame "rm -f $FT_APPS_DIR/ft-menu.desktop; echo removed"
    echo "Frametop's apps stay out of Launch a program: uninstall.sh removes them with the rest." ;;
  *) echo "usage: $0 [install|uninstall]" >&2; exit 2 ;;
esac
