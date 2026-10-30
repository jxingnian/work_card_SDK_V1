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

## 按键与 LED 测试

网页侧边栏中的“按键与 LED”页面包含以下功能：

- 按键使用 SDK `ehal_key` 监听 `board-keys` 输入设备，显示 `function`、`volume_up`、`power`、`volume_down` 的按下、释放、长按计数和最近事件。
- LED 使用板端 `xn-ws2812` 驱动的 `brightness` 和 `color` 节点，网页支持点亮、熄灭、调亮度、设置 RGB 颜色并读取内核节点实际值。

接口如下：

```text
GET  /api/v1/key/status
GET  /api/v1/led/status
POST /api/v1/led/set       body: {"enabled":true|false,"red":0-255,"green":0-255,"blue":0-255,"brightness":0-255}
```

启动参数如下：

```text
--key-device-name board-keys
--led-path /sys/devices/platform/ws2812/brightness
--led-color-path /sys/devices/platform/ws2812/color
```

如果板端驱动路径不同，通过 `--led-path` 和 `--led-color-path` 传入实际节点；程序会回读亮度和 RGB 值，网页状态会明确报告节点是否可读写。

## 电池测试

网页侧边栏中的“电池测试”页面使用板端 LSADC 采集电池电压，并显示：

- 当前电池电压、ADC 原始值和电量百分比。
- 启动本 Demo 后的电量曲线，1 秒采样一次，最多保存 3600 个点。
- 清空曲线记录，不会停止 ADC 采样。

接口如下：

```text
GET  /api/v1/battery/status
GET  /api/v1/battery/curve
POST /api/v1/battery/curve/clear
```

电压换算依据沿用 `battery_adc_demo`：10 位 ADC、3.3V 参考、R6/R11 分压倍率 3。电量百分比采用单节锂电池工程显示区间 3.30V=0%、4.20V=100%，原始电压和 ADC 码同时显示，便于使用万用表校准后复核。

电池 ADC 与其他直接使用 `GPIO1_5/LSADC_CH1` 的程序互斥。部署脚本会先停止旧 `hardware_test_demo`，完整替换 `/opt/hardware_test_demo`；不要同时运行独立 `battery_adc_demo`。

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

第二阶段协议接口：

```text
POST /api/v1/bluetooth/gatt
POST /api/v1/bluetooth/a2dp
POST /api/v1/bluetooth/hfp
POST /api/v1/bluetooth/avrcp
POST /api/v1/bluetooth/test_voice
```

GATT 写入数据使用偶数长度十六进制字符串 `data_hex`；GATT 的发现、读取和通知接口会返回 SDK 最近一次输出。A2DP、HFP 和 AVRCP 操作都要求先填写目标设备地址，并将厂家 SDK 返回的错误码直接显示在操作结果中。

`/api/v1/bluetooth/test_voice` 只需要传入已连接的经典蓝牙 A2DP 设备地址，Demo 会播放自带的 `audio/bluetooth-test.wav` 测试语音，不需要再填写板端文件路径。

网页扫描列表中的“一键配对并连接”会按配对、信任、连接顺序执行，并自动将成功连接的设备地址设置为当前测试目标。
