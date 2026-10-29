#!/usr/bin/env bash
# Collect what a Frametop bug report needs into one text file. In a terminal on the Frame, or from
# a PC over SSH:
#   scripts/report.sh [--system] [--desktop] [--gaze] [--hands] [--voice] [--watch [SECONDS]]
# With no area, all of them. Every report has the versions and the settings (~/.config/frametop.conf).
#   --system   services, what Frametop needs from SteamOS, CPU, memory, heat, the busiest processes,
#              the session's log, SteamVR's errors
#   --desktop  the screens' layout, Plasma's panels, our keyboard and the Steam menu, the input
#              relay's, pointer helper's and power service's logs, ft-screens' log
#   --gaze     (or --eyes) the gaze report (scripts/gaze-report.py): SteamVR's eye tracker and ours,
#              calibrations, the gaze service; it wakes an idle gaze service for about 20 s
#   --hands    hand tracking: its services, models, cameras, side cameras, ft-hands' counts, ft-camd
#   --voice    Frametop Voice: its services, state, settings, speech models, and the microphone: its
#              gain, PipeWire's capture chain, and its level over one second, which whisper-server
#              then transcribes if it runs (no sound or text is kept, only the word count)
#   --watch    then record SECONDS (default 60, 5 to 600) of the chosen areas while you make the
#              problem happen (scripts/report-watch.py), and their logs from that time
# Left out: what was said to Frametop Voice (its word counts stay). Bluetooth addresses and the
# headset's serial number are masked.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
all=system,desktop,gaze,hands,voice
areas= watch=0
usage() { echo "usage: $0 [--system] [--desktop] [--gaze] [--hands] [--voice] [--watch [SECONDS]]  (-h: more)" >&2; exit 2; }
while [ $# -gt 0 ]; do
  case $1 in
    --system | --desktop | --gaze | --hands | --voice) areas+=${areas:+,}${1#--} ;;
    --eyes) areas+=${areas:+,}gaze ;;
    --watch)
      watch=60
      if [[ ${2:-} =~ ^[0-9]+$ ]]; then watch=$2; shift; fi
      [ "$watch" -ge 5 ] && [ "$watch" -le 600 ] || { echo "--watch takes 5 to 600 seconds" >&2; exit 2; } ;;
    -h | --help) sed -n '2,21p' "$0"; exit 0 ;;
    *) usage ;;
  esac
  shift
done
areas=${areas:-$all}
out=$root/frametop-report-$(date +%Y%m%d-%H%M%S)$([ "$areas" = "$all" ] || echo "-${areas//,/-}").txt
echo "Collecting: ${areas//,/, }$([[ ,$areas, == *,gaze,* ]] && echo " (up to half a minute: the gaze service is woken to see whether the eye tracker sends)")..."

on_frame_script "$FRAME_REPO" "$areas" > "$out" 2>&1 <<'EOF' || true
repo=$1 areas=$2
export XDG_RUNTIME_DIR=/run/user/$(id -u) DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus
logs=~/.local/share/Steam/logs
has() { [[ ,$areas, == *,$1,* ]]; }
section() { printf '\n===== %s\n' "$*"; }
units() { for u in "$@"; do echo "$u: $(systemctl --user is-enabled $u 2>/dev/null) / $(systemctl --user is-active $u 2>/dev/null)"; done; }
# podman's lines for each command run in the container, and ft-hands' pinch and grip lines
noise='container (exec|exec_died|init|start|died|attach|cleanup)'
# ask SOCKET COMMAND: a request to one of Frametop's abstract datagram sockets; prints the reply.
ask() {
  python3 -I - "$1" "$2" <<'PY' 2>&1
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind("")
s.settimeout(2)
try:
    s.sendto(sys.argv[2].encode(), "\0" + sys.argv[1])
    print(s.recv(65536).decode())
except OSError as e:
    print(f"no answer from @{sys.argv[1]} ({e})")
PY
}

section "Versions (report: ${areas//,/, })"
grep -E '^(PRETTY_NAME|VERSION_ID|BUILD_ID|VARIANT_ID)=' /etc/os-release
# The log keeps earlier runs: the last start is this SteamVR.
grep -a -o 'vrcompositor [0-9.]* startup' $logs/vrcompositor.txt 2>/dev/null | tail -n 1 | grep . || echo "SteamVR: not found"
if [ -f "$repo/.frametop-release" ]; then
  rel() { sed -n "s/^$1=//p" "$repo/.frametop-release" | tail -1; }
  echo "Frametop release $(rel VERSION) ($(rel COMMIT | cut -c1-7), $(rel CHANNEL)) in $repo"
  box=$(rel BOX)
else
  git -C "$repo" log -1 --format='Frametop %h (%cd)' --date=short 2>/dev/null || echo "Frametop: not a git checkout"
  git -C "$repo" branch --show-current 2>/dev/null | sed 's/^/branch: /'
  box=${FRAME_BOX:-dev}
fi
~/.local/bin/distrobox version 2>/dev/null || echo "distrobox: not installed"
podman container inspect -f "container $box: running={{.State.Running}} image={{.ImageName}}" "$box" 2>/dev/null ||
  echo "container $box: missing"
echo "up $(uptime -p 2>/dev/null | sed 's/^up //'), since $(uptime -s 2>/dev/null)"

section Settings
grep -v '^\s*#' ~/.config/frametop.conf 2>/dev/null | sed 's/\s*#.*//' | grep . || echo "no ~/.config/frametop.conf"

if has system; then
  section Services
  units frametop-input-relay frametop-pointer frametop-power frametop-gaze frametop-desktop frametop-voice \
    frametop-voice-whisper frametop-camd frametop-hands frametop-handview
  for u in frametop-eyegrab steamframe-bt-fixups; do
    echo "$u (system): $(systemctl is-enabled $u 2>/dev/null) / $(systemctl is-active $u 2>/dev/null)"
  done
  echo "failed user units: $(systemctl --user --failed --no-legend --plain 2>/dev/null | awk '{print $1}' | xargs | grep . || echo none)"
  echo "desktop: $(pgrep -x ft-screens >/dev/null && echo running || echo 'not running'), plasmashell: $(pgrep -c plasmashell || true)"
  echo "paused for VR games: $(cat /run/user/$(id -u)/frametop-pause.json 2>/dev/null || echo 'no (never paused since boot)')"
  # SteamVR's config folder: a terminal in the desktop has the session's own (docs/design.md).
  XDG_CONFIG_HOME=$HOME/.config LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 /opt/steamvr/bin/linuxarm64/vrpathreg show 2>/dev/null | sed -n '/xternal/,$p'

  section "What Frametop needs from SteamOS (scripts/update-check.py)"
  python3 "$repo/scripts/update-check.py" 2>&1 || true

  section "CPU, memory, heat"
  echo "load: $(cut -d' ' -f1-3 /proc/loadavg)"
  free -m | sed -n 1,2p
  df -h / /home 2>/dev/null | awk 'NR == 1 || !seen[$1]++'
  for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq; do
    c=${f%/cpufreq/*}; printf '%s %s/%s MHz  ' "${c##*/}" $(( $(cat $f) / 1000 )) $(( $(cat ${f%/*}/cpuinfo_max_freq) / 1000 ))
  done; echo
  for z in /sys/class/thermal/thermal_zone*; do
    echo "$(cat $z/temp 2>/dev/null) $(cat $z/type 2>/dev/null)"
  done | sort -rn | head -n 5 | awk '{ printf "%.1f C %s\n", $1 / 1000, $2 }'
  echo "the busiest processes now:"
  top -b -n 1 -w 160 2>/dev/null | sed -n '7,17p'

  section "/tmp/frametop-session.log (last 60 lines)"
  tail -n 60 /tmp/frametop-session.log 2>/dev/null || echo "missing"
  section "SteamVR server: Frametop and errors (last 60 lines)"
  grep -a -i -E 'ft_pointer|frametop|\[error\]' $logs/vrserver.txt 2>/dev/null | tail -n 60
fi

if has desktop; then
  section "Screens' layout"
  python3 - <<'PY' 2>/dev/null || echo "no ~/.config/frametop-layout.json"
import json, os
d = json.load(open(os.path.expanduser("~/.config/frametop-layout.json")))
print("mode:", d.get("mode"), " auto:", d.get("auto"), " primary:", d.get("primary"))
for i, s in enumerate(d.get("screens", []), 1):
    print(f"screen {i}: {s.get('size')} {s.get('metres')} m curve={s.get('curve', 0)} pinned={s.get('pin', {}).get('hand', '-') if isinstance(s.get('pin'), dict) else '-'}")
print("visibility:", d.get("visibility"))
PY

  section "Plasma panels and outputs"
  python3 "$repo/session/fix-panels.py" --check 2>&1 || true
  python3 - "$repo" <<'PY' 2>&1 || true
import json, subprocess, sys
sys.path.insert(0, sys.argv[1] + "/layout")
import ft_layout
env = ft_layout.nested_env()
if not env:
    sys.exit(print("desktop not running: no live outputs or panels"))
outs = json.loads(subprocess.run(["kscreen-doctor", "-j"], capture_output=True, text=True, env=env, timeout=10).stdout or "{}")
for o in outs.get("outputs", []):
    size = o.get("size") or {}
    print(f"output {o.get('name')}: enabled={o.get('enabled')} priority={o.get('priority')} {size.get('width')}x{size.get('height')}")
js = "print(JSON.stringify(panels().map(p => ({id: p.id, screen: p.screen, location: p.location}))))"
r = subprocess.run(["qdbus6", "org.kde.plasmashell", "/PlasmaShell", "org.kde.PlasmaShell.evaluateScript", js],
                   capture_output=True, text=True, env=env, timeout=10)
print("live panels:", (r.stdout or r.stderr).strip())
PY

  section "Frametop's keyboard and the Steam menu"
  python3 - "$(ask frametop_relay devices)" <<'PY' 2>&1
import json, os, sys
try:
    rules = json.load(open(os.path.expanduser("~/.config/frametop-input.json")))
except (OSError, ValueError):
    rules = {}
print("Keyboard setting (Input Settings > Keyboard):", rules.get("vr_keyboard", "no_keyboard (the default)"),
      "| keep it open:", rules.get("vr_keyboard_persist", True))
print("the relay's devices (with the default setting, a pass-through keyboard keeps ours closed):")
try:
    d = json.loads(sys.argv[1])
except ValueError:
    sys.exit(print("  " + sys.argv[1].strip()))
print(f"  pointer mode: {d.get('pointer_mode')}, paused: {d.get('paused')}")
for n in d.get("nodes", []):
    print("  " + json.dumps(n))
PY
  echo "ft-textinput (tells the relay about text fields): $(pgrep -f '[f]t-textinput' >/dev/null && echo running || echo 'not running')"
  echo "the desktop's KWin input method: $(pgrep -a kwin_wayland | grep -o -- '--inputmethod [^ ]*' | head -n 1 | grep . || echo 'none (the keyboard never opens for text fields)')"
  for p in $(pgrep -x plasmashell); do
    env=$(tr '\0' '\n' < /proc/$p/environ 2>/dev/null) || continue
    echo "$env" | grep -q '^XDG_CONFIG_HOME=.*/frametop$' || continue
    for v in QT_IM_MODULE GTK_IM_MODULE; do  # xim here: Qt and GTK apps never ask for the keyboard
      printf "the desktop's %s: %s\n" $v "$(echo "$env" | sed -n "s/^$v=//p" | grep . || echo unset)"
    done
  done
  echo "ft-screens debug: $(ask ft_screens debug)"
  echo "ft-screens state (mode, manual, wrist, gesture hand and angle, lasers, game, in games): $(ask ft_screens state)"
  echo "ft-screens pause: $(ask ft_screens 'pause state'), concealed: $(ask ft_screens concealed)"
  echo "SteamVR overlays shown now:"
  XDG_CONFIG_HOME=$HOME/.config LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 timeout 10 \
    /opt/steamvr/bin/linuxarm64/vrcmd --overlays 2>/dev/null |
    awk '/^\x27/ { show = $0 !~ /not_visible/ } show' | sed 's/^/  /' | head -n 80
  echo "keyboard, drag and Steam menu lines from ft-screens (last 40):"
  grep -a -E 'keyboard|Steam (in front|out of the way)|: (move|resize|roll)( by| ended| not started)' \
    "$HOME/.local/state/frametop/screens.log" 2>/dev/null | tail -n 40 | sed 's/^/  /'
  echo "tracking: what SteamVR did to standing, and screens that followed a universe change (DEE-206; last 60):"
  grep -a -h -E '^==== ft-screens started|trackwatch:|^universe ' "$HOME/.local/state/frametop/screens.log.1" \
    "$HOME/.local/state/frametop/screens.log" 2>/dev/null | tail -n 60 | sed 's/^/  /'
  echo "text fields, from the relay (last 20):"
  journalctl --user -u frametop-input-relay --no-pager -o short 2>/dev/null | grep 'text field' | tail -n 20 | sed 's/^/  /'

  section "Input relay (last 60 lines)"
  journalctl --user -u frametop-input-relay -n 60 --no-pager -o short 2>/dev/null
  section "Pointer helper (last 60 lines)"
  journalctl --user -u frametop-pointer -n 60 --no-pager -o short 2>/dev/null
  section "Power service (last 30 lines)"
  journalctl --user -u frametop-power -n 30 --no-pager -o short 2>/dev/null
  for f in "$HOME/.local/state/frametop/screens.log" "$XDG_RUNTIME_DIR/frametop-layout.log"; do
    section "$f (last 60 lines)"
    tail -n 60 "$f" 2>/dev/null || echo "missing"
  done
fi

# The gaze service, SteamVR's eye tracker and ours, the checks and calibrations, with what
# looks wrong first. It wakes an idle gaze service for about 20 s, to see the tracker send.
if has gaze; then
  python3 "$repo/scripts/gaze-report.py" </dev/null 2>&1 || echo "scripts/gaze-report.py failed"
  python3 "$repo/models/ft-models" list eyes 2>&1 | head -n 12
fi

# Hand tracking: whether it runs, which models ft-hands loaded and whether they're the pinned ones,
# the cameras (hands/camcheck.py), and ft-hands' own counts: how often each hand was seen, what it
# lost, how long a step took. Its pinch and grip lines are left out (the counts are in the status).
if has hands; then
  section "Hand tracking"
  python3 "$repo/hands/handsmode.py" status 2>&1 || true
  units frametop-camd frametop-hands frametop-handview
  student=$repo/hands/models/student
  echo "fine-tuned models: $(cat "$student/VERSION" 2>/dev/null || echo 'none in this install (then the stock MediaPipe ones)')"
  [ -d "$student" ] && "$repo/hands/models.sh" check 2>&1 | tail -n 3
  echo "last checked for newer ones: $(date -r "$repo/hands/models/.student-checked" '+%F %T' 2>/dev/null || echo never)"
  grep -E '^\s*(FT_)?HANDS_MODELS(_UPDATE)?=' ~/.config/frametop.conf 2>/dev/null | sed 's/\s*#.*//; s/^/override (ft-hands loads these instead): /'
  python3 "$repo/models/ft-models" list hands 2>&1 | head -n 12
  python3 "$repo/hands/camcheck.py" 2>&1 | head -n 30
  echo "sides: $(cat "$XDG_RUNTIME_DIR/frametop-hands/sides.json" 2>/dev/null || echo 'no sides.json')"
  for c in 5 6 7; do   # ft-hands' CPUs (HANDS_CPUS)
    f=/sys/devices/system/cpu/cpu$c/cpufreq
    [ -r $f/scaling_cur_freq ] && echo "cpu$c: $(( $(cat $f/scaling_cur_freq) / 1000 )) of $(( $(cat $f/cpuinfo_max_freq) / 1000 )) MHz"
  done
  echo "hottest thermal zone: $(cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | sort -n | tail -n 1 | awk '{printf "%.1f C", $1 / 1000}')"
  gestures='pinch (left|right)|grip  (left|right)'
  echo "ft-hands' starts, cameras and side decisions (last 20):"
  journalctl --user -u frametop-hands -n 5000 --no-pager -o short 2>/dev/null | grep -a -v -E "$noise|$gestures" |
    grep -a -E 'cameras|models|colour|side cameras|hand model|heartbeat|error|failed|Failed|Started|not ' | tail -n 20 | sed 's/^/  /'
  echo "ft-hands' last status (every minute):"
  journalctl --user -u frametop-hands -n 5000 --no-pager -o cat 2>/dev/null | grep -a -v -E "$noise|$gestures" |
    awk '/^[0-9.]+s +[0-9.]+ sets\/s/ { block = $0; next } /^ +(sets with|flag buffer|pinches)/ { block = block "\n" $0 }
         END { print block }' | sed 's/^/  /'
  # ft-camd prints each camera's rate once a minute: its other lines (starts, cameras, errors), then
  # only the last 3 of those.
  section "Camera broker ft-camd (last 20 lines but its rates, then its last 3 rates)"
  journalctl --user -u frametop-camd -n 3000 --no-pager -o short 2>/dev/null | grep -a -v -E "$noise" > /tmp/ft-report-camd.$$
  grep -a -v -E '\]: \[[0-9]+ s\] ' /tmp/ft-report-camd.$$ | tail -n 20
  grep -a -E '\]: \[[0-9]+ s\] ' /tmp/ft-report-camd.$$ | tail -n 3
  rm -f /tmp/ft-report-camd.$$
fi

# Frametop Voice. Its log has what was said (each dictation window's words, transcripts, wake word
# commands): those lines are left out. The microphone: SteamVR's proxmicmute sets the mic's gain
# (VA_DEC0/1) to 0 while nobody wears the headset, and PipeWire's echo canceller can fail at boot,
# which leaves everything after it silent; the level check reads one second and keeps only its level.
if has voice; then
  section "Voice"
  units frametop-voice frametop-voice-whisper steamvr-proxmicmute
  # frame-voice, the standalone project Frametop Voice came from, uses the same mic and port
  for u in frame-voice frame-voice-whisper; do
    systemctl --user cat $u >/dev/null 2>&1 && echo "standalone $(units $u)"
  done
  echo "state: $(timeout 5 python3 "$repo/voice/ft-voice" status 2>&1 | tr -d '\n')"
  python3 - "$repo" <<'PY' 2>&1 || true
import sys
sys.path.insert(0, sys.argv[1] + "/voice")
import voice_settings as v
s = v.load()
print(f"settings ({v.PATH}{'' if v.PATH.exists() else ', not there: the defaults'}): controller {s['controller']} "
      f"{s['controller_mode']}, live typing {s['live_typing']}, sounds {s['sounds']}, wake word {s['wake_word']} "
      f"({len(s['wake_phrases'])} phrase(s)), {len(s['commands'])} command(s), advanced: {sorted(s['advanced']) or 'none'}")
PY
  [ -x "$repo/voice/models.sh" ] && timeout 60 "$repo/voice/models.sh" check 2>&1 | tail -n 4
  echo "the mic's gain (0 = muted by proxmicmute, the headset off):" \
    "VA_DEC0 $(amixer -c 0 cget numid=23 2>/dev/null | sed -n 's/.*: values=//p')," \
    "VA_DEC1 $(amixer -c 0 cget numid=24 2>/dev/null | sed -n 's/.*: values=//p')"
  r=$(ask ft_gazed status)
  echo "headset worn (the gaze service): $(echo "$r" | python3 -c 'import json, sys; print(json.load(sys.stdin).get("headset_on"))' 2>/dev/null || echo unknown)"
  echo "default source: $(wpctl inspect @DEFAULT_AUDIO_SOURCE@ 2>/dev/null | sed -n 's/.*node.name = "\(.*\)"/\1/p' | head -n 1)," \
    "$(wpctl get-volume @DEFAULT_AUDIO_SOURCE@ 2>/dev/null)"
  echo "PipeWire's capture chain (state, error):"
  pw-dump 2>/dev/null | python3 -c '
import json, sys
for o in json.load(sys.stdin):
    if o.get("type") == "PipeWire:Interface:Node":
        i = o.get("info") or {}
        p = i.get("props") or {}
        if p.get("media.class") in ("Audio/Source", "Stream/Input/Audio"):
            name, state, error = p.get("node.name"), i.get("state"), i.get("error")
            print(f"  {name}: {state}" + (f", {error}" if error else ""))
' 2>&1 || echo "  pw-dump failed"
  echo "who has the capture device open: $(fuser /dev/snd/pcmC*c 2>/dev/null | xargs -r -n 1 sh -c 'cat /proc/$0/comm' 2>/dev/null | sort | uniq -c | xargs | grep . || echo nobody)"
  clip=$(mktemp --suffix=.wav)
  python3 -I - "$clip" <<'PY' 2>&1
import math, subprocess, sys, wave
from array import array
try:
    raw = subprocess.run(["timeout", "1.2", "pw-record", "--raw", "--rate", "16000", "--channels", "1", "--format", "s16", "-"],
                         capture_output=True).stdout
except OSError as e:
    raise SystemExit(f"mic level: can't run pw-record ({e})")
a = array("h", raw[: len(raw) // 2 * 2])
if not a:
    raise SystemExit("mic level: no samples from the default source")
peak = max(abs(x) for x in a) / 32768
rms = math.sqrt(sum(x * x for x in a) / len(a)) / 32768
print("mic level over 1 s: " + ("digital silence (all zeros)" if peak == 0 else
      f"peak {20 * math.log10(peak):.0f} dBFS, RMS {20 * math.log10(max(rms, 1e-9)):.0f} dBFS"))
with wave.open(sys.argv[1], "wb") as w:  # for the whisper-server check below
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(16000)
    w.writeframes(a.tobytes())
PY
  # whisper-server gets that second of the mic, as ft-voiced sends it (audio_ctx 512, its floor).
  # Only the answer's status, time and word count are kept, never its text.
  whisper=http://127.0.0.1:8178
  if ! curl -s -o /dev/null --max-time 2 $whisper/; then
    echo "whisper-server: not listening on ${whisper#http://} (it runs only while the wake word is on, or with the whisper engine)"
  elif [ ! -s "$clip" ]; then
    echo "whisper-server: listening, not tried (no clip from the mic)"
  else
    r=$(curl -sS --max-time 60 $whisper/inference -F file=@"$clip" -F response_format=json -F temperature=0 \
      -F temperature_inc=0 -F no_timestamps=true -F language=en -F audio_ctx=512 -w '\n%{http_code} %{time_total}' 2>&1)
    python3 -I - "$r" "$clip" <<'PY' 2>&1
import json, re, sys, wave
body, _, tail = sys.argv[1].rpartition("\n")
code, _, secs = tail.partition(" ")
try:
    text = json.loads(body).get("text", "")
except ValueError:
    raise SystemExit(f"whisper-server: HTTP {code} after {secs} s, not JSON ({len(body)} bytes)")
words = len(re.sub(r"\[[^\]]*\]|\([^)]*\)", "", text).split())  # as ft-voiced: [BLANK_AUDIO] isn't a word
with wave.open(sys.argv[2]) as w:
    silent = not any(w.readframes(w.getnframes()))
print(f"whisper-server: HTTP {code} in {float(secs or 0):.2f} s for 1 s of the mic, {words} word(s)"
      + (" (the clip was silence: words are whisper's guesses)" if silent and words else ""))
PY
  fi
  rm -f "$clip"
  echo "PipeWire and WirePlumber errors (last 15):"
  journalctl --user -u pipewire -u wireplumber -n 3000 --no-pager -o short 2>/dev/null | grep -a -i -E 'error|cannot|failed' | tail -n 15 | sed 's/^/  /'
  section "Voice dictation (last 30 lines, without what was said)"
  journalctl --user -u frametop-voice -u frametop-voice-whisper -u frame-voice -u frame-voice-whisper -n 200 --no-pager -o short 2>/dev/null | grep -a -v -E "$noise" |
    grep -v -E 'transcribed|voice command|wake word:|window [0-9.]+s|decode skipped|line up with' | tail -n 30
fi
EOF

if [ "$watch" -gt 0 ]; then
  echo
  echo "Now make the problem happen within $watch seconds. For example:"
  [[ ,$areas, == *,desktop,* ]] && echo "  desktop: open the Steam menu, drag a window by its bar, click into a text field"
  [[ ,$areas, == *,gaze,* ]] && echo "  gaze: point with your eyes, at what goes wrong"
  [[ ,$areas, == *,hands,* ]] && echo "  hands: pinch and tap, move your hands in and out of view, where tracking fails"
  [[ ,$areas, == *,voice,* ]] && echo "  voice: dictate, or say your wake word (only the mic's level and word counts are kept)"
  echo "Recording..."
  on_frame_script "$FRAME_REPO" "$areas" "$watch" >> "$out" 2>&1 <<'EOF' || true
repo=$1 areas=$2 seconds=$3
has() { [[ ,$areas, == *,$1,* ]]; }
noise='container (exec|exec_died|init|start|died|attach|cleanup)'
since=$(date '+%F %T')
screens_log=$HOME/.local/state/frametop/screens.log
start=$(wc -l < "$screens_log" 2>/dev/null || echo 0)
printf '\n===== Watched %s for %s s from %s\n' "${areas//,/, }" "$seconds" "$since"
if [ -f "$repo/scripts/report-watch.py" ]; then
  python3 -I "$repo/scripts/report-watch.py" "$seconds" "$areas"
else
  echo "scripts/report-watch.py isn't in this install (older than 0.3.0-exp.12): logs only"
  sleep "$seconds"
fi
log() {  # log TITLE UNIT...: their journal while watching
  local title=$1; shift
  printf '\n===== %s while watching\n' "$title"
  journalctl --user $(printf -- '-u %s ' "$@") --since "$since" --no-pager -o short 2>/dev/null | grep -a -v -E "$noise" | tail -n 300
}
if has system; then
  printf '\n===== warnings and errors while watching\n'
  journalctl --user -p warning --since "$since" --no-pager -o short 2>/dev/null | grep -a -v -E "$noise" | tail -n 100
  journalctl -k -p warning --since "$since" --no-pager -o short 2>/dev/null | tail -n 50
fi
if has desktop; then
  printf '\n===== ft-screens log while watching\n'
  tail -n +"$((start + 1))" "$screens_log" 2>/dev/null | tail -n 300
  log "input relay" frametop-input-relay
  log "pointer helper" frametop-pointer
fi
if has gaze; then
  log "gaze service" frametop-gaze
  has desktop || { printf '\n===== the pointer helper'"'"'s gaze lines while watching\n'
    journalctl --user -u frametop-pointer --since "$since" --no-pager -o short 2>/dev/null | grep -a -i gaze | tail -n 100; }
fi
if has hands; then
  log "ft-hands (pinches and grips too)" frametop-hands
  log "ft-camd" frametop-camd
fi
if has voice; then
  printf '\n===== Frametop Voice while watching (without what was said)\n'
  journalctl --user -u frametop-voice -u frametop-voice-whisper -u frame-voice -u frame-voice-whisper --since "$since" --no-pager -o short 2>/dev/null |
    grep -a -v -E "$noise" | grep -v -E 'transcribed|voice command|wake word:|window [0-9.]+s|decode skipped|line up with' | tail -n 200
  log "PipeWire, WirePlumber and proxmicmute" pipewire wireplumber steamvr-proxmicmute
fi
EOF
  echo "Recorded."
fi

sed -i -E 's/([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}/xx:xx:xx:xx:xx:xx/g; s/cv\.[A-Z0-9]{8,}/cv.<serial>/g' "$out"
echo "wrote $out"
echo "Attach it to an issue at https://github.com/Frametop/frametop/issues, with what you did, what you expected, and what happened."
