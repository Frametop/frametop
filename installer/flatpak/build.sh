#!/usr/bin/env bash
# Build the Frametop Installer's Flatpak bundle: installer/build/FrametopInstaller.flatpak, and
# its SHA256SUMS line. The app is Python on the GNOME runtime (GTK 4, libadwaita, PyGObject), so
# nothing compiles: this needs flatpak and the runtime, not flatpak-builder or an SDK. CI's
# release builds it the same way (.github/workflows/release.yml), on arm64 like the Frame.
#
# Usage: installer/flatpak/build.sh [--arch ARCH] [--version V]
#   --arch ARCH  the bundle's architecture (default flatpak's: aarch64 on a Steam Frame)
#   --version V  the version it says it is (default: git describe)
# Needs: flatpak install --user flathub org.gnome.Platform//51 (for that architecture)
#
# Installing the bundle (a double-click opens Discover; or flatpak install --user FILE) also
# installs org.gnome.Platform//51 from Flathub if it isn't there, about 300 MB.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../.." && pwd)
ID=io.github.frametop.Installer
RUNTIME=org.gnome.Platform
BRANCH=51
FLATHUB=https://dl.flathub.org/repo/flathub.flatpakrepo

arch= version=
while [ $# -gt 0 ]; do
  case $1 in
    --arch) arch=${2:?--arch needs one}; shift ;;
    --version) version=${2:?--version needs one}; shift ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done
command -v flatpak >/dev/null || { echo "flatpak isn't installed" >&2; exit 1; }
arch=${arch:-$(flatpak --default-arch)}
commit=$(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)
version=${version:-$(git -C "$root" describe --tags --always --dirty 2>/dev/null || echo dev)}
if ! flatpak info --arch="$arch" "$RUNTIME//$BRANCH" >/dev/null 2>&1; then
  echo "It needs $RUNTIME//$BRANCH for $arch: flatpak install --user --arch=$arch flathub $RUNTIME//$BRANCH" >&2
  exit 1
fi

out=$root/installer/build
app=$out/app repo=$out/repo bundle=$out/FrametopInstaller.flatpak
rm -rf "$app" "$repo" "$bundle"
mkdir -p "$out"
# The runtime stands in for the SDK: nothing builds, and build-init only records it.
flatpak build-init --arch="$arch" "$app" "$ID" "$RUNTIME" "$RUNTIME" "$BRANCH"

f=$app/files
share=$f/share/frametop-installer
install -Dm755 "$here/frametop-installer" "$f/bin/frametop-installer"
install -d "$share/frametop_installer" "$share/host"
install -m644 "$root"/installer/frametop_installer/*.py "$share/frametop_installer/"
# (files only: a test run leaves host/__pycache__)
find "$root/installer/host" -maxdepth 1 -type f -exec install -m755 -t "$share/host/" {} +
install -m755 "$root/get.sh" "$share/get.sh"
install -m755 "$root/uninstall.sh" "$share/uninstall.sh"
printf 'VERSION=%s\nCOMMIT=%s\n' "$version" "$commit" >"$share/VERSION"
install -Dm644 "$here/$ID.desktop" "$f/share/applications/$ID.desktop"
install -Dm644 "$here/$ID.metainfo.xml" "$f/share/metainfo/$ID.metainfo.xml"
install -Dm644 "$here/$ID.svg" "$f/share/icons/hicolor/scalable/apps/$ID.svg"

# What the sandbox gets (installer/README.md says why): the network for GitHub, a window, and
# flatpak-spawn --host, which runs the install on the host. The one host folder it writes is its
# own cache, where it puts the host scripts, the install's log and its state. It reads Downloads,
# where the Debug button's report goes, to copy its text and show it in the file manager.
flatpak build-finish "$app" --command=frametop-installer \
  --share=network --share=ipc --socket=wayland --socket=fallback-x11 --device=dri \
  --talk-name=org.freedesktop.Flatpak --filesystem=~/.cache/frametop-installer:create \
  --filesystem=xdg-download:ro

stamp=$(git -C "$root" log -1 --format=%cI 2>/dev/null || date -u +%FT%TZ)
# --disable-sandbox: build-export checks the icon in a bwrap sandbox, which CI's runner (Ubuntu
# 24.04, where AppArmor keeps bwrap from making its namespace) and containers refuse: "is not a
# valid icon: bwrap: Creating new namespace failed". The icon is this repo's own file.
flatpak build-export --disable-sandbox --arch="$arch" --timestamp="$stamp" \
  --subject="Frametop Installer $version ($commit)" "$repo" "$app" stable
flatpak build-bundle --arch="$arch" --runtime-repo="$FLATHUB" "$repo" "$bundle" "$ID" stable
(cd "$out" && sha256sum FrametopInstaller.flatpak >FrametopInstaller.flatpak.sha256)
echo "Built $bundle ($version, $arch): $(du -h "$bundle" | cut -f1)"
