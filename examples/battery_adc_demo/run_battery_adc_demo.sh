#!/bin/sh
set -eu
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ ! -c /dev/ot_lsadc ]; then
    insmod /komod/ot_adc.ko
fi
if [ -x "$DIR/bin/battery_adc_demo" ]; then
    exec "$DIR/bin/battery_adc_demo" "$@"
fi
exec "$DIR/battery_adc_demo" "$@"
