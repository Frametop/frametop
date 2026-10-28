#!/bin/bash
# Replays recordings through ft-handreplay as the live tracker runs, each into a publish dump
# (OUT/NAME.dump) with its head poses beside it (OUT/NAME.poses.jsonl, NAME.prompts.jsonl), for the
# offline tools that read what ft-hands would have published (pointer/tools/deskfeat, rayeval,
# screens' handinput tests). Runs several at once; meant for a compute host through frame-job:
#   frame-job -- tools/replay_dumps.sh OUT MODELS REC...
# A recording with calibration.json and device.json beside sets.bin, or two levels up (a hand-recorder
# session's take), replays with that headset's calibration, as in tools/replay_gaps.sh; a take is
# named after its session too (SESSION-TAKE).
set -uo pipefail
out=$1 models=$2
shift 2
here=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$out"
jobs=${JOBS:-4}
for rec in "$@"; do
  name=$(basename "$rec")
  devroot=()
  for c in "$rec" "$rec/../.."; do
    if [ -f "$c/calibration.json" ] && [ -f "$c/device.json" ]; then
      [ "$c" = "$rec" ] || name=$(basename "$(realpath "$c")")-$name
      d=$out/devroot-$name
      mkdir -p "$d/persist"
      cp "$c/calibration.json" "$d/persist/xrservice.json"
      cp "$c/device.json" "$d/persist/device_config.json"
      devroot=(env FRAME_JOB_DEVICE_ROOT="$d")
      break
    fi
  done
  for f in poses.jsonl prompts.jsonl; do [ -f "$rec/$f" ] && cp "$rec/$f" "$out/$name.$f"; done
  while [ "$(jobs -rp | wc -l)" -ge "$jobs" ]; do wait -n; done
  ${devroot[@]+"${devroot[@]}"} "$here/build/ft-handreplay" "$rec" --cost --threads "${THREADS:-3}" --models "$models" \
    --reacquire 1.5 --reacquire-fast --pair-sides --hold 0.6 --publish-dump "$out/$name.dump" > "$out/$name.txt" 2>&1 &
done
wait
for f in "$out"/*.txt; do echo "== $f"; grep -E "processed|hands per" "$f"; done
