#!/bin/bash
set -e
export PATH="$HOME/.local/workcard-toolchain/gcc-20250305-arm-v01c02-linux-musleabi/arm-v01c02-linux-musleabi-gcc/bin:$PATH"
SDK_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SDK_DIR/.." && pwd)
TMP_BUILD="$ROOT_DIR/tmp/imu_web_demo_build"
OUT_DIR="$SDK_DIR/build/imu_web_demo"
rm -rf "$TMP_BUILD"
cmake -S "$SDK_DIR/examples/imu_web_demo" -B "$TMP_BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$SDK_DIR/cmake/arm-linux-musleabi-toolchain.cmake" \
  -DEHAL_IMU_SDK_DIR="$SDK_DIR"
cmake --build "$TMP_BUILD" -j2
mkdir -p "$OUT_DIR"
cp -f "$TMP_BUILD/imu_web_demo" "$OUT_DIR/imu_web_demo"
chmod +x "$OUT_DIR/imu_web_demo"
echo "Built $OUT_DIR/imu_web_demo"
