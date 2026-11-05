#!/usr/bin/env bash
# Run on the development host. Nothing is installed on the Frame by this script.
set -euo pipefail
project_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$project_dir"
./scripts/build.sh frame-arm64
version=$(sed -n 's/^project(framekeyboard VERSION \([^ ]*\).*/\1/p' CMakeLists.txt)
package_dir="$project_dir/out/framekeyboard-$version-aarch64"
# A rebuild must not retain resources removed from the install rules.
rm -rf -- "$package_dir"
mkdir -p "$package_dir"
cmake --install build/frame-arm64 --prefix "$package_dir" --strip
python3 tools/check_release_binary.py "$package_dir/bin/framekeyboard" \
    "$project_dir" "$FRAMEKEYBOARD_SYSROOT"
# The two adapters are release assets even though their system dependencies
# are optional at runtime. Check stripping and path hygiene for them too.
for engine in pinyin hangul; do
    python3 tools/check_release_binary.py "$package_dir/lib/framekeyboard/framekeyboard-$engine.so" \
        "$project_dir" "$FRAMEKEYBOARD_SYSROOT"
done
if readelf -d "$package_dir/bin/framekeyboard" | grep -E 'NEEDED.*(libpyzy|libhangul)'; then
    echo 'Optional language libraries must not be startup dependencies.' >&2
    exit 1
fi
if readelf -d "$package_dir/bin/framekeyboard" | grep -E 'NEEDED.*libei\.so'; then
    echo 'The release must use its pinned libei client, not the system library.' >&2
    exit 1
fi
cp scripts/install-local.sh "$package_dir/install.sh"
tar -C out -czf "out/framekeyboard-$version-aarch64.tar.gz" "framekeyboard-$version-aarch64"
(cd out && sha256sum "framekeyboard-$version-aarch64.tar.gz" > SHA256SUMS)
cat out/SHA256SUMS
