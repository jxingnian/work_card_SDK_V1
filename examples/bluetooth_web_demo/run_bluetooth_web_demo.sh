#!/bin/sh

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="$ROOT_DIR/lib:${LD_LIBRARY_PATH:-}"
exec "$ROOT_DIR/bin/bluetooth_web_demo" --port "${1:-8080}" --web-root "$ROOT_DIR/web"
