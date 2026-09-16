#!/bin/sh

set -eu

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$DIR/../.." && pwd)

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

export LD_LIBRARY_PATH="$ROOT_DIR/lib:/app/lib/lib-hal:/app/lib/lib-chip:/app/lib/lib-strms:/app/lib/lib-thrid:/app/lib:${LD_LIBRARY_PATH:-}"

exec "$APP" \
    --port "$PORT" \
    --web-root "$WEB_ROOT" \
    --default "$DEFAULT_CFG" \
    --runtime "$RUNTIME_CFG" \
    "$@"
