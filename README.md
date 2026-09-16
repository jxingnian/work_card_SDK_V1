# eHal Camera SDK V1

eHal Camera SDK V1 提供摄像头采集、视频编码、视频帧回调、JPEG 抓图和运行状态统计接口。
应用程序通过 `include/ehal_camera.h` 调用 SDK API，并链接 `lib/libehal_camera.so`。

## SDK 架构

```text
应用程序
  |
  | include/ehal_camera.h
  v
lib/libehal_camera.so
  |
  | 摄像头采集 / 编码 / 视频帧回调 / 抓图 / 状态统计
  v
运行依赖库
  |
  | libhal.so / 板端媒体库 / Sensor 库
  v
sc2356 摄像头
```

## 目录结构

| 路径 | 说明 |
| ---- | ---- |
| `include/` | SDK 对外 API 头文件 |
| `lib/` | SDK 动态库和板端运行依赖库 |
| `configs/` | sc2356 单摄运行配置 |
| `font/` | OSD 字体资源 |
| `examples/` | API 调用示例 |
| `docs/` | API 和封装设计说明 |

## API 模块

| 模块 | 接口 |
| ---- | ---- |
| 生命周期管理 | `ehal_camera_create`、`ehal_camera_destroy` |
| 采集控制 | `ehal_camera_start`、`ehal_camera_stop` |
| 视频帧回调 | `ehal_camera_set_video_callback` |
| JPEG 抓图 | `ehal_camera_snapshot` |
| 编码参数 | `ehal_camera_set_bitrate` |
| 状态统计 | `ehal_camera_get_stats` |

## 集成方式

应用程序包含 SDK 头文件：

```c
#include "ehal_camera.h"
```

初始化并启动摄像头：

```c
ehal_camera_config_t config = {0};
config.default_config_path = "configs/hal_default.json";
config.runtime_config_path = "configs/hal.json";
config.pipeline_name = "video_record";
config.sensor_name = "sc2356";
config.channel_count = 1;
config.channels[0].channel = 0;
config.channels[0].codec = EHAL_VIDEO_CODEC_H265;
config.video_callback = on_video_frame;

ehal_camera_t *camera = NULL;
ehal_camera_create(&camera);
ehal_camera_configure(camera, &config);
ehal_camera_start(camera);
```

编译应用程序时包含 `include/`，并链接 `lib/libehal_camera.so`。

## 运行资源

板端运行目录需要包含：

```text
bin/<app>
lib/libehal_camera.so
lib/libhal.so
lib/*.so
configs/hal_default.json
configs/hal.json
configs/hal_sc2356.json
configs/sc2356.json
font/hzk16
```

## 配置说明

当前版本使用 sc2356 单摄配置：

| 文件 | 说明 |
| ---- | ---- |
| `configs/hal_default.json` | 默认媒体参数 |
| `configs/hal.json` | 运行配置入口 |
| `configs/hal_sc2356.json` | sc2356 HAL 配置 |
| `configs/sc2356.json` | sc2356 Sensor 配置 |
