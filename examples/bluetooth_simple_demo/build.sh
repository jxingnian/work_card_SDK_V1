#!/bin/bash

set -e

cd "$(dirname "$0")"

PROJECT_ROOT="$(pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
EXAMPLE_NAME="bluetooth_simple_demo"

echo "======================================"
echo "编译 ${EXAMPLE_NAME}"
echo "======================================"

# 清理旧的编译目录
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

cd "${BUILD_DIR}"

# 使用CMake配置
cmake ..

# 编译
make -j$(nproc)

echo ""
echo "======================================"
echo "编译完成！"
echo "======================================"
echo "可执行文件: ${BUILD_DIR}/bluetooth_simple_demo"
echo ""
echo "运行方式："
echo "  cd ${BUILD_DIR}"
echo "  ./bluetooth_simple_demo --port 8080 --web-root ../web"
echo ""
echo "或使用启动脚本："
echo "  cd ${BUILD_DIR}"
echo "  cp ../run_bluetooth_simple_demo.sh ."
echo "  chmod +x run_bluetooth_simple_demo.sh"
echo "  ./run_bluetooth_simple_demo.sh"
echo ""
