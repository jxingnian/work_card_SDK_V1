# 左右麦克风 Web Demo

网页地址：`http://<开发板IP>:8091/`。

左麦 MIC1（AC_INL/P，模组 28 脚）和右麦 MIC2（AC_INR/P，26 脚）各有开始、停止、播放和下载按钮。一次采集一路，左右文件独立保存。录音格式固定为 48kHz、16bit、单声道 PCM/WAV。播放按钮在浏览器播放录音；扬声器按钮播放板端测试音。

## 编译和完整打包

在 WSL 中，从根项目执行：

```sh
bash work_card_SDK_V1/build_mic_web_demo.sh
```

脚本先构建根项目音频库，再构建 SDK demo，重建完整 package 并生成：

`work_card_SDK_V1/build/mic_web_demo/mic_web_demo.tar.gz`

包内包含 bin、lib、configs、web、audio、run_mic_web_demo.sh。部署时停止旧 demo，完整替换专用运行目录；不要增量复制单个库。不得改动 `/etc/init.d/S90autorun.bak`。进入解压目录运行 `sh run_mic_web_demo.sh`。

## 录音接口

- `GET /api/long_record/start?input=left`：开始左麦；right 为右麦。
- `GET /api/long_record/stop?input=left`：停止指定麦克风并保存。
- `GET /api/long_record/download?input=left`：获取该麦克风上一次成功保存的 WAV，用于播放/下载。
- `GET /api/status`：查询 recording、input、left_saved、right_saved。

重复开始、停止非活动麦克风及录音期间请求其他采集操作会返回 409。下载不存在的录音返回 404。输入模式设置或读回失败时开始录音返回错误，不会返回成功。

录音写入 `/tmp/mic_left.pending.pcm` 或 right 对应文件；停止成功后原子替换该路保存文件。左右录音互不覆盖；重录失败保留前一次成功文件。文件位于 /tmp，受板端存储容量限制，不承诺重启保留。网页刷新可恢复当前进程的录音状态。下载采用分块传输，不整段加载到服务器内存。

## SDK 调用链

`configure_audio` → `ehal_audio_configure` → `ehal_audio_set_input_channel` → `ehal_audio_start`。

新增 API 不改变 ehal_audio_config_t 结构布局。只允许在启动前选择左右；当前限定 AI device 0、单声道。SDK 调用厂商 `ss_mpi_ai_set_track_mode`，左路为 BOTH_LEFT，右路为 BOTH_RIGHT，把选中的物理输入映射到既有 AI0/CH0 → AENC0 链路。随后 `ss_mpi_ai_get_track_mode` 读回核对；启动期间回调被屏蔽，切换后丢弃 300ms 启动数据再放行。

此实现使用厂商声道映射 API，不修改预编译 libhal.so；普通录音不启动扬声器功放。

## 验证范围

已完成 WSL ARM 交叉编译、宿主严格编译（-Wall -Wextra -Werror）和模拟 SDK 的 HTTP 集成测试：左右选择传递、冲突拒绝、开始/停止、独立保存、WAV 头及载荷、状态恢复。模拟测试不等同于硬件拾音验证。

板端验收：启动完整包，分别录制左右麦；检查日志的 MIC1/left track=1 与 MIC2/right track=2 (verified)，分别在两只麦附近发声，对比录音并验证下载/浏览器播放。2026-10-08 已在 192.168.137.245 完成左右开始、停止、下载验证；均返回 HTTP 200，WAV 为 48kHz/16bit/单声道且有非零数据。实体麦对应关系及语音清晰度仍需现场发声验收。
## 2026-10-08 板端故障检查

旧完整包启动录音返回 HTTP 500。日志确认配置 volume=100 被 codec 驱动拒绝（errno=22），随后 configure_acodec_gain 再次打开 /dev/acodec 失败并终止启动。重新打包现有 SDK 的 codec 句柄复用实现及当前 volume=0 初始化配置，启动后按 SDK 映射设置输入增益。普通左右录音仅启用采集，帧长配置与 48kHz/20ms 保持一致；stdout 按行刷新，错误及时写入日志。

本次完整包板端实测：左 PCM 297600 字节（3.10 秒），右 PCM 314880 字节（3.28 秒）；左右声道分别读回 track=1、track=2，开始/停止/下载全部成功。运行目录 /root/mic_web_demo，日志 /tmp/mic_web_diagnosis_new.log。
