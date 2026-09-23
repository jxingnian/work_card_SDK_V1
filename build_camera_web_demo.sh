#!/bin/bash
set -e

# 设置工具链路径
export PATH=$HOME/.local/workcard-toolchain/gcc-20250305-arm-v01c02-linux-musleabi/arm-v01c02-linux-musleabi-gcc/bin:$PATH

# 项目路径
SDK_DIR=/mnt/d/XingNian/client/Steve669063/ehal_media-master/work_card_SDK_V1
BUILD_DIR=/tmp/camera_web_demo_build_$$

echo "=== 清理临时目录 ==="
rm -rf $BUILD_DIR
mkdir -p $BUILD_DIR

echo "=== 配置CMake ==="
cd $BUILD_DIR
cmake $SDK_DIR/examples/camera_web_demo \
  -DCMAKE_TOOLCHAIN_FILE=$SDK_DIR/cmake/arm-linux-musleabi-toolchain.cmake \
  -DEHAL_CAMERA_SDK_DIR=$SDK_DIR

echo "=== 编译 ==="
make -j2

echo "=== 复制到输出目录 ==="
mkdir -p $SDK_DIR/build/camera_web_demo
cp -f camera_web_demo $SDK_DIR/build/camera_web_demo/

echo "=== 验证 ==="
file $SDK_DIR/build/camera_web_demo/camera_web_demo
ls -lh $SDK_DIR/build/camera_web_demo/camera_web_demo

echo "=== 编译完成 ==="
