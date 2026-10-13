#!/bin/bash
# Replays recordings through ft-handreplay with a timeline (trace events + oracle) for
# tools/gap_report.py. Runs several at once; meant for the 7i through frame-job:
#   frame-job -- tools/replay_gaps.sh OUT MODELS "EXTRA ARGS" REC[:FROM:TO]...
# OUT gets REC.txt (the summary) and REC.tl (the timeline) per recording.
set -uo pipefail
out=$1 models=$2 extra=$3
shift 3
here=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$out"
jobs=${JOBS:-4}
for spec in "$@"; do
  IFS=: read -r rec from to <<<"$spec"
  name=$(basename "$rec")
  range=()
  [ -n "${from:-}" ] && range=(--from "$from" --to "$to") && name=$name-$from-$to
  # a recorder session (calibration.json + device.json beside sets.bin or two levels up): replay
  # with that headset's calibration
  devroot=()
  for c in "$rec" "$rec/../.."; do
    if [ -f "$c/calibration.json" ] && [ -f "$c/device.json" ]; then
      d=$out/devroot-$(basename "$(realpath "$c")")
      mkdir -p "$d/persist"
      cp "$c/calibration.json" "$d/persist/xrservice.json"
      cp "$c/device.json" "$d/persist/device_config.json"
      devroot=(env FRAME_JOB_DEVICE_ROOT="$d")
      [ "$c" = "$rec" ] || name=$(basename "$(realpath "$c")")-$name
      break
    fi
  done
  while [ "$(jobs -rp | wc -l)" -ge "$jobs" ]; do wait -n; done
  # shellcheck disable=SC2086
  ${devroot[@]+"${devroot[@]}"} "$here/build/ft-handreplay" "$rec" --cost --threads ${THREADS:-3} --models "$models" --timeline "$out/$name.tl" \
    ${range[@]+"${range[@]}"} $extra > "$out/$name.txt" 2>&1 &
done
wait
for f in "$out"/*.txt; do echo "== $f"; grep -E "processed|hands per|gaps|within 8|re-acquire|views lost|model calls|oracle,|full search|jitter" "$f"; done
python3 "$here/tools/gap_report.py" "$out"/*.tl > "$out/gaps.txt" 2>&1 || true
