#!/usr/bin/env bash
# The hand recorder end to end, without the headset (DESIGN.md "Upload"): a short stretch of
# an ft-hands recording plays into a camera ring (ft-ringplay), a session records it with a
# short test script (session.py, the panel as ft-handpanel --no-vr, a tracking ft-hands),
# takes.py exports it, validate.py checks the export, and hub.py uploads it: a dry run by
# default, or for real to a (test) dataset with --repo.
#
#   hands/rec/rehearse.sh [--repo ID] [--capture DIR] [--from S] [--prompt-seconds N] [--keep]
#
#   --repo ID            upload for real to this dataset, as a pull request. While the texts are
#                        drafts that also needs FT_HANDREC_ALLOW_UPLOAD=1 in the environment, and
#                        ID must be a test dataset, not the real one.
#   --capture DIR        the recording to play (default the frame-hands capture
#                        rec-20260930-103803-lit); --from S: start this far into it (60)
#   --prompt-seconds N   each test step's length (3): a prompt, a two-cue sweep, no hands. The
#                        session runs in step mode, Next pressed by itself (--next-after): each
#                        step is a recording part of its own, with its 3 s countdown, so about
#                        18 s are recorded in all, a few hundred MB before compression
#   --keep               keep the temporary folders (camera images of a room: delete them after)
#
# It runs in the dev container (where ft-hands, ft-handpanel, zstd and huggingface_hub are),
# and in one frame-job scope when frame-job is installed, so it stays capped while the headset
# is worn. Everything goes in temporary folders that are deleted at the end, and every process
# it started is stopped, also on Ctrl+C. A tracking ft-hands that's already running is used as
# it is; otherwise one is started for the rehearsal (it publishes the usual hands file).
set -euo pipefail
here=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
# The recorder is the package hands.rec under the repo root: its programs run with python3 -m.
export PYTHONPATH="$(cd "$here/../.." && pwd)${PYTHONPATH:+:$PYTHONPATH}"
root=$(cd "$here/../.." && pwd)
uid=$(id -u)

if [ ! -e /run/.containerenv ]; then
  "$root/scripts/container-up.sh"
  # Ctrl+C and kill don't reach through distrobox: the rehearsal inside writes its PID to this
  # file, and a signal here is passed on to it, so it still cleans up.
  pidfile=$(mktemp "/run/user/$uid/ft-handrec-rehearse-pid.XXXXXX")
  # The real user bus, so frame-job's systemd-run reaches the host's user manager.
  "$HOME/.local/bin/distrobox" enter dev -- env DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$uid/bus" \
    XDG_RUNTIME_DIR="/run/user/$uid" FT_REHEARSE_PIDFILE="$pidfile" bash "$here/rehearse.sh" "$@" </dev/null &
  child=$! stopping=0 status=0
  pass_on() { [ -s "$pidfile" ] && kill -TERM "$(cat "$pidfile")" 2>/dev/null; return 0; }
  trap 'stopping=1; pass_on' INT TERM HUP
  while kill -0 "$child" 2>/dev/null; do
    wait "$child" && status=0 || status=$?   # (set -e would end this on a signal)
    [ "$stopping" = 0 ] || pass_on
  done
  rm -f "$pidfile"
  exit "$status"
fi
if [ -z "${FT_REHEARSE_SCOPE:-}" ] && command -v frame-job >/dev/null; then
  export FT_REHEARSE_SCOPE=1
  cd /tmp   # no .frame-job up from here: it runs locally, capped
  exec frame-job --local -- bash "$here/rehearse.sh" "$@"
fi
[ -z "${FT_REHEARSE_PIDFILE:-}" ] || echo $$ >"$FT_REHEARSE_PIDFILE"

repo="" capture="$HOME/Desktop/Projects/frame-hands/captures/rec-20260930-103803-lit" from=60 prompt_s=3 keep=0
while [ $# -gt 0 ]; do
  case $1 in
    --repo) repo=$2; shift 2 ;;
    --capture) capture=$2; shift 2 ;;
    --from) from=$2; shift 2 ;;
    --prompt-seconds) prompt_s=$2; shift 2 ;;
    --keep) keep=1; shift ;;
    -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) echo "rehearse: unknown option $1" >&2; exit 2 ;;
  esac
done

ringplay=$root/hands/build/ft-ringplay
hands=$root/hands/build/ft-hands
panel=$here/build/ft-handpanel
for b in "$ringplay" "$hands"; do
  [ -x "$b" ] || { echo "rehearse: $b isn't built: hands/build.sh --tools" >&2; exit 1; }
done
[ -x "$panel" ] || { echo "rehearse: $panel isn't built: hands/rec/build.sh" >&2; exit 1; }
[ -f "$capture/sets.bin" ] || { echo "rehearse: no recording in $capture" >&2; exit 1; }
if pgrep -x ft-handpanel >/dev/null; then
  echo "rehearse: an ft-handpanel is running (a real session?): try again when it's done" >&2
  exit 1
fi
if [ -n "$repo" ]; then
  if FT_HANDREC_DATASET="$repo" python3 -c "import sys; from hands.rec import hub; sys.exit(0 if hub.upload_allowed() else 1)"; then :; else
    echo "rehearse: the texts are drafts: a real upload needs FT_HANDREC_ALLOW_UPLOAD=1 as well as --repo, to a test dataset" >&2
    exit 1
  fi
  python3 -c "import huggingface_hub" 2>/dev/null || {
    echo "rehearse: huggingface_hub isn't installed: setup/dev-container.sh" >&2; exit 1; }
fi

# The data: the ring in the runtime folder (memory), the rest in /tmp. Both go at the end.
work=$(mktemp -d "${TMPDIR:-/tmp}/ft-handrec-rehearse.XXXXXX")
ringdir=$(mktemp -d "/run/user/$uid/ft-handrec-rehearse.XXXXXX")
base=$work/base ring=$ringdir/cam-ring logs=$work/logs
mkdir -p "$base" "$logs"
pids=()
cleanup() {
  local code=$?
  # Finish even if more signals come: frame-job stops its scope with SIGTERM to every process
  # in it when it's interrupted.
  trap '' INT TERM HUP
  trap - EXIT
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
  for p in "${pids[@]}"; do wait "$p" 2>/dev/null || true; done
  if [ "$keep" = 1 ]; then
    echo "kept: $work and $ringdir (camera images of a room: delete them when done)"
  else
    rm -rf "$work" "$ringdir"
  fi
  exit "$code"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
t0=$(date +%s)
step() { echo; echo "== $*"; }
# Long steps run in the background and are waited for: a signal then ends the wait at once,
# and cleanup stops them (bash runs traps only after a foreground command ends).
run() { "$@" & local p=$!; pids+=("$p"); wait "$p"; }
indent() { sed 's/^/  /'; }

step "1. Playing ${capture##*/} from ${from} s into $ring"
"$ringplay" "$capture" --ring "$ring" --from "$from" --to "$((from + 30))" --loop >"$logs/ringplay.log" 2>&1 &
pids+=($!)
for _ in $(seq 100); do
  python3 -c "import sys; from hands.rec import session
r = session.Ring('$ring'); sys.exit(0 if r.alive() else 1)" 2>/dev/null && break
  sleep 0.2
done
python3 -c "import sys; from hands.rec import session
r = session.Ring('$ring'); print('  cameras:', ', '.join('%s %dx%d' % (c['name'], c['width'], c['height']) for c in r.cams))
sys.exit(0 if r.alive() else 1)" || { echo "rehearse: ft-ringplay doesn't publish: $(tail -3 "$logs/ringplay.log")" >&2; exit 1; }

step "2. Session (test script, panel --no-vr)"
tracker=existing
if ! python3 -c "import sys; from hands.rec import session; sys.exit(0 if session.tracker_running() else 1)"; then
  "$hands" --ring "$ring" --no-gestures --status 0 >"$logs/tracker.log" 2>&1 &
  pids+=($!)
  tracker=started
fi
echo "  tracker: $tracker"
"$panel" --no-vr >"$logs/panel.log" 2>&1 &
pids+=($!)
python3 - "$here" "$base" "$prompt_s" "$work/script.json" <<'EOF'
import json, os, re, sys, uuid
here, base, secs, out = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
consent = re.search(r"^Version:\s*(\S+)", open(os.path.join(here, "CONSENT.md")).read(), re.M).group(1)
json.dump({"schema": 1, "contributor": str(uuid.uuid4()),
           "consent": {"version": consent, "accepted": "2026-10-02T00:00:00+00:00", "adult": True},
           "optional": {"handedness": "", "notes": ""}}, open(os.path.join(base, "profile.json"), "w"))
json.dump({"version": 1, "intro_s": 1, "between_s": 1,
           "welcome": {"title": "Rehearsal", "seconds": 1, "text": "A rehearsal of the hand recorder."},
           "done": {"title": "Done", "seconds": 1, "text": "Rehearsal done."},
           "stopped": {"title": "Stopped", "seconds": 1, "text": "Rehearsal stopped."},
           "sections": [
               {"id": "hand-size", "title": "Hand size", "intro": "Rehearsal: hands.",
                "prompts": [{"text": "Both hands flat, palms toward you.", "seconds": secs, "hands": "both",
                             "pose": "flat", "distance": "mid"}]},
               {"id": "pose-sweeps", "title": "Hand poses", "kind": "sweep", "intro": "", "cue_s": secs / 2,
                "groups": [["open", "fist"]], "sweeps": [{"hands": "both", "group": "next", "text": "Keep moving."}]},
               {"id": "no-hands", "title": "No hands", "intro": "",
                "prompts": [{"text": "Hands out of view.", "seconds": secs, "hands": "none"}]}]},
          open(out, "w"))
EOF
for _ in $(seq 50); do
  python3 -c "import sys; from hands.rec import session
sys.exit(0 if session.Panel().cmd('ping', reply=True) else 1)" 2>/dev/null && break
  sleep 0.2
done
run python3 -m hands.rec.session --ring "$ring" --no-start --base "$base" --script "$work/script.json" --lighting room \
  --next-after 0.3 \
  </dev/null > >(indent)
sid=$(ls "$base/sessions" | tail -1)
[ -n "$sid" ] || { echo "rehearse: the session left nothing" >&2; exit 1; }
python3 -m hands.rec.takes --base "$base" takes "$sid" | sed 's/^/  /'
echo "  panel pictures: $(grep -c '^--- picture' "$logs/panel.log" || true)"

step "3. Export"
run python3 -m hands.rec.takes --base "$base" export "$sid" >"$logs/export.log"
tr '\r' '\n' <"$logs/export.log" | tail -1 | indent
export_dir=$base/exports/$sid

step "4. Validate"
valid=1
run python3 -m hands.rec.validate "$export_dir" --json >"$work/validate.json" || valid=0
python3 -c "import json, sys; v = json.load(open(sys.argv[1]))
for k in ('errors', 'warnings'):
    for line in v[k]: print('  %s: %s' % (k[:-1], line))
print('  OK' if v['ok'] else '  %d errors' % len(v['errors']))" "$work/validate.json"
[ "$valid" = 1 ] || { echo "rehearse: the export didn't validate" >&2; exit 1; }

if [ -n "$repo" ]; then
  step "5. Upload to $repo (for real)"
  run env FT_HANDREC_DATASET="$repo" python3 -m hands.rec.hub --base "$base" upload "$sid" >"$logs/hub.log"
  indent <"$logs/hub.log"
else
  step "5. Upload (dry run: --repo ID uploads for real)"
  run python3 -m hands.rec.hub --base "$base" upload "$sid" --dry-run >"$logs/hub.log"
  indent <"$logs/hub.log"
fi

step "Summary"
python3 - "$work/validate.json" "$logs/hub.log" "$tracker" "$(( $(date +%s) - t0 ))" <<'EOF'
import json, sys
v = json.load(open(sys.argv[1]))
s = v["summary"]
hub = open(sys.argv[2]).read().strip().splitlines()
print(f"  session {s['session']}: {s['takes']} takes, {s['sets']} sets, {s['minutes']} min recorded")
print(f"  export: {v['bytes'] / 1e6:.1f} MB, {len(v['errors'])} errors, {len(v['warnings'])} warnings")
for w in v["warnings"]:
    print(f"    warning: {w}")
print(f"  hub: {hub[-1] if hub else '-'}")
print(f"  tracker: {sys.argv[3]}; took {sys.argv[4]} s")
EOF
