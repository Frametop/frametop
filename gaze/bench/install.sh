#!/usr/bin/env bash
# Add "Frametop Gaze Bench" to the app menu on a Frame with Frametop, for this tree (README.md).
# It needs ft-gaze and ft-gazepanel (gaze/build.sh; a release brings them built, and a tree
# without a build uses the installed release's) and, to record our tracker, ft-eyes
# (gaze/tracker/install.sh).
# Usage: gaze/bench/install.sh            install or update
#        gaze/bench/install.sh uninstall  remove the menu entry. Runs stay where they are.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$root/scripts/_env.sh"
entry='~/.local/share/applications/frametop-gazebench.desktop'

case ${1:-install} in
  install)
    fill_template "$root/gaze/bench/ft-gazebench.desktop" |
      on_frame "mkdir -p ~/.local/share/applications && cat > $entry"
    echo "Installed. On Frametop's desktop, open \"Frametop Gaze Bench\" from the app menu."
    echo "Runs go to ~/.local/share/frametop/gaze/bench."
    ;;
  uninstall)
    on_frame "rm -f $entry"
    echo "Removed the menu entry. Runs are still in ~/.local/share/frametop/gaze/bench."
    ;;
  *) echo "usage: $0 [install|uninstall]" >&2; exit 2 ;;
esac
