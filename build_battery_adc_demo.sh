#!/bin/bash
set -e
export PATH="$HOME/.local/workcard-toolchain/gcc-20250305-arm-v01c02-linux-musleabi/arm-v01c02-linux-musleabi-gcc/bin:$PATH"
SDK_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TMP_BUILD="$SDK_DIR/build/battery_adc_demo/cmake"
OUT_DIR="$SDK_DIR/build/battery_adc_demo"
cmake -S "$SDK_DIR/examples" -B "$TMP_BUILD" -DCMAKE_TOOLCHAIN_FILE="$SDK_DIR/cmake/arm-linux-musleabi-toolchain.cmake"
cmake --build "$TMP_BUILD" --target battery_adc_demo -j2
mkdir -p "$OUT_DIR"
cp "$TMP_BUILD/battery_adc_demo" "$OUT_DIR/"
cp "$SDK_DIR/examples/battery_adc_demo/run_battery_adc_demo.sh" "$OUT_DIR/"
cp "$SDK_DIR/examples/battery_adc_demo/README.md" "$OUT_DIR/"
chmod +x "$OUT_DIR/battery_adc_demo" "$OUT_DIR/run_battery_adc_demo.sh"
tar -czf "$OUT_DIR/battery_adc_demo.tar.gz" -C "$OUT_DIR" battery_adc_demo run_battery_adc_demo.sh README.md
echo "Built $OUT_DIR/battery_adc_demo"
