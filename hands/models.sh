#!/usr/bin/env bash
# Fetch the fine-tuned hand models into hands/models/student/ on the Frame (in the image build,
# here: pack/Containerfile runs hands/build.sh, which runs this). hands/models.pin names them: a
# fixed commit of the Hugging Face repo below, and every file's sha256. ft-hands uses them when
# they're there (HANDS_MODELS overrides; hands/models/ncnn, the stock MediaPipe models, otherwise).
#   palm.ncnn.*, hand.ncnn.*   MediaPipe's palm detector and hand landmark model, fine-tuned on the
#                              Frametop hand dataset (DeeJanuz/frametop-hands)
#   LICENSE, README.md         their licence (CC BY-NC 4.0, not Frametop's MIT) and model card
# Usage: hands/models.sh [fetch]   fetch what's missing or doesn't match models.pin (the default)
#        hands/models.sh check     check every file against the pin it was fetched by (exit 1 if not)
#        hands/models.sh update    fetch the models the install's branch pins on GitHub now (main for
#                                  a stable release, experimental for an experimental one or a
#                                  clone on that branch), if they changed. Into a folder of its own,
#                                  swapped in once every file matches. Offline or nothing new: no-op.
#        hands/models.sh auto      update, as frametop-hands.service runs it when hand tracking
#                                  starts: not with HANDS_MODELS_UPDATE=0 in ~/.config/frametop.conf,
#                                  and at most every 15 minutes.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
case ${1:-fetch} in fetch | check | update | auto) ;; *) sed -n '9,18p' "$0" >&2; exit 2 ;; esac

on_frame_script "${1:-fetch}" "$FRAME_REPO" <<'SCRIPT'
set -euo pipefail
mode=$1 tree=$2
models=$tree/hands/models dir=$tree/hands/models/student
hf=DeeJanuz/frametop-hands-models gh=Frametop/frametop
names=" NOTICE hand.ncnn.bin hand.ncnn.param palm.ncnn.bin palm.ncnn.param LICENSE README.md "

# read_pin FILE: check every line of a pin, then set rev, model and files ("name sha256" each).
# Anything unexpected fails it: a pin from GitHub only names files this list allows.
read_pin() {
  local key a b n format=
  rev= model= files=()
  while read -r key a b; do
    case $key in '' | '#'*) continue ;; esac
    case $key in
      format) [ "$a" = 1 ] && [ -z "$b" ] || return 1; format=$a ;;
      rev) [[ $a =~ ^[0-9a-f]{40}$ ]] && [ -z "$b" ] || return 1; rev=$a ;;
      model) [[ $a =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$ ]] && [ -z "$b" ] || return 1; model=$a ;;
      file) [[ $names == *" $a "* && $b =~ ^[0-9a-f]{64}$ ]] || return 1; files+=("$a $b") ;;
      *) return 1 ;;
    esac
  done <"$1"
  [ -n "$format" ] && [ -n "$rev" ] && [ -n "$model" ] || return 1
  for n in hand.ncnn.bin hand.ncnn.param palm.ncnn.bin palm.ncnn.param; do
    [[ " ${files[*]} " == *" $n "* ]] || return 1
  done
}

matches() { [ -f "$1" ] && [ "$(sha256sum <"$1" | cut -d' ' -f1)" = "$2" ]; }

# fetch_into DIR: fetch the pin's files that are missing there or don't match.
fetch_into() {
  local d=$1 entry name sum src
  mkdir -p "$d"
  for entry in "${files[@]}"; do
    read -r name sum <<<"$entry"
    matches "$d/$name" "$sum" && continue
    src=https://huggingface.co/$hf/resolve/$rev/$model/$name
    case $name in LICENSE | README.md) src=https://huggingface.co/$hf/resolve/$rev/$name ;; esac
    echo "fetching hand model $name ($model)"
    if ! curl -fsSL --retry 3 --connect-timeout 10 --max-filesize 100000000 -o "$d/$name.part" "$src"; then
      rm -f "$d/$name.part"; return 1
    fi
    if ! matches "$d/$name.part" "$sum"; then
      echo "hand models: $name's sha256 doesn't match the pin; not kept" >&2
      rm -f "$d/$name.part"; return 1
    fi
    mv "$d/$name.part" "$d/$name"
  done
}

stamp() { echo "$hf@${rev::7} $model" >"$1/VERSION"; }

case $mode in
  fetch)
    read_pin "$tree/hands/models.pin" || { echo "hand models: hands/models.pin isn't a valid pin" >&2; exit 1; }
    fetch_into "$dir"
    cp "$tree/hands/models.pin" "$dir/PIN"; stamp "$dir"
    echo "hand models: $model in $dir"
    ;;
  check)
    # Against the pin they were fetched by: an update may have replaced the release's own.
    pin=$dir/PIN; [ -f "$pin" ] || pin=$tree/hands/models.pin
    [ -d "$dir" ] || { echo "hand models: $dir missing" >&2; exit 1; }
    read_pin "$pin" || { echo "hand models: $pin isn't a valid pin" >&2; exit 1; }
    bad=0
    for entry in "${files[@]}"; do
      read -r name sum <<<"$entry"
      matches "$dir/$name" "$sum" || { echo "hand models: $name missing or changed" >&2; bad=1; }
    done
    [ "$bad" = 0 ] || exit 1
    echo "hand models: $model in $dir, all files match"
    ;;
  update | auto)
    if [ "$mode" = auto ]; then
      conf=$(sed -n 's/#.*//; s/^[[:space:]]*HANDS_MODELS_UPDATE[[:space:]]*=[[:space:]]*//p' ~/.config/frametop.conf 2>/dev/null | tail -1)
      conf=${FT_HANDS_MODELS_UPDATE:-$conf}
      [ "$(echo $conf | tr -d "\"'")" = 0 ] && { echo "hand models: not updating (HANDS_MODELS_UPDATE=0)"; exit 0; }
      [ -n "$(find "$models/.student-checked" -mmin -15 2>/dev/null)" ] && exit 0
    fi
    if [ -f "$tree/.frametop-release" ]; then
      case $(sed -n 's/^CHANNEL=//p' "$tree/.frametop-release" | tail -1) in
        experimental) branch=experimental ;; *) branch=main ;;
      esac
    else
      branch=$(git -C "$tree" symbolic-ref --short -q HEAD 2>/dev/null || true)
    fi
    case $branch in main | experimental) ;; *) echo "hand models: no updates for branch '$branch'"; exit 0 ;; esac
    mkdir -p "$models"
    exec 9>"$models/.student-lock"
    flock -n 9 || { echo "hand models: another update is running"; exit 0; }
    touch "$models/.student-checked"
    tmp=$(mktemp); trap 'rm -f "$tmp"' EXIT
    code=$(curl -sS --connect-timeout 4 --max-time 15 --max-filesize 16384 -o "$tmp" -w '%{http_code}' \
      "https://raw.githubusercontent.com/$gh/$branch/hands/models.pin") || code=
    case $code in
      200) ;;
      '') echo "hand models: couldn't reach GitHub; keeping these"; exit 0 ;;
      *) echo "hand models: $branch pins none ($code); keeping these"; exit 0 ;;
    esac
    cmp -s "$tmp" "$dir/PIN" 2>/dev/null && { echo "hand models: up to date ($(cat "$dir/VERSION" 2>/dev/null))"; exit 0; }
    read_pin "$tmp" || { echo "hand models: $branch's pin isn't valid (or needs a newer Frametop); keeping these" >&2; exit 1; }
    # Into .student-new, seeded with the files we have (a stopped update keeps what it fetched),
    # then swapped with student in one rename: ft-hands never sees half a set.
    new=$models/.student-new
    mkdir -p "$new"
    [ -d "$dir" ] && cp -a --update=none "$dir/." "$new/"
    rm -f "$new"/*.part
    fetch_into "$new" || { echo "hand models: update failed; keeping these" >&2; exit 1; }
    cp "$tmp" "$new/PIN"; stamp "$new"
    if [ -d "$dir" ]; then mv -T --exchange "$new" "$dir"; rm -rf "$new"; else mv -T "$new" "$dir"; fi
    echo "hand models: updated to $model from $branch's pin"
    ;;
esac
SCRIPT
