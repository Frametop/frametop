#!/usr/bin/env bash
# Build and run the offline unit and loopback integration tests for stream/apple_hp.h without
# the test gate (on a Mac too); `just test-c` runs the same test (scripts/testgate/cprograms.py).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
mkdir -p "$root/build/tests"

cxx=${CXX:-c++}
cflags=(-std=c++17 -O2 -Wall -Wextra -Werror "-I$root/stream")
libs=(-lz -lpthread)

if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists openssl; then
    read -r -a ossl_cflags <<<"$(pkg-config --cflags openssl)"
    read -r -a ossl_libs <<<"$(pkg-config --libs openssl)"
    cflags+=("${ossl_cflags[@]}")
    libs+=("${ossl_libs[@]}")
elif [ -d /opt/homebrew/opt/openssl/include ]; then
    cflags+=(-I/opt/homebrew/opt/openssl/include)
    libs+=(-L/opt/homebrew/opt/openssl/lib -lcrypto)
else
    libs+=(-lcrypto)
fi

"$cxx" "${cflags[@]}" "$root/stream/test/apple-hp-test.cpp" -o "$root/build/tests/apple-hp-test" "${libs[@]}"
"$root/build/tests/apple-hp-test"
