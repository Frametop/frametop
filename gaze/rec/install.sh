#!/usr/bin/env bash
# Install the eye recorder on a Frame that has Frametop (get.sh --experimental): bring the dev
# container's packages up to date, build the gaze tools the round uses (gaze/build.sh: ft-gaze and
# the headset panel ft-gazepanel; gaze/tracker/build.sh: our tracker ft-eyes, which the fit coach
# runs) and add "Frametop Eye Recorder" to the app menu (for Frametop's desktop, as the hand
# recorder's). It doesn't change Frametop's gaze setup; the window offers SteamVR's 90 fps eye
# cameras (gaze/eye-rate.py) when they're slower.
# Usage: gaze/rec/install.sh            install or update
#        gaze/rec/install.sh uninstall  remove the menu entry. Recordings stay where they are.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$root/scripts/_env.sh"
entry='~/.local/share/applications/frametop-eyerec.desktop'

case ${1:-install} in
  install)
    echo "== 1/3 dev container packages"
    "$root/setup/dev-container.sh"
    echo "== 2/3 the gaze tools: ft-gaze, ft-gazepanel and ft-eyes"
    "$root/gaze/build.sh"
    "$root/gaze/tracker/build.sh"
    echo "== 3/3 the menu entry"
    fill_template "$root/gaze/rec/ft-eyerec.desktop" |
      on_frame "mkdir -p ~/.local/share/applications && cat > $entry"
    echo
    echo "Installed. On Frametop's desktop, open \"Frametop Eye Recorder\" from the app menu."
    echo "Recordings go to ~/.local/share/frametop/eyes/contrib."
    ;;
  uninstall)
    on_frame "rm -f $entry"
    echo "Removed the menu entry."
    echo "Your recordings are still in ~/.local/share/frametop/eyes/contrib:"
    echo "delete that folder to remove them."
    ;;
  *) echo "usage: $0 [install|uninstall]" >&2; exit 2 ;;
esac
