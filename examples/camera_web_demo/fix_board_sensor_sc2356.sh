#!/bin/sh

set -eu

AUTORUN=${AUTORUN:-/etc/init.d/S90autorun}

if [ ! -f "$AUTORUN" ]; then
    echo "missing autorun script: $AUTORUN" >&2
    exit 1
fi

if grep -q '/root/insmod.sh sc2356' "$AUTORUN"; then
    echo "board sensor already configured as sc2356"
    exit 0
fi

if ! grep -q '/root/insmod.sh ' "$AUTORUN"; then
    echo "no /root/insmod.sh sensor line found in $AUTORUN" >&2
    exit 1
fi

cp "$AUTORUN" "$AUTORUN.bak"
sed -i '/^[[:space:]]*\/root\/insmod.sh /s#/root/insmod.sh [^[:space:]]*#/root/insmod.sh sc2356#' "$AUTORUN"

echo "updated $AUTORUN to use sc2356"
echo "backup saved to $AUTORUN.bak"
echo "reboot the board before starting camera_web_demo"
