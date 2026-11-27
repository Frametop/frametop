#!/usr/bin/env bash
# Fetch Frametop Voice's models (about 870 MB) into ~/.local/share/frametop/voice/models on the
# Frame, each from a fixed URL (a Hugging Face commit or a sherpa-onnx release asset) and checked
# against its sha256. They stay there across releases and reinstalls; uninstall.sh removes them.
#   sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8/   Parakeet TDT 0.6B v3 (NVIDIA's, sherpa-onnx's
#                            int8 export): dictation and commands, 25 languages   CC BY 4.0
#   ggml-small.en-q5_1.bin   whisper small.en (whisper.cpp's): what whisper-server loads
#                            when the model the settings choose isn't there             MIT
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
para=sherpa-onnx-nemo-parakeet-tdt-0.6b-v3-int8
pk=$hf/csukuangfj/$para/resolve/ed5d745623472f97cfc2e6c2916ee248798fc5a8
kws=sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01
# file, sha256, URL (an archive's files: its own entry, from "tar:" plus the archive's name)
files=(
  "ggml-small.en-q5_1.bin bfdff4894dcb76bbf647d56263ea2a96645423f1669176f4844a1bf8e478ad30 $hf/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/ggml-small.en-q5_1.bin"
  "$para/encoder.int8.onnx 1afbe69c1ffdabee94755f42e4daf1d93549131a3da11356bda84fcef0f2c479 $pk/encoder.int8.onnx"
  "$para/decoder.int8.onnx 0222a86147299d4dee90edf95b6d13827edb42ad8b7c9cf0b2582f030937c0b5 $pk/decoder.int8.onnx"
  "$para/joiner.int8.onnx cbd02f3696e9944970111fb439d9c3fd365a80374565f13f2b9c7007d253fc98 $pk/joiner.int8.onnx"
  "$para/tokens.txt d58544679ea4bc6ac563d1f545eb7d474bd6cfa467f0a6e2c1dc1c7d37e3c35d $pk/tokens.txt"
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
mkdir -p "$dir/$para" "$dir/$kws"
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
