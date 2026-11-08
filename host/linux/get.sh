#!/usr/bin/env bash
# Frametop host for Linux, in one line. In a terminal on the PC (Arch Linux or CachyOS, KDE
# Plasma on Wayland):
#
#   curl -fsSL https://frametop.github.io/frametop/host/linux/get.sh | bash
#
# It does what host/linux/README.md's Set up steps 1 and 2 do: downloads Frametop's build of
# Vibepollo (its Arch package) and Vibepollo's installer from the same release, checks both
# against the SHA-256s below, and runs the installer with the package (it asks for your sudo
# password). Then it says what's left: a restart, the Web UI login, one setting, and adding the
# PC on the Steam Frame. Run it again to reinstall, or for a newer build once this script has one.
# Options (piped, they go after "bash -s --"):
#   --dir DIR        where the downloads go (default ~/frametop-host)
#   --download-only  download and check them, but don't install
# When the build changes, update TAG, PACKAGE and both SHA-256s together, from the release's
# .sha256 file and the installer at that tag, and the README's steps to match.
set -euo pipefail

TAG=frametop-2.0.3
PACKAGE=vibepollo-2.0.3-1-x86_64.pkg.tar.zst
PACKAGE_SHA256=cd19c3079608d38b1ef02c665be5c1f8b3213b09bacf25ec4027265674518dc2
INSTALLER_SHA256=8b3611684341df7bbad0b2f6dc80c3644a8dc182fe0fe5282dcf618c87f27242
RELEASE=https://github.com/Frametop/frametop-vibepollo/releases/download/$TAG
INSTALLER=https://raw.githubusercontent.com/Frametop/frametop-vibepollo/$TAG/scripts/linux_install.sh

usage() {
  cat <<'EOF'
usage: get.sh [--dir DIR] [--download-only]
piped: curl -fsSL https://frametop.github.io/frametop/host/linux/get.sh | bash -s -- [options]
EOF
}

say() { printf '%s\n' "$*"; }
die() { printf 'Frametop host: %s\n' "$*" >&2; exit 1; }

# The whole script is one function, called on its last line: piped from curl, a download
# cut off part-way runs nothing.
main() {
dir=$HOME/frametop-host
install=1
while [ $# -gt 0 ]; do
  case "$1" in
    --dir) [ $# -ge 2 ] || die "--dir needs a folder"; dir=$2; shift ;;
    --download-only) install=0 ;;
    -h | --help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
  shift
done

# Vibepollo's Linux build is an Arch package for x86_64; its installer checks the rest
# (Plasma on Wayland, the kernel, the encoder).
if [ "$install" = 1 ]; then
  [ "$(uname -m)" = x86_64 ] || die "this PC is $(uname -m); Vibepollo's Linux build is for x86_64 only."
  command -v pacman >/dev/null || die "no pacman here: Vibepollo's Linux build is for Arch Linux and CachyOS only."
  [ "$(id -u)" != 0 ] || die "run this as yourself, not as root: it asks for sudo when it installs."
  command -v sudo >/dev/null || die "sudo isn't installed."
  case "${XDG_SESSION_TYPE:-}/${XDG_CURRENT_DESKTOP:-}" in
    wayland/*KDE*) ;;
    *) say "Note: this session isn't KDE Plasma on Wayland (${XDG_SESSION_TYPE:-?}, ${XDG_CURRENT_DESKTOP:-?}). Vibepollo streams only that; the installer checks too." ;;
  esac
fi
for tool in curl sha256sum; do
  command -v "$tool" >/dev/null || die "$tool isn't installed."
done

mkdir -p "$dir"
cd "$dir"

# fetch URL FILE SHA256: downloads FILE (unless it's there and checks out) and checks it.
fetch() {
  if [ -f "$2" ] && printf '%s  %s\n' "$3" "$2" | sha256sum -c --status; then
    say "$2: already here, checked."
    return
  fi
  say "Downloading $2..."
  curl -fL --progress-bar -o "$2.part" "$1"
  if ! printf '%s  %s\n' "$3" "$2.part" | sha256sum -c --status; then
    rm -f "$2.part"
    die "$2 doesn't match its SHA-256; nothing was installed. Try again, or report it at https://github.com/Frametop/frametop/issues"
  fi
  mv -f "$2.part" "$2"
  say "$2: checked."
}
fetch "$RELEASE/$PACKAGE" "$PACKAGE" "$PACKAGE_SHA256"
fetch "$INSTALLER" linux_install.sh "$INSTALLER_SHA256"

if [ "$install" = 0 ]; then
  say "Downloaded to $dir. To install: cd $dir && sudo bash linux_install.sh --package ./$PACKAGE"
  exit 0
fi

say "Installing Frametop's build of Vibepollo ($TAG). sudo asks for your password."
# Piped, this script's stdin is curl's output: the installer reads the terminal instead.
if [ -t 0 ]; then
  sudo bash linux_install.sh --package "./$PACKAGE"
elif [ -r /dev/tty ]; then
  sudo bash linux_install.sh --package "./$PACKAGE" </dev/tty
else
  sudo bash linux_install.sh --package "./$PACKAGE" </dev/null
fi

cat <<'EOF'

Installed. What's left:
  1. Restart the PC (after the first install), and log in to Plasma (Wayland).
  2. Open https://localhost:47990 and create the Web UI login. The Steam Frame signs in with it once.
  3. For virtual displays: in the Web UI's settings, turn on
     "Remove Remote Monitor when its client disconnects".
  4. On the Steam Frame: Frametop Settings > Remote displays > Add computer, pick this PC,
     sign in with the Web UI login, and choose its displays.
Don't add Vibepollo's own package repository: an update from it would replace Frametop's build.
EOF
}

main "$@"
