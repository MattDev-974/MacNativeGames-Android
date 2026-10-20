#!/usr/bin/env bash
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CROSS=${CROSS:-"$HERE/../../../tools/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc"}
QEMU=${QEMU:-"$HERE/../../../tools/qemu-aarch64-static"}
GEN="$HERE/gen"
mkdir -p "$GEN"
"$CROSS" -static -O1 -g -I"$HERE/../../app/cpp" \
    "$HERE/../../app/cpp/mng_image.c" "$HERE/image_test.c" -o "$GEN/image-test"
"$QEMU" "$GEN/image-test"
