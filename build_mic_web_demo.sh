#!/usr/bin/env bash
set -euo pipefail
SDK_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT_DIR=$(cd "$SDK_DIR/.." && pwd)
BUILD_DIR="$SDK_DIR/build/mic_web_demo"
export PATH="$HOME/.local/workcard-toolchain/gcc-20250305-arm-v01c02-linux-musleabi/arm-v01c02-linux-musleabi-gcc/bin:$PATH"
cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build"
cmake --build "$ROOT_DIR/build" --target ehal_audio -j4
cp "$ROOT_DIR/build/libehal_audio.so" "$SDK_DIR/lib/libehal_audio.so"
cmake -S "$SDK_DIR/examples" -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE="$SDK_DIR/cmake/arm-linux-musleabi-toolchain.cmake"
cmake --build "$BUILD_DIR" --target mic_web_demo -j4
PACKAGE="$BUILD_DIR/package"
# Only replace this script's package directory under the SDK build tree.
case "$PACKAGE" in "$SDK_DIR"/build/mic_web_demo/package) ;; *) exit 1 ;; esac
rm -rf -- "$PACKAGE"
mkdir -p "$PACKAGE/bin" "$PACKAGE/lib"
cp "$BUILD_DIR/mic_web_demo" "$PACKAGE/bin/"
cp -a "$SDK_DIR/lib/." "$PACKAGE/lib/"
cp -a "$SDK_DIR/examples/mic_web_demo/"{web,configs,audio} "$PACKAGE/"
cp "$SDK_DIR/examples/mic_web_demo/run_mic_web_demo.sh" "$PACKAGE/"
chmod +x "$PACKAGE/run_mic_web_demo.sh"
tar -czf "$BUILD_DIR/mic_web_demo.tar.gz" -C "$PACKAGE" .
file "$PACKAGE/bin/mic_web_demo"
printf 'Complete package: %s\n' "$BUILD_DIR/mic_web_demo.tar.gz"
