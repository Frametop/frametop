#!/usr/bin/env bash
# The Frametop Installer's Debug button runs this on the host. It writes one text file with what
# a report of a problem needs, in Downloads: the headset and SteamOS, SteamVR, the Frametop
# Installer, the installed Frametop and its settings, its services and what's running, the last
# install's log, recent errors, each feature's logs and NPU hand tracking (debug-features.sh),
# and, when a Frametop release is installed, its own report (scripts/report.sh). Nothing is sent
# anywhere: the user sends the file.
#
# It also runs by hand, when the window won't open:
#   bash ~/.cache/frametop-installer/bin/debug-report.sh
#
# Usage: debug-report.sh [INSTALLER_VERSION]
# Prints "wrote PATH" last.
# Secrets, addresses and the serial number are hidden before it's saved (debug-mask.py says which).
# Names the user gave things, such as their PCs' in the screens' layout, stay. Left out: Frametop
# Voice's part of Frametop's report, which listens to the microphone for a second.
# The commands' timeouts add up to under 8 minutes, inside the window's 9 (debug_page.TIMEOUT).
# FRAMETOP_DEBUG_OFFLINE=1: no GitHub check (the tests).
set -u
here=$(dirname "${BASH_SOURCE[0]}")
. "$here/debug-lib.sh"
uid=$(id -u)
export XDG_RUNTIME_DIR=/run/user/$uid DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$uid/bus
# (English dates on journal lines, here and in Frametop's own report, for the mask to find the hostname)
export LC_ALL=C
inst=${1:-}
offline=${FRAMETOP_DEBUG_OFFLINE:-0}
cache=$HOME/.cache/frametop-installer
logs=$HOME/.local/share/Steam/logs
rel=$HOME/.local/share/frametop/releases/current

dir=$(xdg-user-dir DOWNLOAD 2>/dev/null || true)
[ -n "$dir" ] && [ "$dir" != "$HOME" ] || dir=$HOME/Downloads
mkdir -p "$dir" || { echo "couldn't make $dir" >&2; exit 1; }
out=$dir/frametop-debug-$(date +%Y%m%d-%H%M%S).txt
raw=$(mktemp) || exit 1
trap 'rm -f "$raw"' EXIT

# show FILE [BYTES]: a file, or why it isn't there
show() {
  if [ -f "$1" ]; then
    echo "--- $1 ($(stat -c '%s bytes, changed %y' "$1" 2>/dev/null | cut -d. -f1))"
    head -c "${2:-30000}" "$1"
    echo
  else
    echo "--- $1: not there"
  fi
}

{
  section "The headset"
  grep -E '^(PRETTY_NAME|VERSION_ID|BUILD_ID|VARIANT_ID)=' /etc/os-release
  echo "SteamOS branch: $(t 10 steamos-select-branch -c || echo unknown)"
  echo "kernel: $(uname -srm)"
  echo "model: $(tr -d '\0' 2>/dev/null </sys/firmware/devicetree/base/model || echo unknown)"
  echo "up $(uptime -p 2>/dev/null | sed 's/^up //'), since $(uptime -s 2>/dev/null)"
  echo "load: $(cut -d' ' -f1-3 /proc/loadavg)"
  free -m | sed -n 1,3p
  df -h / /home "$HOME" 2>/dev/null | awk 'NR == 1 || !seen[$1]++'
  for z in /sys/class/thermal/thermal_zone*; do
    echo "$(cat "$z/temp" 2>/dev/null) $(cat "$z/type" 2>/dev/null)"
  done | sort -rn | head -n 3 | awk '$1 != "" { printf "%.1f C %s\n", $1 / 1000, $2 }'
  for b in /sys/class/power_supply/*; do
    [ -r "$b/capacity" ] && echo "battery ${b##*/}: $(cat "$b/capacity")% $(cat "$b/status" 2>/dev/null)"
  done
  pw=$(passwd -S "$(id -un)" 2>/dev/null | awk '{print $2}')
  echo "sudo: $(case $pw in P) echo 'a password is set' ;; NP) echo 'no password set yet' ;; L) echo 'the password is locked' ;; *) echo "unknown ($pw)" ;; esac)"
  echo "user: $(id -un), groups: $(id -Gn)"
  if [ "$offline" != 1 ]; then
    echo "GitHub: $(t 15 curl -s -o /dev/null -w 'HTTP %{http_code} in %{time_total} s' https://api.github.com/repos/Frametop/frametop/releases/latest || echo 'no answer')"
  fi

  section "SteamVR"
  last=$(grep -a -o 'vrcompositor [0-9.]* startup' "$logs/vrcompositor.txt" 2>/dev/null | tail -n 1)
  echo "build: $(tr -d '\r\n' 2>/dev/null </opt/steamvr/bin/version.txt || echo unknown), last start: ${last:-not found}"
  units steamvr.service
  XDG_CONFIG_HOME=$HOME/.config LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 \
    t 10 /opt/steamvr/bin/linuxarm64/vrpathreg show 2>/dev/null | sed -n '/xternal/,$p'

  section "Frametop Installer"
  echo "this one: ${inst:-run by hand}"
  t 15 flatpak list --app --columns=application,version,branch,installation 2>/dev/null | grep -i frametop || echo "no Frametop Installer flatpak"
  t 15 flatpak info io.github.frametop.Installer 2>/dev/null | grep -E 'Ref|Installation|Runtime|Commit|Installed'
  echo "GNOME runtime 51: $(t 15 flatpak info org.gnome.Platform//51 >/dev/null 2>&1 && echo installed || echo 'not installed')"
  echo "its install service: $(systemctl --user is-active frametop-installer 2>/dev/null || true)," \
    "FrameDrop's: $(systemctl --user is-active frametop-framedrop-install 2>/dev/null || true)"
  show "$cache/state.json" 4000
  show "$cache/result" 100
  echo "--- the last install's log (last 200 lines)"
  # (curl's download meter is carriage returns: keep the last of each line)
  if [ -f "$cache/install.log" ]; then tail -n 200 "$cache/install.log" | sed 's/.*\r//'; else echo "no log"; fi

  section "Frametop"
  if [ -d "$HOME/.local/share/frametop/releases" ]; then
    ls -1 "$HOME/.local/share/frametop/releases" | sed 's/^/release folder: /'
  else
    echo "no releases installed"
  fi
  echo "current: $(readlink "$rel" 2>/dev/null || echo none)"
  show "$rel/.frametop-release" 2000
  if [ -d "$HOME/frametop/.git" ]; then
    echo "a git checkout in ~/frametop: $(git -C "$HOME/frametop" log -1 --format='%h %cd' --date=short 2>/dev/null)," \
      "branch $(git -C "$HOME/frametop" branch --show-current 2>/dev/null)," \
      "$(git -C "$HOME/frametop" status --porcelain 2>/dev/null | wc -l) changed file(s)"
    git -C "$HOME/frametop" remote -v 2>/dev/null | head -n 4
  fi
  for u in frametop-desktop frametop-pointer frametop-input-relay; do
    echo "$u runs from: $(systemctl --user cat "$u" 2>/dev/null | grep -m 1 -o -E "$HOME/[^ ]+" || echo '(no unit)')"
  done
  echo "distrobox: $(t 10 "$HOME/.local/bin/distrobox" version 2>/dev/null || echo 'not installed')"
  echo "podman: $(t 10 podman --version || echo 'not installed')"
  t 20 podman ps -a --format '{{.Names}}  {{.Image}}  {{.Status}}' 2>/dev/null | sed 's/^/container: /'
  t 20 podman images --format '{{.Repository}}:{{.Tag}}  {{.ID}}  {{.Size}}' 2>/dev/null | grep -i frametop | sed 's/^/image: /'

  section "Services"
  # (every one installed, from its unit file: a feature added later is listed too)
  mapfile -t user_units < <(systemctl --user list-unit-files --plain --no-legend 'frametop*' 'frame-voice*' 2>/dev/null |
    awk '$1 !~ /@/ {print $1}')
  mapfile -t system_units < <(systemctl list-unit-files --plain --no-legend 'frametop*' 'steamframe*' 'ft-npud*' \
    'dsp_service*' 2>/dev/null | awk '$1 !~ /@/ {print $1}')
  [ ${#user_units[@]} -gt 0 ] && units "${user_units[@]}" || echo "no Frametop services installed"
  [ ${#system_units[@]} -gt 0 ] && sunits "${system_units[@]}"
  echo "failed user units: $(systemctl --user --failed --no-legend --plain 2>/dev/null | awk '{print $1}' | xargs)"
  echo "failed system units: $(systemctl --failed --no-legend --plain 2>/dev/null | awk '{print $1}' | xargs)"

  section "What's running"
  for p in vrserver vrcompositor vrmonitor ft-screens kwin_wayland plasmashell; do
    echo "$p: $(pgrep -c -x "$p" 2>/dev/null)"
  done
  echo "Frametop processes (without podman's and distrobox's own):"
  pgrep -a -f '/frametop/|/frametop-|ft-npud' 2>/dev/null |
    grep -v -E '^[0-9]+ ([^ ]*/)?(conmon|podman exec|sh [^ ]*distrobox enter)( |$)' |
    cut -c1-200 | head -n 40 | sed 's/^/  /'
  echo "the busiest processes now:"
  t 10 top -b -n 1 -w 160 | sed -n '7,22p'

  section "Settings"
  echo "--- ~/.config/frametop.conf (without comments)"
  grep -v '^\s*#' "$HOME/.config/frametop.conf" 2>/dev/null | sed 's/\s*#.*//' | grep . || echo "not there"
  # (not frametop-float.json or frametop-frunner.json: they name the web apps and sites used; the
  # input relay's command: actions are hidden by the mask, and the user's own commands go in by name)
  for f in frametop-input.json frametop-layout.json frametop-voice.json frametop-palm.txt frametop-tiles.txt \
    frametop-bars.txt; do
    show "$HOME/.config/$f" 20000
  done
  f=$HOME/.config/frametop-commands.json
  if [ -f "$f" ]; then
    echo "--- $f: the names only"
    python3 -I -c 'import json, sys
cs = json.load(open(sys.argv[1], encoding="utf-8")).get("commands", [])
print(", ".join(str(c.get("name", "?")) for c in cs) or "none")' "$f" 2>&1 | tail -n 1
  else
    echo "--- $f: not there"
  fi
  echo "--- ~/.config/frametop* files: $(cd "$HOME/.config" 2>/dev/null && ls -d frametop* 2>/dev/null | xargs)"

  section "Recent warnings and errors"
  echo "--- this user's, from the desktop, SteamVR, audio and Frametop (last 80, the last 2 hours)"
  t 20 journalctl --user -p warning --since -2h -n 400 --no-pager -o short 2>/dev/null |
    grep -a -E "$OURS" | grep -a -v -E "$NOISE" | tail -n 80
  echo "--- the system's (last 40, the last 2 hours)"
  t 20 journalctl -p err --since -2h -n 40 --no-pager -o short 2>/dev/null | tail -n 40
  echo "--- SteamVR server: Frametop and errors (last 40)"
  grep -a -i -E 'ft_pointer|frametop|\[error\]' "$logs/vrserver.txt" 2>/dev/null | tail -n 40
  echo "--- /tmp/frametop-session.log: Frametop's, D-Bus's and the portals' lines (last 40)"
  sessionlog /tmp/frametop-session.log 40

  t 150 bash "$here/debug-features.sh"
  [ $? -eq 124 ] && printf '\n(the feature logs stopped at 150 seconds: the rest of them is missing)\n'

  if [ -f "$rel/scripts/report.sh" ]; then
    section "Frametop's own report (scripts/report.sh --system --desktop --gaze --hands)"
    r=$(t 90 bash "$rel/scripts/report.sh" --system --desktop --gaze --hands </dev/null)
    made=$(printf '%s\n' "$r" | sed -n 's/^wrote //p' | tail -n 1)
    if [ -n "$made" ] && [ -f "$made" ]; then
      # (an older report.sh has the whole session log, apps' lines too: this report has its own)
      awk '/^===== /{skip = /^===== \/tmp\/frametop-session\.log/; if (skip) print $0 "\n(left out here: it is above)"}
           !skip' "$made"
      rm -f "$made"
    else
      echo "it didn't finish:"
      printf '%s\n' "$r" | tail -n 20
    fi
  fi
} >"$raw" 2>&1 </dev/null

# Hide what the reader doesn't need; then the header, which has the address to send it to.
{
  echo "Frametop debug report, $(date '+%F %T %Z')"
  echo "Send this file to Deej__ on Discord (a direct message), or email it to dj_anuz@protonmail.com,"
  echo "with what you did, what you expected, and what happened."
  python3 -I "$here/debug-mask.py" "$raw" "$(cat /proc/sys/kernel/hostname 2>/dev/null)"
} >"$out" || { echo "couldn't write $out" >&2; exit 1; }
echo "wrote $out"
