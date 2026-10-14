#!/usr/bin/env bash
# Fetch the fine-tuned hand models into hands/models/student/ on the Frame (in the image build,
# here: pack/Containerfile runs hands/build.sh, which runs this). They come from a fixed commit of
# the Hugging Face repo below and are checked against their sha256. ft-hands uses them when they're
# there (HANDS_MODELS overrides; hands/models/ncnn, the stock MediaPipe models, otherwise).
#   palm.ncnn.*, hand.ncnn.*   MediaPipe's palm detector and hand landmark model, fine-tuned on the
#                              Frametop hand dataset (DeeJanuz/frametop-hands)
#   LICENSE, README.md         their licence (CC BY-NC 4.0, not Frametop's MIT) and model card
# Usage: hands/models.sh [fetch]   fetch what's missing or doesn't match (the default)
#        hands/models.sh check     check every file's sha256 (exit 1 if any is missing or wrong)
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"

on_frame_script "${1:-fetch}" "$FRAME_REPO/hands/models/student" <<'SCRIPT'
set -euo pipefail
mode=$1 dir=$2
repo=DeeJanuz/frametop-hands-models
rev=41afd78fbd4527456fc4cbd840ffc9f39fdc939a
model=v13b-p1
url=https://huggingface.co/$repo/resolve/$rev
# file, sha256 (the repo's $model/ folder, but LICENSE and README.md at its top)
files=(
"NOTICE 0e6b4fa8ea37665cb699abc36fcf1ee0ffaa63a78dfee788be6613dea6607f84"
"hand.ncnn.bin d6a6f00d845fb9ee490276d84b1e978da884361c372614546ed2b1b4d2f5c449"
"hand.ncnn.param 54e08f284ff6f121ff00f4a9c65afc38c0dbb22a32f4af44fc4e0fde6c4a1bbd"
"palm.ncnn.bin 9865f5fa3c6bc885cb0b50f201c4ac1bd83b2c1ad64af4e122efa3fc56d61909"
"palm.ncnn.param c61df2dde5b221a55c35c9693e530845a95ce2732949a9b3d3c583e3b2bae92e"
"LICENSE 41003d4a74749c0220e33dd415042164b5a1093ed401f36277234f772d22d3d0"
"README.md 29c6c5460e47412a9a37b22d6a7c8a9ed266a872bbcce93d3867f9e56cfe0ca1"
)
[ "$mode" = check ] && [ ! -d "$dir" ] && { echo "hand models: $dir missing" >&2; exit 1; }
mkdir -p "$dir"
cd "$dir"
bad=0
for entry in "${files[@]}"; do
  read -r name sum <<<"$entry"
  if [ -f "$name" ] && echo "$sum  $name" | sha256sum -c --quiet --status 2>/dev/null; then continue; fi
  if [ "$mode" = check ]; then echo "hands models: $name missing or changed" >&2; bad=1; continue; fi
  src=$url/$model/$name
  case $name in LICENSE | README.md) src=$url/$name ;; esac
  echo "fetching hand model $name"
  curl -fsSL --retry 3 -o "$name.part" "$src"
  if ! echo "$sum  $name.part" | sha256sum -c --quiet --status; then
    echo "hands models: $name: the download's sha256 doesn't match; not kept" >&2
    rm -f "$name.part"
    exit 1
  fi
  mv "$name.part" "$name"
done
[ "$bad" = 0 ] || exit 1
[ "$mode" = check ] && { echo "hand models: $model in $dir, all files match"; exit 0; }
echo "$repo@${rev::7} $model" > VERSION
echo "hand models: $model in $dir"
SCRIPT
