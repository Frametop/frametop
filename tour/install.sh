#!/usr/bin/env bash
# Install (or remove) the Frametop Tour and Controls Guide menu entries on the Frame.
# Usage: tour/install.sh [install|uninstall]
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
"$root/scripts/sync.sh" >/dev/null
case ${1:-install} in
  install)
    on_frame 'chmod +x tour/ft-tour && mkdir -p ~/.local/share/applications'
    for entry in ft-tour ft-recenter-help; do
      fill_template "$root/tour/$entry.desktop" |
        on_frame "cat > ~/.local/share/applications/$entry.desktop"
    done
    echo "installed: Frametop Tour and Frametop Controls Guide (Plasma menu, Utilities)" ;;
  uninstall) on_frame 'rm -f ~/.local/share/applications/ft-tour.desktop ~/.local/share/applications/ft-recenter-help.desktop; echo removed' ;;
  *) echo "usage: $0 [install|uninstall]" >&2; exit 2 ;;
esac
