#!/usr/bin/env bash
# Install or remove the testbench on the Frame. It's a development tool: get.sh, the release
# zip, and the image leave it out, and uninstall.sh doesn't remove it. Remove it with this.
# Usage: testbench/install.sh install [--restart]    # build, stage SteamVR's preload, MCP venv
#        testbench/install.sh uninstall [--restart]  # remove SteamVR's preload
# SteamVR picks the change up when it next starts. --restart restarts it now, which ends
# whatever is running in VR: only with nobody in the headset.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
frame="$root/scripts/frame.sh"
src=$FRAME_REPO/testbench

action=${1:-install}
restart=
case ${2:-} in
  --restart) restart=--restart ;;
  '') ;;
  *) echo "usage: $0 install|uninstall [--restart]" >&2; exit 2 ;;
esac

case $action in
  install)
    "$root/testbench/build.sh"
    # The preload's libraries are copied to ~/.local/share/frame-testbench/<hash>, so a
    # rebuild never overwrites a library a running vrserver has mapped.
    "$frame" --host "cd $src && python3 frame-testbench install $restart"
    # The MCP server runs on the host's python3; --no-deps keeps the venv to the pinned set.
    # A .pth file puts this checkout on the venv's path (the server's CLI subprocesses import
    # frame_testbench from it), with no package build.
    "$frame" --host "set -e; cd $src
[ -x .venv/bin/python ] || python3 -m venv .venv
.venv/bin/python -m pip install -q --disable-pip-version-check --no-deps -r requirements-mcp.txt
echo \"\$PWD\" >\"\$(.venv/bin/python -c 'import sysconfig; print(sysconfig.get_paths()[\"purelib\"])')/frame_testbench.pth\""
    echo "MCP server: $src/.venv/bin/python $src/frame-testbench-mcp --artifacts-dir $src/artifacts"
    ;;
  uninstall)
    "$frame" --host "cd $src && python3 frame-testbench uninstall $restart"
    ;;
  *) echo "usage: $0 install|uninstall [--restart]" >&2; exit 2 ;;
esac
