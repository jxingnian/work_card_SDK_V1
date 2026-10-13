#!/bin/bash
set -euo pipefail

# 固定使用 SDK 随附的 ARM musl 交叉工具链，保证产物与目标板的 armv7l/musl 环境一致。
export PATH="$HOME/.local/workcard-toolchain/gcc-20250305-arm-v01c02-linux-musleabi/arm-v01c02-linux-musleabi-gcc/bin:$PATH"

# 解析 SDK、临时构建目录和最终产物目录。
# 临时 CMake 文件统一放入 SDK 的 tmp，最终可交付文件统一放入 SDK 的 build。
SDK_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SDK_DIR/.." && pwd)
TMP_BUILD="$SDK_DIR/tmp/imu_web_demo_build"
OUT_DIR="$SDK_DIR/build/imu_web_demo"
PACKAGE_DIR="$OUT_DIR/package"
PACKAGE_TAR="$OUT_DIR/imu_web_demo.tar.gz"

# 清理本 Demo 上一次的临时构建结果和完整部署包，避免旧文件残留到本次交付物。
rm -rf -- "$TMP_BUILD" "$PACKAGE_DIR" "$PACKAGE_TAR"

# 使用 SDK 的交叉编译工具链构建 IMU Web Demo。
cmake -S "$SDK_DIR/examples/imu_web_demo" -B "$TMP_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$SDK_DIR/cmake/arm-linux-musleabi-toolchain.cmake" \
    -DEHAL_IMU_SDK_DIR="$SDK_DIR"
cmake --build "$TMP_BUILD" -j2

# 组装板端完整运行包：可执行文件、IMU SDK 动态库、网页和启动脚本必须同时部署。
mkdir -p "$PACKAGE_DIR/bin" "$PACKAGE_DIR/lib"
cp -f "$TMP_BUILD/imu_web_demo" "$PACKAGE_DIR/bin/imu_web_demo"
cp -f "$SDK_DIR/lib/libehal_imu.so" "$PACKAGE_DIR/lib/libehal_imu.so"
cp -a "$SDK_DIR/examples/imu_web_demo/web" "$PACKAGE_DIR/web"
cp -f "$SDK_DIR/examples/imu_web_demo/run_imu_web_demo.sh" "$PACKAGE_DIR/run_imu_web_demo.sh"
chmod +x "$PACKAGE_DIR/bin/imu_web_demo" "$PACKAGE_DIR/run_imu_web_demo.sh"

# 生成单一完整压缩包，部署时整体替换板端专用目录，不进行增量复制。
tar -czf "$PACKAGE_TAR" -C "$PACKAGE_DIR" .

# 输出可核验信息，便于确认架构和交付包位置。
file "$PACKAGE_DIR/bin/imu_web_demo"
sha256sum "$PACKAGE_TAR"
echo "Complete package: $PACKAGE_TAR"
