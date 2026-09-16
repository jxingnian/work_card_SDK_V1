# Camera Web Demo

这是一个独立的设备端视频测试程序。程序通过 `libehal_camera.so`
访问摄像头，并在设备端提供 HTTP 配置页面和 WebSocket 视频流。

## 功能

- 浏览器访问设备 IP 查看实时视频
- 通过 WebSocket 推送 H.264/H.265 编码帧
- 配置 Sensor、Pipeline、视频通道、分辨率、帧率和码率
- 切换 H.264/H.265 编码格式
- 修改配置文件路径并重新启动视频链路
- 查看视频运行统计
- 请求 JPEG 抓图

## 运行

程序和网页目录位于同一安装目录时：

```sh
cd /path/to/camera_web_demo
./fix_board_sensor_sc2356.sh
reboot

cd /path/to/camera_web_demo
./run_camera_web_demo.sh
```

`fix_board_sensor_sc2356.sh` updates `/etc/init.d/S90autorun` so the board loads
media modules with `sc2356` instead of the factory default sensor. Run it once
after flashing or replacing the rootfs, then reboot.

`run_camera_web_demo.sh` sets the SDK library path and the board runtime library
paths required by `libehal_camera.so`, then starts the web demo on port `8080`.

电脑浏览器访问：

```text
http://<设备IP>:8080/
```

网页依赖浏览器 WebCodecs。Chrome/Chromium 只会在安全上下文中开放 WebCodecs，
直接通过局域网 HTTP 访问设备 IP 时可能提示“当前浏览器不支持 WebCodecs”。测试时建议
先建立 SSH 本地端口转发：

```powershell
ssh -i $env:USERPROFILE\.ssh\id_ed25519 -L 8080:127.0.0.1:8080 root@172.22.22.168
```

保持该 SSH 窗口不关闭，然后在电脑浏览器访问：

```text
http://127.0.0.1:8080/index.html
```

首次测试建议选择 H.264。网页使用浏览器 WebCodecs 解码原始编码帧，
浏览器需要支持 WebCodecs；H.265 是否可播放由浏览器和操作系统的解码能力决定。

## 命令行参数

| 参数 | 说明 |
| --- | --- |
| `--port <n>` | HTTP 和 WebSocket 服务端口，默认 `8080` |
| `--web-root <path>` | 网页目录，默认 `web` |
| `--default <path>` | 默认配置文件路径 |
| `--runtime <path>` | 运行配置文件路径 |
| `--user <path>` | 用户配置文件路径 |

## 访问接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/` | 视频测试页面 |
| `GET` | `/api/status` | 获取当前运行状态和统计 |
| `POST` | `/api/config` | 应用视频配置并重新启动视频链路 |
| `GET` | `/snapshot.jpg` | 获取当前通道 JPEG 图片 |
| WebSocket | `/ws` | 接收原始 H.264/H.265 编码帧 |

## 连接关系

```text
浏览器
  | HTTP
  | WebSocket /ws
  v
camera_web_demo
  |
  | ehal_camera.h
  v
libehal_camera.so
```

## 独立构建

示例可以使用已经安装的 SDK 单独构建，不需要加入 SDK 源码工程：

```sh
cmake -S . -B build -DEHAL_CAMERA_SDK_DIR=/path/to/sdk
cmake --build build
```
