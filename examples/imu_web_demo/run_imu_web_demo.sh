#!/bin/sh
set -eu
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PORT=${PORT:-8081}
exec "$DIR/../../build/imu_web_demo/imu_web_demo" --port "$PORT" --web-root "$DIR/web"
