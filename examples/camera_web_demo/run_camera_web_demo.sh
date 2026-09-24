#!/bin/sh

set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR="$DIR"

if [ -x "$ROOT_DIR/bin/camera_web_demo" ]; then
    APP="$ROOT_DIR/bin/camera_web_demo"
elif [ -x "$DIR/camera_web_demo" ]; then
    ROOT_DIR="$DIR"
    APP="$DIR/camera_web_demo"
else
    echo "camera_web_demo executable not found" >&2
    exit 127
fi

PORT=${PORT:-8080}
WEB_ROOT=${WEB_ROOT:-$ROOT_DIR/web}
DEFAULT_CFG=${DEFAULT_CFG:-$ROOT_DIR/configs/hal_default.json}
RUNTIME_CFG=${RUNTIME_CFG:-$ROOT_DIR/configs/hal.json}
RTSP_INTERFACE=${RTSP_INTERFACE:-wlan0}
RTSP_PORT=${RTSP_PORT:-8554}
RTSP_CONFIG_TEMPLATE=${RTSP_CONFIG_TEMPLATE:-$ROOT_DIR/configs/rtsp_config.xml}
RTSP_CONFIG=${RTSP_CONFIG:-/tmp/camera_web_demo_rtsp_config.xml}

RTSP_BIND_IP=$(
    ip -4 addr show "$RTSP_INTERFACE" 2>/dev/null |
        awk '/inet / { split($2, address, "/"); print address[1]; exit }'
)
if [ -z "$RTSP_BIND_IP" ]; then
    RTSP_BIND_IP=0.0.0.0
fi

awk -v bind_ip="$RTSP_BIND_IP" -v rtsp_port="$RTSP_PORT" '
    !ip_done && sub(/<serverip>[^<]*<\/serverip>/,
                    "<serverip>" bind_ip "</serverip>") {
        ip_done = 1
    }
    !port_done && sub(/<serverport>[^<]*<\/serverport>/,
                      "<serverport>" rtsp_port "</serverport>") {
        port_done = 1
    }
    { print }
' "$RTSP_CONFIG_TEMPLATE" > "$RTSP_CONFIG"
export RTSP_CONFIG

echo "RTSP bind address: $RTSP_BIND_IP:$RTSP_PORT"

export LD_LIBRARY_PATH="$ROOT_DIR/lib:/app/lib/lib-hal:/app/lib/lib-chip:/app/lib/lib-strms:/app/lib/lib-thrid:/app/lib:${LD_LIBRARY_PATH:-}"

exec "$APP" \
    --port "$PORT" \
    --web-root "$WEB_ROOT" \
    --default "$DEFAULT_CFG" \
    --runtime "$RUNTIME_CFG" \
    "$@"
