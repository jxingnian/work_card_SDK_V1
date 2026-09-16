# SDK 封装设计

## 设计目标

将摄像头采集、HAL 初始化、视频编码、配置解析、OSD、截图和码流处理
统一封装到 `libehal_camera.so`，通过稳定的 C API 提供摄像头功能。

应用程序只需要使用 `include/ehal_camera.h` 中定义的接口，底层媒体组件和处理流程
由 SDK 统一管理。

## SDK 组成

SDK 运行组成如下：

```text
include/ehal_camera.h
lib/libehal_camera.so
lib/libhal.so
lib/芯片和第三方依赖库
configs/sc2356 单摄配置
font/hzk16
examples/camera_demo.c
```

`include/ehal_camera.h` 是 SDK 的接口层，`libehal_camera.so` 是功能封装层，
HAL、芯片媒体库和 Sensor 库属于运行依赖。

## 实现分层

```text
应用程序
  |
  v
include/ehal_camera.h
  |
  v
libehal_camera.so
  |
  +-- sample_config_load       读取并合并 JSON 配置
  +-- hal_init_with_config     初始化 HAL
  +-- hal_pipeline_runtime     创建 VI/VPSS/VENC 管线
  +-- camera_data_callback     接收视频码流并回调应用程序
  +-- hal_venc_request_idr     请求关键帧
  +-- hal_venc_snapshot...     JPEG 抓图
  |
  v
libhal.so / 芯片库
```

SDK 内部实现包括以下模块：

```text
配置加载与校验
摄像头采集与媒体管线
视频码流处理
JPEG 抓图
时间 OSD 和区域叠加
运行状态统计
```

## API 设计原则

1. API 面向摄像头业务功能，不暴露 VI、VPSS、VENC、RGN 和 pipeline 节点。
2. SDK 统一管理配置加载、HAL 初始化、管线创建、回调注册、启动、停止和释放。
3. 所有函数返回 `EHAL_OK` 或负数错误码，可使用 `ehal_camera_error_string()` 获取错误描述。
4. `ehal_camera_t` 是不透明句柄，应用程序不需要访问 SDK 内部状态。
5. 当前版本使用 sc2356 单摄配置，后续增加 Sensor 时扩展配置和内部实现，保持 API 稳定。

## SDK 内部实现

以下模块编译进 `libehal_camera.so`：

```text
sdk/src/ehal_camera.cpp
samples/common/sample_config.cpp
samples/common/sample_config_validate.cpp
samples/common/sample_pipeline_runtime.cpp
samples/common/sample_stream_runtime.cpp
samples/common/sample_region_targets.cpp
samples/common/sample_snapshot.cpp
samples/common/sample_osd_time.cpp
samples/common/sample_osd_time_runtime.cpp
samples/common/sample_overlay_rects.cpp
samples/common/sample_overlay_runtime.cpp
```

上述文件属于 SDK 实现层，不属于 API 接口层。

## 生命周期

```text
ehal_camera_create()
  -> 创建 SDK 实例

ehal_camera_configure()
  -> 保存配置文件、Sensor 和视频通道参数

ehal_camera_start()
  -> 加载 hal_default.json + hal.json
  -> 选择视频 pipeline
  -> hal_init_with_config()
  -> 创建 pipeline
  -> 启动 pipeline

ehal_camera_stop()
  -> 停止 pipeline
  -> 销毁 pipeline
  -> hal_deinit()

ehal_camera_destroy()
  -> 释放 SDK 实例
```

## 模块对应关系

| 当前文件 | 封装后位置 | 说明 |
| ---- | ---- | ---- |
| `sample_board.cpp` | `sdk/src/ehal_camera.cpp` | 主流程改造成 SDK 生命周期 |
| `samples/common/sample_config.*` | 编进 SDK 库 | JSON 配置加载 |
| `include/hal/hal_pipeline_runtime.h` | HAL 运行依赖 | 管线生命周期 |
| `include/hal/hal_video.h` | HAL 运行依赖 | 视频编码、关键帧和 JPEG 接口 |

## 构建与安装

1. `sdk/src/ehal_camera.cpp` 将视频流程封装为 `create/configure/start/stop/destroy` 生命周期接口。
2. 根项目 CMake 使用 `add_library(ehal_camera SHARED ...)` 构建 SDK 动态库。
3. `libehal_camera.so` 链接 `hal`、`jsoncpp`、芯片库和线程库。
4. 安装内容包括 `include/ehal_camera.h`、运行库、`configs/`、`font/`、示例和文档。
5. 业务应用通过视频帧回调获取编码数据，自行处理后续传输。
