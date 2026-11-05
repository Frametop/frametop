#!/usr/bin/env bash
# Build hand tracking in the dev container on the Frame, into hands/build/: ft-camd and ft-hands,
# and with --tools also ft-handreplay and ft-ringplay. The first build fetches ncnn and builds
# it (a few minutes); NCNN=DIR, an ncnn install already on the Frame, skips that. It also fetches
# the fine-tuned hand models (hands/models.sh) into hands/models/student, which ft-hands prefers.
# A rebuilt ft-camd has lost its capabilities: hands/run.sh install sets them again.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
targets=all
[ "${1:-}" = --tools ] && targets="all tools"
"$root/scripts/sync.sh" >/dev/null
"$root/hands/models.sh" fetch
exec "$root/scripts/frame.sh" -C hands "make -s ${NCNN:+NCNN=$NCNN} $targets && echo built \$(ls build/ft-* | tr '\n' ' ')"
