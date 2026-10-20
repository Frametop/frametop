#!/usr/bin/env bash
# Run this from an extracted ARM64 package on the Frame. User files are preserved.
set -euo pipefail
installer_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if (( $# > 1 )); then
    echo 'Usage: bash install.sh [extracted-package-directory]' >&2
    exit 1
fi
# The current installer can also install an older package whose bundled installer
# predates replacement/cleanup support.
package_dir=$(cd -- "${1:-$installer_dir}" && pwd)
helper="$installer_dir/share/framekeyboard/update/update_helper.py"
if [[ $(uname -m) != aarch64 ]]; then
    echo 'This package targets the ARM64 Steam Frame.' >&2
    exit 1
fi
version=$("$package_dir/bin/framekeyboard" --version | awk '{print $2}')
if [[ ! $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo 'Invalid package version.' >&2
    exit 1
fi
# The override lets integration tests install into an isolated directory.
install_home=${FRAMEKEYBOARD_INSTALL_HOME:-$HOME}
install_root="$install_home/.local/share/framekeyboard"
runtime_base="/run/user/$(id -u)"
[[ -d $runtime_base ]] || runtime_base=${XDG_RUNTIME_DIR:?No user runtime directory is available.}
# Isolated installer tests supply a private runtime directory too.
runtime_dir=${FRAMEKEYBOARD_INSTALL_RUNTIME_DIR:-$runtime_base/framekeyboard}
if [[ ! -d $runtime_dir ]]; then
    mkdir -m 700 -- "$runtime_dir"
fi
if [[ -L $runtime_dir || ! -O $runtime_dir || $(stat -c %a -- "$runtime_dir") != 700 ||
      -L $runtime_dir/vr.lock || ( -e $runtime_dir/vr.lock && ! -f $runtime_dir/vr.lock ) ]]; then
    echo 'Invalid keyboard runtime directory or instance lock.' >&2
    exit 1
fi
exec 8>> "$runtime_dir/vr.lock"
flock -n 8 || { echo 'Full Keyboard is running. Quit it in Settings, then install again.' >&2; exit 1; }
if [[ -L $install_root/releases ]]; then
    echo 'The releases directory is a symbolic link; leave this installation managed manually.' >&2
    exit 1
fi
mkdir -p "$install_root/releases" "$install_home/.local/bin" "$install_home/.local/share/applications"
install_root=$(realpath -e -- "$install_root")
exec 9>> "$install_root/install.lock"
flock -n 9 || { echo 'Another installation is running.' >&2; exit 1; }
if [[ -e $install_root/current && ! -L $install_root/current ]]; then
    echo 'The current installation path is not a release link.' >&2
    exit 1
fi
old_release=''
if [[ -L $install_root/current ]]; then
    old_release=$(readlink -f -- "$install_root/current" || true)
fi
release=''
stage=''
launcher_tmp=''
desktop_tmp=''
link_stage=''
release_created=false
activated=false
cleanup() {
    [[ -z $stage ]] || rm -rf -- "$stage"
    [[ -z $launcher_tmp ]] || rm -f -- "$launcher_tmp"
    [[ -z $desktop_tmp ]] || rm -f -- "$desktop_tmp"
    [[ -z $link_stage ]] || rm -rf -- "$link_stage"
    if $release_created && ! $activated; then
        # An interruption immediately after the atomic switch must not delete
        # the release now named by current, even before activated was assigned.
        if [[ $(readlink -f -- "$install_root/current" || true) != "$release" ]]; then
            rm -rf -- "$release"
        fi
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
stage=$(mktemp -d "$install_root/releases/.install-$version.XXXXXX")
release="$install_root/releases/$version-install-${stage##*.}"
if [[ -e $release || -L $release ]]; then
    echo 'The new release directory already exists.' >&2
    exit 1
fi
cp -a "$package_dir/." "$stage/"
for kind in layouts languages themes; do
    if [[ ! -d $stage/share/framekeyboard/$kind ]]; then
        echo "Missing packaged profiles: $kind" >&2
        exit 1
    fi
done
# Validate the copied JSON, not just the fallback profiles embedded in the binary.
"$stage/bin/framekeyboard" --check --data-dir "$stage/share/framekeyboard" --config-dir "$stage/.validation"
python3 -I "$helper" --mark-production "$install_root" "$stage" "${release##*/}"
launcher_tmp=$(mktemp "$install_home/.local/bin/.framekeyboard.XXXXXX")
desktop_tmp=$(mktemp "$install_home/.local/share/applications/.framekeyboard.XXXXXX")
cat > "$launcher_tmp" <<'LAUNCHER'
#!/usr/bin/env bash
set -euo pipefail
install_root="${FRAMEKEYBOARD_INSTALL_HOME:-$HOME}/.local/share/framekeyboard/current"
if [[ $# == 0 || ( $# == 1 && $1 == --vr ) ]]; then
    set -- --vr --input ei --start-enabled
    if [[ -n ${FRAMEKEYBOARD_TARGET_LANGUAGE:-} ]]; then
        set -- "$@" --target-language "$FRAMEKEYBOARD_TARGET_LANGUAGE"
    fi
fi
exec "$install_root/bin/framekeyboard" --data-dir "$install_root/share/framekeyboard" "$@"
LAUNCHER
chmod 755 "$launcher_tmp"
cat > "$desktop_tmp" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Full Keyboard
Comment=Full-size keyboard for Steam Frame dashboard and local apps
Exec="$install_home/.local/bin/framekeyboard"
Icon=input-keyboard
Terminal=false
Categories=Utility;Accessibility;
DESKTOP
chmod 644 "$desktop_tmp"
# Finish every fallible preparation before switching the active release.
link_stage=$(mktemp -d "$install_root/.links.XXXXXX")
ln -s -- "releases/${release##*/}" "$link_stage/current"
mv -T -- "$stage" "$release"
stage=''
release_created=true
mv -Tf -- "$launcher_tmp" "$install_home/.local/bin/framekeyboard"
launcher_tmp=''
mv -Tf -- "$desktop_tmp" "$install_home/.local/share/applications/framekeyboard.desktop"
desktop_tmp=''
mv -Tf -- "$link_stage/current" "$install_root/current"
activated=true
if [[ -n $old_release ]]; then
    if ! python3 -I "$helper" --remove-production "$install_root" "$old_release"; then
        echo 'Installed successfully, but the replaced release could not be removed.' >&2
    fi
fi
printf 'Installed Full Keyboard %s. No autostart or Steam changes.\n' "$version"
