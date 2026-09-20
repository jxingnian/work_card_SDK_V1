# camera_web_demo 烧录说明

整个 `camera_web_demo_sc2356/` 目录就是本次唯一交付目录，包含程序、网页、配置、字体、运行库、压缩包和本说明。不要再从源码目录零散复制文件。

## 交付与校验

本目录内的 `camera_web_demo_sc2356.tar.gz` 是应用部署包，不是完整 rootfs/Flash 镜像；目标板需要先有可启动的基础固件。

```bash
cd camera_web_demo_sc2356
sha256sum -c camera_web_demo_sc2356.tar.gz.sha256
tar -xzf camera_web_demo_sc2356.tar.gz
```

如果直接复制目录，可跳过解压步骤，整体上传 `camera_web_demo_sc2356/`。

## 上传与首次配置

```bash
scp -r camera_web_demo_sc2356 root@<设备IP>:/opt/
ssh root@<设备IP>
cd /opt/camera_web_demo_sc2356
chmod +x fix_board_sensor_sc2356.sh run_camera_web_demo.sh
./fix_board_sensor_sc2356.sh
reboot
```

脚本会备份 `/etc/init.d/S90autorun`，并将传感器加载项改为 `sc2356`。重启后才会生效。

## 启动与验证

```bash
cd /opt/camera_web_demo_sc2356
./run_camera_web_demo.sh
curl http://127.0.0.1:8080/api/status
```

电脑浏览器访问 `http://<设备IP>:8080/`，首次测试建议选择 H.264。若浏览器不支持局域网 HTTP 下的 WebCodecs，可执行：

```powershell
ssh -L 8080:127.0.0.1:8080 root@<设备IP>
```

然后访问 `http://127.0.0.1:8080/`。

## 常见问题

- 找不到可执行文件：确认当前目录是 `/opt/camera_web_demo_sc2356`，且 `bin/camera_web_demo` 存在。
- 动态库缺失：必须保留整个 `lib/`，并使用 `run_camera_web_demo.sh` 启动。
- 没有视频：确认 `fix_board_sensor_sc2356.sh` 已执行且板端已重启，再检查 `/api/status`。