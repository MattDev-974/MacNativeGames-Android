#!/usr/bin/env bash
# Build the host test for aarch64 and run it under qemu-user.
#   TOOLS=../../tools ./run.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CROSS=${CROSS:-"$HERE/../../../tools/aarch64-linux-musl-cross/bin/aarch64-linux-musl-gcc"}
QEMU=${QEMU:-"$HERE/../../../tools/qemu-aarch64-static"}
GEN="$HERE/gen"

mkdir -p "$GEN"
cp "$HERE/../../app/cpp/nvn.c" "$HERE/../../app/cpp/nvn.h" "$HERE/../../app/cpp/gate.S" "$GEN/"
cp "$HERE/vk.h" "$HERE/stub_check.h" "$GEN/"

"$CROSS" -static -O1 -g \
    -I"$HERE/portable" -I"$GEN" \
    "$GEN/nvn.c" "$GEN/gate.S" "$HERE/stub_vk.c" "$HERE/asm_test.S" "$HERE/main.c" \
    -o "$GEN/host-test" -lpthread

"$QEMU" "$GEN/host-test"
