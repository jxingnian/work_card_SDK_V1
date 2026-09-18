#!/bin/sh
set -eu
ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
PORT=${PORT:-8090}
IFACE=${IFACE:-wlan0}
WPA_CONFIG=${WPA_CONFIG:-/etc/wireless/wpa_supplicant.conf}
export LD_LIBRARY_PATH="$ROOT_DIR/lib:/app/lib/lib-hal:/app/lib/lib-chip:/app/lib/lib-strms:/app/lib/lib-thrid:/app/lib:${LD_LIBRARY_PATH:-}"
exec "$ROOT_DIR/bin/wifi_web_demo" --port "$PORT" --web-root "$ROOT_DIR/web" --interface "$IFACE" --wpa-config "$WPA_CONFIG"
