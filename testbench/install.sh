#!/usr/bin/env bash
# Install or remove the testbench. It's a development tool: get.sh never installs it, the
# release zip and the image leave it out, and uninstall.sh doesn't remove it. Remove it with this.
# Usage: testbench/install.sh install [--restart]    # build, stage SteamVR's preload, MCP venv
#        testbench/install.sh uninstall [--restart]  # remove the preload and its libraries
# SteamVR picks the change up when it next starts. --restart restarts it now, which ends
# whatever is running in VR: only with nobody in the headset.
#
# The preload runs inside vrserver. If it crashes SteamVR, systemd restarts SteamVR at once,
# and SteamOS's health check (/usr/share/deckard/steamvr-health-check) counts every session
# that fails or lasts under 10 s: after three in a row it repairs Steam, after four it sets
# the next boot to the other OS slot. So --restart refuses while a failure is already
# counted, and takes the preload back out if SteamVR doesn't stay up.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
dropin=$HOME/.config/systemd/user/steamvr.service.d/90-frame-testbench.conf
libs=$HOME/.local/share/frame-testbench
tracker=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/steamvr-short-session-tracker

usage() { echo "usage: $0 install|uninstall [--restart]" >&2; exit 2; }

# The MCP server runs where its client runs: on the Frame, or on a PC that drives the Frame
# with --host. --no-deps and --only-binary keep the venv to the pinned wheels. A .pth file
# puts this checkout on the venv's path (the server's CLI subprocesses import
# frame_testbench) with no package build. A venv from another Python (after a SteamOS
# update) is made again.
mcp_venv() {
  cd "$root/testbench"
  [ "$(.venv/bin/python -c 'import pip, sys; print(sys.version)' 2>/dev/null)" = "$(python3 -c 'import sys; print(sys.version)')" ] ||
    python3 -m venv --clear .venv
  .venv/bin/python -m pip install -q --disable-pip-version-check --no-deps --only-binary=:all: -r requirements-mcp.txt
  .venv/bin/python -m pip check -q >/dev/null
  echo "$PWD" >"$(.venv/bin/python -c 'import sysconfig; print(sysconfig.get_paths()["purelib"])')/frame_testbench.pth"
  .venv/bin/python -m unittest discover -q -s tests -p 'test_mcp*.py'
  echo "MCP server: $PWD/.venv/bin/python $PWD/frame-testbench-mcp --artifacts-dir $PWD/artifacts"
}

# On the SteamOS host, from here down.
restart_steamvr() {
  if grep -q frog "$tracker" 2>/dev/null; then
    echo "SteamOS already counts a failed SteamVR session ($tracker), and a restart could add another." >&2
    echo "Not restarting: restart SteamVR yourself once it has run normally." >&2
    exit 1
  fi
  local since age
  since=$(systemctl --user show -p ActiveEnterTimestamp --value steamvr.service)
  if [ -n "$since" ] && systemctl --user -q is-active steamvr.service; then
    age=$(( $(date +%s) - $(date -d "$since" +%s) ))
    [ "$age" -ge 15 ] || sleep $((15 - age))
  fi
  local before after
  before=$(systemctl --user show -p NRestarts --value steamvr.service)
  systemctl --user restart steamvr.service
  sleep 20
  after=$(systemctl --user show -p NRestarts --value steamvr.service)
  if [ "$after" = "$before" ] && systemctl --user -q is-active steamvr.service && pgrep -x vrserver >/dev/null; then
    echo "SteamVR restarted and stayed up"
    return
  fi
  echo "SteamVR didn't stay up after the restart (restarts $before -> $after)." >&2
  if [ -f "$dropin" ]; then
    (cd "$root/testbench" && python3 frame-testbench uninstall >/dev/null)
    echo "Took the testbench preload out: SteamVR starts without it next time." >&2
  fi
  exit 1
}

host_install() {
  cd "$root/testbench"
  python3 frame-testbench install >/dev/null
  # Earlier builds' libraries. A running vrserver keeps a deleted one mapped.
  local keep
  keep=$(sed -n 's|^Environment="LD_PRELOAD=\(.*\)/libframe_loader.so"$|\1|p' "$dropin")
  [ -d "$keep" ] || { echo "can't read the library folder from $dropin" >&2; exit 1; }
  for d in "$libs"/*/; do
    [ "${d%/}" = "$keep" ] || rm -rf -- "$d"
  done
  echo "staged SteamVR's preload from $keep"
}

host_uninstall() {
  cd "$root/testbench"
  python3 frame-testbench uninstall >/dev/null
  rm -rf -- "$libs"
  echo "removed SteamVR's preload"
}

restart=
case ${2:-} in
  --restart) restart=--restart ;;
  '') ;;
  *) usage ;;
esac

case ${1:-} in
  install)
    "$root/testbench/build.sh"
    (mcp_venv)
    "$root/scripts/frame.sh" --host "testbench/install.sh host-install $restart"
    ;;
  uninstall)
    "$root/scripts/sync.sh" >/dev/null
    "$root/scripts/frame.sh" --host "testbench/install.sh host-uninstall $restart"
    ;;
  host-install) host_install; [ -z "$restart" ] || restart_steamvr ;;
  host-uninstall) host_uninstall; [ -z "$restart" ] || restart_steamvr ;;
  *) usage ;;
esac
