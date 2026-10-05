#!/bin/bash
set -e
export PATH="$HOME/.local/workcard-toolchain/gcc-20250305-arm-v01c02-linux-musleabi/arm-v01c02-linux-musleabi-gcc/bin:$PATH"
SDK_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SDK_DIR/.." && pwd)
TMP_BUILD="$ROOT_DIR/tmp/location_cli_demo_build"
OUT_DIR="$SDK_DIR/build/location_cli_demo"
rm -rf "$TMP_BUILD"
cmake -S "$SDK_DIR/examples" -B "$TMP_BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$SDK_DIR/cmake/arm-linux-musleabi-toolchain.cmake"
cmake --build "$TMP_BUILD" --target location_cli_demo -j2
mkdir -p "$OUT_DIR"
cp -f "$TMP_BUILD/location_cli_demo" "$OUT_DIR/location_cli_demo"
chmod +x "$OUT_DIR/location_cli_demo"
echo "Built $OUT_DIR/location_cli_demo"
