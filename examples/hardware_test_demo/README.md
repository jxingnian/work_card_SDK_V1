# Hardware Test Demo

本 Demo 是独立硬件测试程序，WiFi 和摄像头由同一个进程提供服务。

摄像头固定使用厂家 `ehal_camera` API，输出参数为 `640x480、5 FPS`。浏览器通过本 Demo 自己的 HTTP 接口获取 JPEG 图像，不依赖 `8093` 端口，也不依赖其他摄像头 Demo。

## 构建

在 WSL 中执行：

```sh
bash build/hardware_test_demo/build_hardware_test_demo.sh
bash build/hardware_test_demo/package_hardware_test_demo.sh
```

产物：

```text
build/hardware_test_demo/hardware_test_demo.tar.gz
```

## 部署

```sh
BOARD_IP=<板端实际地址> bash build/hardware_test_demo/deploy_hardware_test_demo.sh
```

部署脚本会完整替换 `/opt/hardware_test_demo`，不会修改 `/etc/init.d/S90autorun.bak` 或 `/root/insmod.sh`。

如果 SSH 不通，先通过串口确认板端实际 SSH 地址，再执行部署脚本。

## 运行

```sh
cd /opt/hardware_test_demo
sh run_hardware_test_demo.sh
```

浏览器访问：

```text
http://<板端实际地址>:8090/
```

摄像头接口：

```text
GET /api/v1/camera/status
GET /api/v1/camera/snapshot.jpg
```

状态接口必须返回 `width=640`、`height=480`、`fps=5` 和 `running=true`。

## 蓝牙测试

本 Demo 通过 SDK `ehal_bluetooth` 独立提供蓝牙测试，不启动或依赖其他蓝牙 Demo。

网页侧边栏中的“蓝牙”页面支持：

- 读取适配器状态、地址、名称、厂商、供电、可配对和扫描状态
- 开始扫描、停止扫描、刷新附近设备
- 查看设备地址、名称、RSSI、Classic/BLE 类型、配对、信任、连接和阻止状态
- 执行配对、取消配对、信任、取消信任、连接、断开和删除设备

蓝牙接口：

```text
GET  /api/v1/bluetooth/status
GET  /api/v1/bluetooth/devices
POST /api/v1/bluetooth/scan/start
POST /api/v1/bluetooth/scan/stop
POST /api/v1/bluetooth/action
```

`/api/v1/bluetooth/action` 的 JSON 参数为 `action`、`address`；信任和阻止操作还需要 `enabled` 布尔值。
