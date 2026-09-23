#!/bin/bash
set -e

SDK=/mnt/d/XingNian/client/Steve669063/ehal_media-master/work_card_SDK_V1
BUILD="$SDK/build/camera_web_demo"

echo "=== 清理并创建打包目录 ==="
rm -rf "$BUILD/package"
mkdir -p "$BUILD/package/bin" "$BUILD/package/lib" "$BUILD/package/configs"

echo "=== 复制可执行文件 ==="
cp "$BUILD/camera_web_demo" "$BUILD/package/bin/"

echo "=== 复制配置文件 ==="
cp -a "$SDK/configs" "$BUILD/package/"

echo "=== 复制字体和Web资源 ==="
cp -a "$SDK/font" "$SDK/examples/camera_web_demo/web" "$BUILD/package/"

echo "=== 复制运行脚本 ==="
cp "$SDK/examples/camera_web_demo/run_camera_web_demo.sh" \
   "$SDK/examples/camera_web_demo/fix_board_sensor_sc2356.sh" \
   "$BUILD/package/"

echo "=== 复制依赖库（包括RTSP库） ==="
cp -a "$SDK/lib"/*.so* "$BUILD/package/lib/"

echo "=== 设置执行权限 ==="
chmod +x "$BUILD/package/bin/camera_web_demo" "$BUILD/package"/*.sh

echo "=== 打包 ==="
cd "$BUILD"
rm -f camera_web_demo_rtsp.tar.gz camera_web_demo_rtsp.tar.gz.sha256
tar -czf camera_web_demo_rtsp.tar.gz -C package .
sha256sum camera_web_demo_rtsp.tar.gz > camera_web_demo_rtsp.tar.gz.sha256
sha256sum -c camera_web_demo_rtsp.tar.gz.sha256

echo "=== 打包完成 ==="
ls -lh "$BUILD/camera_web_demo_rtsp.tar.gz"
