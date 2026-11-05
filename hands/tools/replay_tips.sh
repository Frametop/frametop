#!/bin/bash
# Replays recordings through ft-handreplay with --tips (each hand's thumb, index and middle tips and the
# pinch detector's measures per processed set, plus its pinch events), for frame-hands'
# train/taps/tapeval.py. Runs several at once; meant for a compute host through frame-job:
#   frame-job -- tools/replay_tips.sh OUT NAME=MODELS "EXTRA ARGS" REC[:FROM:TO]...
# OUT gets NAME/<rec>.tips and NAME/<rec>.txt (the summary) per recording. A recorder take
# (takes/NN-name, with calibration.json and device.json two levels up) replays with that headset's
# calibration and its side cameras as stored (--sides 0: exports are named right).
set -uo pipefail
out=$1 nm=$2 extra=$3
shift 3
name=${nm%%=*} models=${nm#*=}
here=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$out/$name"
jobs=${JOBS:-4}
for spec in "$@"; do
  IFS=: read -r rec from to <<<"$spec"
  base=$(basename "$rec")
  range=()
  [ -n "${from:-}" ] && range=(--from "$from" --to "$to") && base=$base-$from-$to
  devroot=() sides=()
  if [ -f "$rec/../../calibration.json" ] && [ -f "$rec/../../device.json" ]; then
    s=$(realpath "$rec/../..")
    d=$out/devroot-$(basename "$(dirname "$s")" | cut -c1-8)-$(basename "$s")
    mkdir -p "$d/persist"
    cp "$s/calibration.json" "$d/persist/xrservice.json"
    cp "$s/device.json" "$d/persist/device_config.json"
    devroot=(env FRAME_JOB_DEVICE_ROOT="$d")
    sides=(--sides 0)
    base=$(basename "$(dirname "$s")" | cut -c1-8)-$base
  fi
  while [ "$(jobs -rp | wc -l)" -ge "$jobs" ]; do wait -n; done
  # shellcheck disable=SC2086
  ${devroot[@]+"${devroot[@]}"} "$here/build/ft-handreplay" "$rec" --cost --threads ${THREADS:-3} --models "$models" \
    --tips "$out/$name/$base.tips" ${sides[@]+"${sides[@]}"} ${range[@]+"${range[@]}"} $extra > "$out/$name/$base.txt" 2>&1 &
done
wait
for f in "$out/$name"/*.txt; do echo "== $f"; grep -E "processed|pinches" "$f"; done
