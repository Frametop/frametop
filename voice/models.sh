#!/usr/bin/env bash
# Fetch Frametop Voice's models (about 340 MB) into ~/.local/share/frametop/voice/models on the
# Frame, each from a fixed URL (a Hugging Face commit or a sherpa-onnx release asset) and checked
# against its sha256. They stay there across releases and reinstalls; uninstall.sh removes them.
#   ggml-small.en-q5_1.bin   whisper small.en (whisper.cpp's), wake word commands      MIT
#   sherpa-onnx-moonshine-base-en-quantized-2026-02-27/   Moonshine base, live dictation  MIT
#   sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01/ the wake word spotter      Apache-2.0
#   silero_vad.onnx          Silero VAD, where speech starts and stops                  MIT
# And whisper models for other languages, fetched only when chosen on the Voice page (or here):
#   small           ggml-small-q5_1.bin, 190 MB, multilingual                            MIT
#   large-v3-turbo  ggml-large-v3-turbo-q5_0.bin, 574 MB, multilingual                   MIT
# Usage: voice/models.sh [fetch]      fetch what's missing or doesn't match (the default)
#        voice/models.sh check        check every file's sha256 (exit 1 if any is missing or wrong),
#                                     the extra models too where they're there
#        voice/models.sh model NAME   fetch one of the extra whisper models (voice_settings.WHISPER_MODELS)
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"

on_frame_script "${1:-fetch}" "${2:-}" <<'SCRIPT'
set -euo pipefail
mode=$1
want=$2
dir=$HOME/.local/share/frametop/voice/models
hf=https://huggingface.co
gh=https://github.com/k2-fsa/sherpa-onnx/releases/download
moon=sherpa-onnx-moonshine-base-en-quantized-2026-02-27
kws=sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01
# file, sha256, URL (an archive's files: its own entry, from "tar:" plus the archive's name)
files=(
  "ggml-small.en-q5_1.bin bfdff4894dcb76bbf647d56263ea2a96645423f1669176f4844a1bf8e478ad30 $hf/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/ggml-small.en-q5_1.bin"
  "$moon/encoder_model.ort 7c66495948d0d08ec1af454cd4b5514862ae6511e94712a60e6d83eaec8dc8cf $hf/csukuangfj2/$moon/resolve/8f4d6c58c03d40bcea40043bb7120a878f2bbef6/encoder_model.ort"
  "$moon/decoder_model_merged.ort d9d7b333af34bc552580576ddcf248a1c6c839e0d3b43b09afb9376ed009899d $hf/csukuangfj2/$moon/resolve/8f4d6c58c03d40bcea40043bb7120a878f2bbef6/decoder_model_merged.ort"
  "$moon/tokens.txt 2870d843e14c1e187bf1913a521562a63b53933814bd7f2145120468f494a049 $hf/csukuangfj2/$moon/resolve/8f4d6c58c03d40bcea40043bb7120a878f2bbef6/tokens.txt"
  "silero_vad.onnx 9e2449e1087496d8d4caba907f23e0bd3f78d91fa552479bb9c23ac09cbb1fd6 $gh/asr-models/silero_vad.onnx"
  "$kws.tar.bz2 f170013b4716e41b62b9bfd809687c207cef798ef9bc6534d524e17af9b6561a $gh/kws-models/$kws.tar.bz2"
)
# The extra whisper models: name, file, sha256, URL (the same whisper.cpp commit).
wcpp=$hf/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1
extra=(
  "small ggml-small-q5_1.bin ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb $wcpp/ggml-small-q5_1.bin"
  "large-v3-turbo ggml-large-v3-turbo-q5_0.bin 394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2 $wcpp/ggml-large-v3-turbo-q5_0.bin"
)
case $mode in
  fetch|check) ;;
  model)
    files=()
    for entry in "${extra[@]}"; do
      read -r name file sum url <<<"$entry"
      [ "$name" = "$want" ] && files=("$file $sum $url")
    done
    [ ${#files[@]} = 1 ] || { echo "models.sh: no whisper model \"$want\" (small, large-v3-turbo)" >&2; exit 2; }
    mode=fetch
    ;;
  *) echo "usage: voice/models.sh [fetch|check|model NAME]" >&2; exit 2 ;;
esac
if [ "$1" = check ]; then
  for entry in "${extra[@]}"; do
    read -r name file sum url <<<"$entry"
    [ -f "$dir/$file" ] && files+=("$file $sum $url")
  done
fi
# What ft-voiced uses from the wake word archive (int8, the tokens and the BPE model).
kws_files="tokens.txt bpe.model encoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx decoder-epoch-12-avg-2-chunk-16-left-64.int8.onnx joiner-epoch-12-avg-2-chunk-16-left-64.int8.onnx"
mkdir -p "$dir/$moon" "$dir/$kws"
cd "$dir"
bad=0
for entry in "${files[@]}"; do
  read -r name sum url <<<"$entry"
  if [ "$name" = "$kws.tar.bz2" ]; then
    # Kept as its files (a stamp holds the archive's sha256), not the archive.
    if [ "$(cat "$kws/.archive" 2>/dev/null)" = "$sum" ] && (cd "$kws" && ls $kws_files >/dev/null 2>&1); then
      continue
    fi
  elif [ -f "$name" ]; then
    # A file that's there is checked once, then trusted by its stamp, unless check asks again.
    if [ "$mode" = fetch ] && [ "$(cat "$name.sha256" 2>/dev/null)" = "$sum" ]; then continue; fi
    if echo "$sum  $name" | sha256sum -c --quiet --status 2>/dev/null; then
      echo "$sum" > "$name.sha256"
      continue
    fi
    [ "$mode" = check ] && { echo "$name: sha256 doesn't match" >&2; bad=1; continue; }
  fi
  if [ "$mode" = check ]; then echo "$name: missing" >&2; bad=1; continue; fi
  echo "fetching $name"
  curl -fL --retry 3 --progress-bar -o "$name.part" "$url"
  if ! echo "$sum  $name.part" | sha256sum -c --quiet --status; then
    echo "$name: the download's sha256 doesn't match; not kept" >&2
    rm -f "$name.part"
    exit 1
  fi
  if [ "$name" = "$kws.tar.bz2" ]; then
    tar -xjf "$name.part" --strip-components=1 -C "$kws" $(for f in $kws_files; do echo "$kws/$f"; done)
    echo "$sum" > "$kws/.archive"
    rm -f "$name.part"
  else
    mv "$name.part" "$name"
    echo "$sum" > "$name.sha256"
  fi
done
[ "$bad" = 0 ] || exit 1
echo "models: $(du -sh "$dir" | cut -f1) in $dir"
SCRIPT
