#!/bin/sh

cd "$(dirname "$0")" || exit 1

# 确保蓝牙服务运行
if ! pgrep bluetoothd > /dev/null; then
    echo "启动蓝牙服务..."
    bluetoothd &
    sleep 2
fi

# 确保bluealsa运行
if ! pgrep bluealsa > /dev/null; then
    echo "启动BlueALSA服务..."
    bluealsa -p a2dp-source -p a2dp-sink &
    sleep 1
fi

# 启动demo程序
echo "启动蓝牙演示程序..."
./bluetooth_simple_demo --port 8080 --web-root ./web
