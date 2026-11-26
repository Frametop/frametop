#!/usr/bin/env bash
# The debug report's logs, feature by feature: each one's services and the last of its own logs,
# and NPU hand tracking's service, devices and models. debug-report.sh runs it inside a timeout,
# and masks its output with the rest of the report (debug-mask.py).
#
# Left out, as what the user said, typed or ran isn't the report's: what Frametop Voice heard
# and what its commands and hooks printed (only ft-voiced's own lines go in), the output of the
# user's own commands (only when each started and ended), apps' output in the desktop's session
# (only KWin's, Plasma's and the session's), and the names of the people in a Together room.
set -u
. "$(dirname "${BASH_SOURCE[0]}")/debug-lib.sh"
shopt -s nullglob
conf=$HOME/.config/frametop.conf
rel=$HOME/.local/share/frametop/releases/current
state=$HOME/.local/state/frametop
run=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
steamlogs=$HOME/.local/share/Steam/logs

# tailf FILE LINES: a log file's last lines, or that it isn't there
tailf() {
  if [ -f "$1" ]; then
    echo "--- $1 (last $2 lines; changed $(date -r "$1" '+%F %T'))"
    tail -n "$2" "$1" | sed 's/.*\r//'
  else
    echo "--- $1: not there"
  fi
}

# ulog LINES UNIT...: user services' last journal lines; slog: system services'
# (a unit can be a pattern, such as 'frametop-room-*': journalctl matches it, the shell doesn't)
ulog() {
  local n=$1 u args=(); shift
  for u in "$@"; do args+=(-u "$u"); done
  echo "--- journal: $* (last $n lines)"
  t 15 journalctl --user "${args[@]}" -n "$((n * 2))" --no-pager -o short | grep -a -v -E "$NOISE" | tail -n "$n"
}
slog() {
  local n=$1 u args=(); shift
  for u in "$@"; do args+=(-u "$u"); done
  echo "--- journal: $* (system; last $n lines)"
  t 15 journalctl "${args[@]}" -n "$n" --no-pager -o short
}

section "Logs: the desktop and its screens"
tailf "$state/screens.log" 150
tailf "$run/frametop-layout.log" 40
tailf "$state/ondesktop.log" 40
for f in /tmp/frametop-dashroute.log /tmp/frametop-gtksettings.log /tmp/frametop-notifyfwd.log /tmp/frametop-tour.log; do
  tailf "$f" 20
done
ulog 40 frametop-autostart frametop-keyboard frametop-dashpatch frametop-pin-lock
echo "--- journal: frametop-desktop, KWin's, Plasma's and the session's lines (last 60)"
t 15 journalctl --user -u frametop-desktop -n 3000 --no-pager -o short | grep -a -E "$OURS" | tail -n 60

section "Logs: input and the pointer"
ulog 150 frametop-input-relay
ulog 100 frametop-pointer
tailf /tmp/ft-pointer.log 60

section "Logs: power"
ulog 60 frametop-power

section "Logs: gaze and the eye tracker"
ulog 100 frametop-gaze
sunits frametop-eyegrab
slog 40 frametop-eyegrab

section "Logs: hand tracking"
units frametop-camwatch
ulog 120 frametop-hands
ulog 60 frametop-camd
ulog 30 frametop-handview frametop-camwatch

section "NPU hand tracking (ft-npud, on the DSP)"
npu=$(grep -E '^\s*HANDS_NPU(_DET)?=' "$conf" 2>/dev/null | sed 's/\s*#.*//' | xargs)
echo "frametop.conf: ${npu:-HANDS_NPU not set (the NPU when ft-npud is there, else the CPU)}"
sunits ft-npud dsp_service
for u in ft-npud dsp_service; do
  echo "$u: $(t 10 systemctl show -p LoadState -p NRestarts -p Result -p ExecMainStatus -p ActiveEnterTimestamp \
    -p InactiveEnterTimestamp "$u" | xargs)"
done
echo "ft-npud runs: $(t 10 systemctl show -p ExecStart --value ft-npud | grep -o 'argv\[\]=[^;]*' | head -n 1)"
dirs=(/run/ft-npud)
own=$(sed -n 's/^\s*HANDS_NPU=\s*//p' "$conf" 2>/dev/null | tail -n 1 | sed 's/\s*#.*//' | tr -d "\"'")
[[ $own == /* && $own != /run/ft-npud ]] && dirs+=("$own")   # (a service folder of the user's own)
for d in "${dirs[@]}"; do
  echo "--- $d (the heads file and the socket: the service is up)"
  ls -la "$d" 2>&1
done
echo "--- the DSP's devices"
ls -l /dev/cdsp /dev/adsp 2>&1
# (the service sees its tree as /var/lib/ft-npud; outside its namespace it's on the home partition)
npud=/home/.steamos/offload/var/lib/ft-npud
echo "--- $npud: its models, and where they came from"
ls -la "$npud/graphs" 2>&1 | head -n 12
head -n 20 "$npud/SOURCE" 2>&1
if [ -f "$rel/hands/handsnpu.py" ]; then
  echo "handsnpu status: $(t 10 python3 -I "$rel/hands/handsnpu.py" status --json)"
fi
echo "--- ft-hands' NPU lines (last 20)"
t 15 journalctl --user -u frametop-hands -n 5000 --no-pager -o short | grep -a -i npu | tail -n 20
echo "--- journal: ft-npud (system; last 80 lines, without its clients coming and going)"
t 15 journalctl -u ft-npud -n 3000 --no-pager -o short | grep -a -v -E 'client (connected|left after)' | tail -n 80
slog 30 dsp_service
echo "--- the kernel's DSP lines, this boot (last 40)"
t 15 journalctl -k -b --no-pager -o short | grep -a -i -E 'cdsp|remoteproc|fastrpc|adsprpc|glink|hexagon|smmu.*fault' |
  tail -n 40

section "Logs: Frametop Voice (ft-voiced's own lines, without what was heard)"
# (its lines are "DATE TIME,MS LEVEL ..."; the rest is what voice commands and hooks printed. A
# hook of the user's own that logs in that same format gets in: its lines can't be told apart.)
t 15 journalctl --user -u frametop-voice -n 1000 --no-pager -o short |
  grep -a -E "$FROM (systemd\[|[^ ]+\[[0-9]+\]: [0-9-]{10} [0-9:]{8},[0-9]{3} [A-Z]+ )" |
  grep -a -v -E 'transcribed|voice command|wake word:|window [0-9.]+s|short window|decode skipped|line up with' | tail -n 100
# (whisper.cpp prints what it heard only as "[00:00.000 --> 00:02.000] ..." segments, run with -pr)
ulog 30 frametop-voice-whisper | grep -a -v -E '\[[0-9:.]+ --> [0-9:.]+\]'
echo "--- PipeWire, WirePlumber and proxmicmute: warnings (last 30)"
t 15 journalctl --user -u pipewire -u wireplumber -u steamvr-proxmicmute -p warning -n 30 --no-pager -o short

section "Logs: Frametop Together (people's names left out, yours too)"
units frametop-together
ulog 100 frametop-together 'frametop-room-*' |
  sed -E "s/(ft-together: ).* \(([^)]*)\) at /\1<you> (<login>) at /
          s/: .+ (came in|left)\.\$/: <someone> \1./
          s/You are in .+'s room\./You are in <someone>'s room./
          s/: [^ ]+ asks to come in\./: <someone> asks to come in./
          s/(in a room of [0-9]+ as ).*(, floor )/\1<you>\2/"

section "Logs: remote displays"
tailf /tmp/frametop-remote.log 40
for f in "$run"/frametop-remote-*.log; do
  tailf "$f" 40
done
tailf /tmp/frametop-vnc.log 20

section "Logs: floating windows and frunner"
tailf /tmp/frametop-floatd.log 60
tailf /tmp/frametop-frunner.log 30

section "Logs: your commands (when each started and ended; not their output)"
t 15 journalctl --user -u 'frametop-cmd-*' -n 3000 --no-pager -o short | grep -a -E ' systemd\[[0-9]+\]: ' | tail -n 30

section "Logs: FrameDrop and the Bluetooth fixes"
units frametop-framedrop-install
ulog 40 frametop-framedrop-install
for f in "$HOME"/.cache/frametop-framedrop/*.log; do
  tailf "$f" 30
done
sunits steamframe-bt-fixups
slog 20 steamframe-bt-fixups

section "Logs: SteamVR's compositor (errors, last 30)"
grep -a -i -E 'error|fail' "$steamlogs/vrcompositor.txt" 2>/dev/null | tail -n 30
