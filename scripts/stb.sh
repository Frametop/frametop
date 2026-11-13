# Sourced by renderer/build.sh: stb_image_write.h for offline snapshots
# with, at one pinned commit, each checked by its sha256 (hashed from that commit's files).
#   stb_fetch DIR HEADER...   fetches each HEADER into DIR once; a copy that doesn't match is
#                             fetched again, and a download that doesn't match fails.
# Works on a development PC too (shasum when there's no sha256sum).
stb_commit=2c980bb59875b0d32144a71867fbdebb2f77cd20

stb_sha256() {
  case $1 in
    stb_image_write.h) echo cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05 ;;
    *) return 1 ;;
  esac
}

stb_digest() {
  local d
  if command -v sha256sum >/dev/null 2>&1; then d=$(sha256sum "$1"); else d=$(shasum -a 256 "$1"); fi
  echo "${d%% *}"
}

stb_fetch() {
  local dir=$1 name wanted
  shift
  mkdir -p "$dir"
  for name in "$@"; do
    wanted=$(stb_sha256 "$name") || { echo "stb_fetch: $name has no pinned sha256" >&2; return 1; }
    [ -f "$dir/$name" ] && [ "$(stb_digest "$dir/$name")" = "$wanted" ] && continue
    curl -fsSL "https://raw.githubusercontent.com/nothings/stb/$stb_commit/$name" -o "$dir/$name.part" || return
    if [ "$(stb_digest "$dir/$name.part")" != "$wanted" ]; then
      echo "$name from stb $stb_commit doesn't match its pinned sha256" >&2
      rm -f "$dir/$name.part"
      return 1
    fi
    mv "$dir/$name.part" "$dir/$name"
  done
}
