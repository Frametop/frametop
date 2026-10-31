#!/usr/bin/env bash
# Build Frametop's download: framedrop/build/Frametop.zip, a "Frametop" folder that FrameDrop
# installs from a PC, or that you unpack on the headset and run (frametop-install.sh). Also
# framedrop/build/frametop.framedrop.json, the manifest an "Install with FrameDrop" button
# points at, and SHA256SUMS. Same files in, same zip out.
#
# Usage: framedrop/build.sh --image REF --version V [--commit SHA] [--channel C] [--allow-changes]
#                           [ZIP_URL]
#        framedrop/build.sh [ZIP_URL]
#   --image REF   a release: the image REF (built from this checkout, pack/Containerfile) goes
#                 in the zip as frametop-image.tar with frametop-release.json, and the
#                 installer installs it, built (pack/install-release.sh). About 1.1 GB.
#   --version V   the release's version (0.3.0, or 0.3.0-exp.1 for an experimental one)
#   --commit SHA  the commit it was built from (default: this checkout's HEAD)
#   --channel C   stable or experimental (default: experimental if V has a "-")
#   --allow-changes  pack an image that wasn't built from that commit alone: ft dev build's
#                 labels say it had other changes in, or another commit, or it has none. Its
#                 frametop-release.json then names a commit it wasn't built from, and
#                 fast-install diffs from that commit. For local tests only.
#   Without --image, a few KB: the installer clones Frametop from GitHub (get.sh), for
#   testing the installer itself.
#   ZIP_URL  where the zip will be downloaded from (default: the release's asset, or for
#            a test zip, the framedrop-installer release)
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
image= version= commit= channel= allow_changes=0
while [ $# -gt 0 ]; do
  case $1 in
    --image) image=${2:?--image needs an image}; shift ;;
    --version) version=${2:?--version needs a version}; shift ;;
    --commit) commit=${2:?--commit needs a commit}; shift ;;
    --channel) channel=${2:?--channel needs stable or experimental}; shift ;;
    --allow-changes) allow_changes=1 ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) break ;;
  esac
  shift
done
out=$here/build
rm -rf "$out"
mkdir -p "$out"

files=("$here/installer/frametop-install.sh" "$here/installer/progress.py" "$here/installer/askpass")
if [ -n "$image" ]; then
  [ -n "$version" ] || { echo "--image needs --version" >&2; exit 2; }
  commit=${commit:-$(git -C "$repo" rev-parse HEAD)}
  # ft dev build labels the image with the commit it was built from and whether the tree had
  # changes in (DEE-288). A release names one commit, and fast-install trusts it.
  built=$(podman image inspect -f '{{index .Labels "org.frametop.commit"}} {{index .Labels "org.frametop.tree"}}' \
            "$image" 2>/dev/null) || built=
  read -r built_commit built_tree _ <<<"$built" || true
  if [ "${built_tree:-}" != clean ] || [[ ${built_commit:-} != "$commit"* ]] || [ ${#commit} -lt 7 ]; then
    why="built from ${built_commit:-an unknown commit} with uncommitted or untracked changes in"
    [ "${built_tree:-}" = clean ] && why="built from ${built_commit:-an unknown commit}, not $commit"
    case ${built_commit:-} in ''|'<no'*|unknown) why="not labelled by ft dev build (an older build, or another tool)" ;; esac
    if [ "$allow_changes" = 0 ]; then
      echo "$image was $why. Commit or stash the changes (untracked files too) and run ft dev build" >&2
      echo "again, or add --allow-changes for a local test release." >&2
      exit 1
    fi
    echo "warning: $image was $why; packing it anyway (--allow-changes)" >&2
  fi
  url=${1:-https://github.com/Frametop/frametop/releases/download/v$version/Frametop.zip}
  echo "saving $image"
  podman save -q --format oci-archive -o "$out/frametop-image.tar" "$image"
  python3 "$repo/pack/release-info.py" --image-file "$out/frametop-image.tar" --version "$version" \
    --commit "$commit" ${channel:+--channel "$channel"} >"$out/frametop-release.json"
  files+=("$repo/pack/install-release.sh" "$out/frametop-release.json" "$out/frametop-image.tar")
else
  url=${1:-https://github.com/Frametop/frametop/releases/download/framedrop-installer/Frametop.zip}
  files+=("$repo/get.sh")
fi

python3 - "$out/Frametop.zip" "${files[@]}" <<'EOF'
import os, shutil, sys, zipfile
dest, *files = sys.argv[1:]
with zipfile.ZipFile(dest, "w", allowZip64=True) as z:
    for path in files:
        name = os.path.basename(path)
        info = zipfile.ZipInfo(f"Frametop/{name}", date_time=(2026, 1, 1, 0, 0, 0))
        info.create_system = 3  # unix, so the permissions below count
        info.external_attr = (0o100755 if os.access(path, os.X_OK) else 0o100644) << 16
        # The image's layers are compressed already.
        info.compress_type = zipfile.ZIP_STORED if name.endswith(".tar") else zipfile.ZIP_DEFLATED
        info.file_size = os.path.getsize(path)
        with open(path, "rb") as src, z.open(info, "w", force_zip64=True) as dst:
            shutil.copyfileobj(src, dst, 1 << 20)
EOF
rm -f "$out/frametop-image.tar"

sha=$(sha256sum "$out/Frametop.zip" | cut -d' ' -f1)
python3 - "$url" "$sha" >"$out/frametop.framedrop.json" <<'EOF'
import json, sys
url, sha = sys.argv[1:]
print(json.dumps({
    "schema": "framedrop.install/v1",
    "name": "Frametop",
    "files": [{"url": url, "sha256": sha}],
}, indent=2))
EOF
(cd "$out" && sha256sum Frametop.zip frametop.framedrop.json ${image:+frametop-release.json} >SHA256SUMS)
echo "built $out/Frametop.zip ($(du -h "$out/Frametop.zip" | cut -f1), sha256 $sha)"
echo "      $out/frametop.framedrop.json, $out/SHA256SUMS"
