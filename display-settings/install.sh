#!/usr/bin/env bash
# Install (or remove) the Reset Screen Layout and Hide/Show Screens entries on the Frame (the
# display settings themselves are Frametop Settings' pages: settings/install.sh), with a global shortcut in the Frametop desktop for Reset Screen Layout:
# Meta+Shift+R (it takes effect the next time the desktop starts). Meta+Shift+H (Hide/Show
# Screens) is the input relay's (screens_toggle): KWin gets no keys while the screens are hidden,
# so a KWin shortcut could hide them but not show them again. Install drops the one older
# versions set.
# Usage: display-settings/install.sh [install|uninstall]
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
. "$root/scripts/install-lib.sh"
"$root/scripts/sync.sh" >/dev/null
apps=.local/share/applications
shortcuts=.config/frametop/kglobalshortcutsrc
case ${1:-install} in
  install)
    for f in ft-layout-reset ft-screens-toggle; do
      install_menu_entry "$root/display-settings/$f.desktop"
    done
    on_frame "chmod +x layout/ft-layout layout/ft_layout.py
mkdir -p ~/.config/frametop
kwriteconfig6 --file ~/$shortcuts --group services --group ft-layout-reset.desktop --key _launch 'Meta+Shift+R'
kwriteconfig6 --file ~/$shortcuts --group services --group ft-screens-toggle.desktop --key _launch --delete
layout/ft-layout launchers  # each profile's entry (Frametop: NAME), if there are profiles"
    echo "installed: Reset Screen Layout (Meta+Shift+R), Hide/Show Screens" ;;
  uninstall)
    on_frame "rm -f ~/$apps/ft-layout-reset.desktop ~/$apps/ft-screens-toggle.desktop
rm -f ~/$apps/frametop-profile-*.desktop  # the profiles' entries (the profiles stay in ~/.config/frametop-layout.json)
[ -f ~/$shortcuts ] && for f in ft-layout-reset ft-screens-toggle; do kwriteconfig6 --file ~/$shortcuts --group services --group \$f.desktop --key _launch --delete; done
echo removed" ;;
  *) echo "usage: $0 [install|uninstall]" >&2; exit 2 ;;
esac
