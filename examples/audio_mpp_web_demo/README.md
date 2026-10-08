# Audio MPP Web Demo

这是一个全新的音频网页 demo，不复用 `mic_web_demo`，也不调用 `ehal_audio` 封装接口。

## 接口范围

后端直接调用厂商接口：

- `ss_mpi_audio_*`
- `ss_mpi_ai_*`
- `ss_mpi_ao_*`
- `ss_mpi_aenc_*`
- `ss_mpi_adec_*`
- `ss_mpi_ai_*_vqe*`
- `ss_mpi_ao_*_vqe*`
- `/dev/acodec` + `ot_acodec.h` ioctl

普通录放 demo 固定参数：

```text
sample_rate = 16000
bit_width = 16
channels = 1
point_num_per_frame = 320
```

网页高级页仍然暴露厂商枚举和参数，用于配置、执行和读回验证。

## 构建

构建脚本放在当前 demo 对应的 build 目录中。请在 WSL 中从项目根目录执行：

```sh
bash work_card_SDK_V1/build/audio_mpp_web_demo/build_audio_mpp_web_demo.sh
```

输出完整包：

```text
work_card_SDK_V1/build/audio_mpp_web_demo/audio_mpp_web_demo.tar.gz
```

## 板端运行

解压完整包后执行：

```sh
sh run_audio_mpp_web_demo.sh
```

默认网页端口：

```text
http://<开发板IP>:8092/
```

可通过环境变量改端口：

```sh
AUDIO_MPP_WEB_PORT=8093 sh run_audio_mpp_web_demo.sh
```

## 页面功能

- 基础状态：音频模块初始化、退出、模块时钟参数。
- AI 麦克风：AI 公共属性、通道属性、通道缓存、track mode、数字麦增益。
- AO 扬声器：AO 公共属性、通道控制、音量、静音、track mode、缓存状态。
- AENC/ADEC：编码/解码通道创建、销毁、缓存和状态接口。
- VQE：AI record/talk/talk_v2、AO VQE 的核心公共参数。
- ACODEC：通过 `/dev/acodec` 执行 codec ioctl。
- 录放 Demo：左麦录音、右麦录音、单声道下载、双声道下载、指定频率播放、内置语音 WAV 播放。

内置扬声器文件：

```text
audio/speaker_voice_16k.wav
```

该文件是 16kHz / 16bit / mono 语音 WAV，内容为“音频测试，扬声器播放正常。”

所有设置类接口都会在返回结果区域显示 HTTP 状态和厂商接口返回信息。硬件读回请使用对应模块页的 `get_*` 按钮。
