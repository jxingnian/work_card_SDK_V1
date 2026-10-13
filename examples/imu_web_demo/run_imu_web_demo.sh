#!/bin/sh
set -eu

# 获取当前完整包的根目录。
# 运行包固定包含 bin、lib、web 三个目录，因此脚本不依赖源码工程的相对路径。
PACKAGE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
APP="$PACKAGE_DIR/bin/imu_web_demo"
WEB_ROOT="$PACKAGE_DIR/web"

# 允许通过环境变量覆盖 HTTP 端口，未设置时使用 IMU Demo 的默认端口 8081。
PORT=${PORT:-8081}

# 运行前明确检查包内文件，避免因上传不完整而出现不明确的动态加载错误。
if [ ! -x "$APP" ]; then
    echo "imu_web_demo executable not found: $APP" >&2
    exit 127
fi
if [ ! -f "$WEB_ROOT/index.html" ]; then
    echo "IMU web root not found: $WEB_ROOT" >&2
    exit 127
fi

# 优先加载完整包中的 SDK 库，保持板端运行环境与开发机、系统其他 Demo 解耦。
export LD_LIBRARY_PATH="$PACKAGE_DIR/lib:${LD_LIBRARY_PATH:-}"

# 使用包内可执行文件和网页目录启动服务。
exec "$APP" --port "$PORT" --web-root "$WEB_ROOT"
