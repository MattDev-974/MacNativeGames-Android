#!/usr/bin/env bash
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CROSS=${CROSS:-"$HERE/../../../tools/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc"}
QEMU=${QEMU:-"$HERE/../../../tools/qemu-aarch64-static"}
TREE=${1:-"$HERE/../../../TOTK_android/totk"}
GEN="$HERE/gen"
mkdir -p "$GEN"
"$CROSS" -static -O0 -w -I"$HERE/../../app/cpp" -I"$TREE/build" \
    "$HERE/../../app/cpp/mng_image.c" "$HERE/../../app/cpp/mng_loader.c" "$HERE/../../app/cpp/mng_gate.c" \
    "$HERE/loader_real_test.c" -o "$GEN/loader-real"
"$QEMU" "$GEN/loader-real" "$TREE/rewritten/v0"
