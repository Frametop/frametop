#!/bin/bash
# Start, stop, or ask about remote access to the Frametop desktop (VNC over the tailnet):
# remote-desktop.sh (krdp capturing the desktop on 127.0.0.1) and vnc-bridge.sh (VNC of its
# primary screen). Runs on the Frame host; frametop-session.sh and Frametop Remote Access
# (remote/ft-remote-settings) use it.
#
#   remote-ctl.sh start | stop | restart | status
#
# Capture only works in a desktop that started with REMOTE=1: KWin's permission checks for
# screencast and fake input are turned off when it starts, and only then. Such a session
# leaves $runtime/remote-capable; without it, start says so (exit 3) and the setting applies
# at the next desktop start. status prints key=value lines: capable, running, address, name,
# port.
set -u

here=$(dirname "$(readlink -f "$0")")
runtime=/run/user/$(id -u)/frametop
vnc_port=${VNC_PORT:-5900}

running() { pgrep -f '[X]vnc :20 ' >/dev/null && pgrep -f '[k]rdpserver --plasma' >/dev/null; }

stop() {
  pkill -f "[v]nc-bridge.sh" 2>/dev/null
  pkill -f "[r]emote-desktop.sh" 2>/dev/null
  pkill -f '[k]rdpserver --plasma' 2>/dev/null
  pkill -f '[X]vnc :20 ' 2>/dev/null
  pkill -f '[x]freerdp /v:127.0.0.1:' 2>/dev/null
  return 0
}

start() {
  if python3 -c 'import sys; sys.path.insert(0, sys.argv[1]); import ft_pinlock; sys.exit(0 if ft_pinlock.enabled() else 1)' "$here/../conf"; then
    echo 'Remote desktop access is unavailable with Steam PIN protection enabled' >&2
    return 3
  fi
  if [ ! -e "$runtime/remote-capable" ]; then
    echo "this desktop didn't start with remote access on: it applies at the next desktop start" >&2
    return 3
  fi
  running && return 0
  stop
  setsid "$here/remote-desktop.sh" "$runtime" > /tmp/frametop-remote.log 2>&1 < /dev/null &
  setsid "$here/vnc-bridge.sh" > /tmp/frametop-vnc.log 2>&1 < /dev/null &
  return 0
}

case ${1:-status} in
  start) start ;;
  stop) stop ;;
  restart) stop; sleep 1; start ;;
  status)
    addr=$(ip -4 -o addr show tailscale0 2>/dev/null | awk '{print $4}' | cut -d/ -f1)
    name=$(curl -s --max-time 2 --unix-socket /run/tailscale/tailscaled.sock \
             http://local-tailscaled.sock/localapi/v0/status 2>/dev/null |
           python3 -c 'import json, sys; print(json.load(sys.stdin)["Self"]["DNSName"].rstrip("."))' 2>/dev/null)
    echo "capable=$([ -e "$runtime/remote-capable" ] && echo 1 || echo 0)"
    echo "running=$(running && echo 1 || echo 0)"
    echo "address=$addr"
    echo "name=$name"
    echo "port=$vnc_port" ;;
  *) echo "usage: $0 start|stop|restart|status" >&2; exit 2 ;;
esac
