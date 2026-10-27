# Hardware Test Demo

Demo 使用侧边栏组织硬件模块：第一栏是 WiFi 与网络，第二栏是音频。WiFi 进程不调用 `ehal_wifi`、`ehal_audio`、`ehal_camera` 或 `libhal.so`；音频第二栏启动交付包内独立的原生 Audio MPP Demo。

WiFi 操作直接执行板端 `wpa_cli`、`iw` 和 `ip`。当前开发板的 `wpa_supplicant` 控制 socket 位于 `/etc/wireless/wpa_supplicant/wlan0`，运行脚本默认使用 `/etc/wireless/wpa_supplicant`。扫描只调用当前运行中的 `wpa_supplicant` 控制接口，代码不执行 `terminate`、`killall`、`ifconfig down`、`disconnect`、`select_network` 或 `reassociate`，因此扫描流程不会主动切断当前网络。

## 构建

在 WSL 中从本目录执行：

```sh
bash build/hardware_test_demo/build_hardware_test_demo.sh
bash build/hardware_test_demo/package_hardware_test_demo.sh
```

完整产物位于：

```text
build/hardware_test_demo/hardware_test_demo.tar.gz
```

## 部署

部署前确认板端 SSH 地址仍然是当前实际地址：

```sh
BOARD_IP=192.168.137.167 bash build/hardware_test_demo/deploy_hardware_test_demo.sh
```

脚本会清理并重新部署 `/opt/hardware_test_demo`，不会修改 `/etc/init.d/S90autorun.bak` 和 `/root/insmod.sh`。

## 运行

```sh
cd /opt/hardware_test_demo
HARDWARE_TEST_DEMO_PORT=8090 sh run_hardware_test_demo.sh
```

访问：

```text
http://192.168.137.167:8090/
```

连接网络是明确的切换操作；扫描、读取当前状态、读取保存网络和删除非当前网络不执行连接切换。

音频页面运行在同一板端的 8092 端口，直接使用厂商 `ss_mpi_audio_*`、AI/AO、`/dev/acodec` 接口，提供麦克风/扬声器状态、录音下载、录音播放、文字语音测试音频、扬声器音量和麦克风增益调节。
