#!/usr/bin/env bash
# Fetch the pinned, fine-tuned ncnn models. Stock models are bundled in models/ncnn.
set -euo pipefail
root=$(cd "$(dirname "$0")" && pwd)
mode=${1:-fetch}
[[ $mode == fetch || $mode == check ]] || { echo 'usage: models.sh fetch|check' >&2; exit 2; }
rev= model=
declare -A sums=()
while read -r key name sum; do
  case $key in
    ''|'#'*) ;;
    format) [[ $name == 1 && -z ${sum:-} ]] || exit 1 ;;
    rev) [[ $name =~ ^[0-9a-f]{40}$ ]] || exit 1; rev=$name ;;
    model) [[ $name =~ ^[A-Za-z0-9._-]+$ ]] || exit 1; model=$name ;;
    file)
      case $name in NOTICE|LICENSE|README.md|hand.ncnn.bin|hand.ncnn.param|palm.ncnn.bin|palm.ncnn.param) ;; *) exit 1 ;; esac
      [[ $sum =~ ^[0-9a-f]{64}$ ]] || exit 1
      sums[$name]=$sum ;;
    *) exit 1 ;;
  esac
done < "$root/models.pin"
[[ -n $rev && -n $model ]] || exit 1
for name in hand.ncnn.bin hand.ncnn.param palm.ncnn.bin palm.ncnn.param; do
  [[ -n ${sums[$name]:-} ]] || exit 1
done
dir=$root/models/student
[[ $mode == check || -d $dir ]] || mkdir -p "$dir"
for name in "${!sums[@]}"; do
  target=$dir/$name
  if [[ -f $target ]] && echo "${sums[$name]}  $target" | sha256sum -c --status; then continue; fi
  [[ $mode == fetch ]] || { echo "missing or corrupt: $target" >&2; exit 1; }
  url=https://huggingface.co/DeeJanuz/frametop-hands-models/resolve/$rev/$model/$name
  [[ $name == LICENSE || $name == README.md ]] && url=https://huggingface.co/DeeJanuz/frametop-hands-models/resolve/$rev/$name
  echo "fetching $name ($model)"
  curl -fL --retry 3 --connect-timeout 10 --max-time 180 -o "$target.part" "$url"
  echo "${sums[$name]}  $target.part" | sha256sum -c --status || { rm -f "$target.part"; exit 1; }
  mv -f "$target.part" "$target"
done
echo "hand models: $model verified"
