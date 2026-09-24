#!/bin/bash
set -e

# 设置工具链路径
export PATH=$HOME/.local/workcard-toolchain/gcc-20250305-arm-v01c02-linux-musleabi/arm-v01c02-linux-musleabi-gcc/bin:$PATH

# 项目路径
SDK_DIR=/mnt/d/XingNian/client/Steve669063/ehal_media-master/work_card_SDK_V1
BUILD_DIR=$SDK_DIR/build/mic_web_demo

echo "=== 清理构建目录 ==="
rm -rf $BUILD_DIR
mkdir -p $BUILD_DIR

echo "=== 配置CMake ==="
cd $BUILD_DIR
cmake $SDK_DIR/examples \
  -DCMAKE_TOOLCHAIN_FILE=$SDK_DIR/cmake/arm-linux-musleabi-toolchain.cmake

echo "=== 编译mic_web_demo ==="
make mic_web_demo -j4

echo "=== 复制资源文件 ==="
# 复制web文件
if [ -d "$SDK_DIR/examples/mic_web_demo/web" ]; then
    cp -r $SDK_DIR/examples/mic_web_demo/web $BUILD_DIR/
fi

# 复制配置文件
if [ -d "$SDK_DIR/examples/mic_web_demo/configs" ]; then
    cp -r $SDK_DIR/examples/mic_web_demo/configs $BUILD_DIR/
fi

# 复制音频文件
if [ -d "$SDK_DIR/examples/mic_web_demo/audio" ]; then
    cp -r $SDK_DIR/examples/mic_web_demo/audio $BUILD_DIR/
fi

# 复制运行脚本
if [ -f "$SDK_DIR/examples/mic_web_demo/run_mic_web_demo.sh" ]; then
    cp $SDK_DIR/examples/mic_web_demo/run_mic_web_demo.sh $BUILD_DIR/
    chmod +x $BUILD_DIR/run_mic_web_demo.sh
fi

echo "=== 验证 ==="
file $BUILD_DIR/mic_web_demo
ls -lh $BUILD_DIR/mic_web_demo

echo "=== 编译完成 ==="
echo "输出目录: $BUILD_DIR"
