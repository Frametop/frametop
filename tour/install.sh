#!/usr/bin/env bash
# Install (or remove) the Frametop Tour menu entry on the Frame.
# Usage: tour/install.sh [install|uninstall]
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
"$root/scripts/sync.sh" >/dev/null
case ${1:-install} in
  install)
    fill_template "$root/tour/ft-tour.desktop" |
      on_frame "chmod +x tour/ft-tour && mkdir -p ~/.local/share/applications && cat > ~/.local/share/applications/ft-tour.desktop"
    echo "installed: Frametop Tour (Plasma menu, Utilities)" ;;
  uninstall) on_frame 'rm -f ~/.local/share/applications/ft-tour.desktop; echo removed' ;;
  *) echo "usage: $0 [install|uninstall]" >&2; exit 2 ;;
esac
