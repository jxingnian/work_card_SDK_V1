# mic_web_demo - 麦克风Web演示

独立的设备端麦克风音频采集与Web播放Demo，通过WebSocket实时传输音频流。

## 功能特性

- ✅ **实时音频采集** - 支持PCM/Opus格式
- ✅ **Web实时播放** - 浏览器端WebSocket接收并播放
- ✅ **HTTP配置接口** - `http://<device-ip>:8091/`
- ✅ **音频格式配置** - 采样率、通道数、编码格式可调
- ✅ **本地音频播放** - 支持播放本地音频文件

## 架构说明

```
麦克风 → libehal_audio.so → PCM/Opus编码 
  → WebSocket传输 → 浏览器AudioContext播放
```

---

## 编译

### 环境要求
- **操作系统**: WSL2 / Ubuntu 20.04+
- **工具链**: 已在SDK中提供，运行 `./docs/开发环境/scripts/setup_dev_env.sh` 安装

### 编译步骤

在WSL中执行：

```bash
cd /mnt/d/XingNian/client/Steve669063/ehal_media-master/work_card_SDK_V1

# 使用编译脚本（推荐）
bash build_mic_web_demo.sh
```

**输出:** `build/mic_web_demo/mic_web_demo: ELF 32-bit LSB executable, ARM`

### 手动编译

如需手动编译：

```bash
cd /mnt/d/XingNian/client/Steve669063/ehal_media-master/work_card_SDK_V1

# 清理旧编译产物
rm -rf build/mic_web_demo

# 配置CMake
cmake \
  -S examples \
  -B build/mic_web_demo \
  -DCMAKE_TOOLCHAIN_FILE=$(pwd)/cmake/arm-linux-musleabi-toolchain.cmake

# 编译mic_web_demo
cd build/mic_web_demo
make mic_web_demo -j4

# 验证
file mic_web_demo
```

---

## 打包

生成完整部署包：

```bash
#!/bin/bash
set -e

SDK=/mnt/d/XingNian/client/Steve669063/ehal_media-master/work_card_SDK_V1
BUILD="$SDK/build/mic_web_demo"

# 清理并创建打包目录
rm -rf "$BUILD/package"
mkdir -p "$BUILD/package/bin" "$BUILD/package/lib"

# 复制可执行文件
cp "$BUILD/mic_web_demo" "$BUILD/package/bin/"

# 复制配置文件
cp -r "$SDK/examples/mic_web_demo/configs" "$BUILD/package/"

# 复制Web资源
cp -r "$SDK/examples/mic_web_demo/web" "$BUILD/package/"

# 复制音频文件
cp -r "$SDK/examples/mic_web_demo/audio" "$BUILD/package/"

# 复制运行脚本
cp "$SDK/examples/mic_web_demo/run_mic_web_demo.sh" "$BUILD/package/"

# 复制依赖库
cp -a "$SDK/lib"/*.so* "$BUILD/package/lib/"

# 设置执行权限
chmod +x "$BUILD/package/bin/mic_web_demo" "$BUILD/package/run_mic_web_demo.sh"

# 打包
cd "$BUILD"
rm -f mic_web_demo.tar.gz mic_web_demo.tar.gz.sha256
tar -czf mic_web_demo.tar.gz -C package .
sha256sum mic_web_demo.tar.gz > mic_web_demo.tar.gz.sha256
sha256sum -c mic_web_demo.tar.gz.sha256

echo "✅ 打包完成: $BUILD/mic_web_demo.tar.gz"
ls -lh "$BUILD/mic_web_demo.tar.gz"
```

**输出:** `work_card_SDK_V1/build/mic_web_demo/mic_web_demo.tar.gz`

---

## 部署

### 上传到板端

**Windows PowerShell:**
```powershell
$sdk = 'D:\XingNian\client\Steve669063\ehal_media-master\work_card_SDK_V1'
scp "$sdk\build\mic_web_demo\mic_web_demo.tar.gz" `
    root@192.168.137.47:/tmp/
```

**WSL/Linux:**
```bash
scp work_card_SDK_V1/build/mic_web_demo/mic_web_demo.tar.gz \
    root@192.168.137.47:/tmp/
```

### 板端部署

通过SSH或串口连接到设备：

```bash
# SSH连接（需要使用Python脚本连接）
ssh root@192.168.137.47
# 密码: ebaina

# 清理旧部署
rm -rf /home/xingnian/mic_web_demo

# 解压新包
mkdir -p /home/xingnian/mic_web_demo
cd /home/xingnian/mic_web_demo
tar -xzf /tmp/mic_web_demo.tar.gz

# 设置执行权限
chmod +x bin/mic_web_demo run_mic_web_demo.sh

# 启动
./run_mic_web_demo.sh
```

**预期输出：**
```
mic web demo listening on port 8091
WebSocket server ready
```

---

## 使用

### 1. Web界面访问

浏览器访问：`http://192.168.137.47:8091/`

**功能说明：**
- 实时音频播放控制（开始/停止）
- 音频格式配置
- 采样率、通道数调整
- 音量控制

### 2. 配置音频参数

**HTTP API接口：**
```bash
# 获取状态
curl http://192.168.137.47:8091/api/status

# 配置音频参数（PCM格式）
curl -X POST http://192.168.137.47:8091/api/config \
  -H "Content-Type: application/json" \
  -d '{
    "format": "pcm",
    "sample_rate": 16000,
    "channels": 1,
    "bit_depth": 16
  }'

# 配置音频参数（Opus格式）
curl -X POST http://192.168.137.47:8091/api/config \
  -H "Content-Type: application/json" \
  -d '{
    "format": "opus",
    "sample_rate": 16000,
    "channels": 1,
    "bitrate": 64000
  }'

# 播放本地音频
curl -X POST http://192.168.137.47:8091/api/playback/start

# 停止播放
curl -X POST http://192.168.137.47:8091/api/playback/stop
```

### 3. 检查运行状态

```bash
# 检查进程
ps aux | grep mic_web_demo

# 检查端口
netstat -an | grep 8091

# 查看系统负载
top -b -n 1 | head -5
```

---

## 配置文件说明

### hal_default.json
HAL层默认配置，包含音频设备初始化参数。

### pcm_runtime.json
PCM格式运行时配置：
```json
{
  "sample_rate": 16000,
  "channels": 1,
  "bit_depth": 16
}
```

### opus_runtime.json
Opus格式运行时配置：
```json
{
  "sample_rate": 16000,
  "channels": 1,
  "bitrate": 64000,
  "complexity": 10
}
```

---

## 故障排查

### 问题1: 无法连接WebSocket

**原因：** 服务未启动或端口被占用

**解决：**
```bash
# 检查8091端口
netstat -an | grep 8091

# 检查进程
ps aux | grep mic_web_demo

# 查看日志
./run_mic_web_demo.sh 2>&1 | tee demo.log
```

### 问题2: 没有声音或音质差

**原因：** 音频配置不当或硬件问题

**解决：**
1. 检查麦克风硬件连接
2. 调整采样率：推荐16000Hz或48000Hz
3. 检查音量设置
4. 查看日志中的错误信息

### 问题3: 浏览器无法播放

**原因：** 浏览器不支持或安全限制

**解决：**
1. 使用Chrome/Edge等现代浏览器
2. 确保使用HTTP访问（HTTPS需要证书）
3. 检查浏览器控制台错误信息
4. 允许浏览器访问音频权限

### 问题4: 音频延迟高

**原因：** 网络延迟或缓冲区设置

**解决：**
1. 使用有线网络连接
2. 降低音频质量（采样率/码率）
3. 检查网络延迟：`ping 192.168.137.47`

---

## 性能参数

| 指标 | PCM格式 | Opus格式 | 说明 |
|------|---------|----------|------|
| 板端CPU | 1-2% | 2-3% | Opus需编码 |
| 网络带宽 | 256kbps | 64kbps | Opus压缩更好 |
| 延迟 | 100-200ms | 150-300ms | PCM更低 |
| 音质 | 无损 | 有损压缩 | Opus在低码率下更优 |

**推荐配置：**
- **低延迟场景**: PCM, 16000Hz, 单声道
- **低带宽场景**: Opus, 16000Hz, 64kbps
- **高音质场景**: PCM, 48000Hz, 双声道

---

## 依赖库

本demo依赖以下库（已包含在部署包中）：

- `libehal_audio.so` - 音频封装库
- `libhal.so` - HAL底层库
- `libss_mpi_audio.so` - 芯片音频MPI库
- `libopus.so` - Opus编解码库
- `libvqe_*.so` - 音频质量增强库
- `libaac_*.so` - AAC编解码库
- `libmp3_*.so` - MP3编解码库

---

## 开发说明

### 目录结构
```
work_card_SDK_V1/examples/mic_web_demo/
├── mic_web_demo.cpp             # 主程序
├── web/
│   ├── index.html               # Web界面
│   ├── style.css                # 样式文件
│   └── app.js                   # WebSocket客户端
├── configs/
│   ├── hal_default.json         # HAL默认配置
│   ├── pcm_runtime.json         # PCM配置
│   └── opus_runtime.json        # Opus配置
├── audio/
│   └── speaker_voice.wav        # 测试音频文件
├── run_mic_web_demo.sh          # 运行脚本
└── readme.md                    # 本文档
```

### 运行参数

```bash
mic_web_demo [options]

选项:
  --port <port>                  HTTP/WebSocket端口 (默认: 8091)
  --web-root <path>              Web资源目录
  --default-config <path>        HAL默认配置文件
  --pcm-runtime-config <path>    PCM运行配置文件
  --opus-runtime-config <path>   Opus运行配置文件
  --playback-audio <path>        播放音频文件路径
```

### WebSocket协议

**客户端 → 服务端：**
```json
{
  "type": "start",
  "format": "pcm",
  "sample_rate": 16000,
  "channels": 1
}

{
  "type": "stop"
}
```

**服务端 → 客户端：**
```
二进制消息: 音频数据流 (PCM/Opus)
```

---

## 版本信息

- **版本**: 1.0.0
- **日期**: 2024-09-24
- **作者**: Ebaina Technology Community
- **功能**: 麦克风Web实时采集与播放

---

## 许可证

Copyright (C), 2023-2028, Ebaina Technology Community (www.ebaina.com)

---

## 技术支持

如遇问题，请检查：
1. 工具链是否正确安装
2. 依赖库是否完整部署
3. 麦克风硬件是否正常
4. 网络连接是否正常
5. 浏览器是否支持WebSocket和AudioContext

**注意事项：**
- SSH连接时需要使用Python脚本连接（根据开发提示词）
- 调试时优先使用串口
- 部署时需要完整清理旧包并重新部署，不支持增量部署
