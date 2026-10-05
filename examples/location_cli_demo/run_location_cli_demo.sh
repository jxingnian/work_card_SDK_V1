#!/bin/sh
# 定位获取 demo 运行脚本

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
cd "$SCRIPT_DIR" || exit 1

./location_cli_demo
