# IMU Web Demo

本 Demo 通过 `libehal_imu.so` 读取 QMI8658C，并在浏览器实时显示三轴加速度、角速度、温度和状态寄存器。Canvas 中的绿色椭圆表示设备姿态，黄色箭头表示重力方向；“启用移动唤醒”按钮写入 QMI8658C WoM 配置并使用原理图已有 INT1。

```sh
./build_imu_web_demo.sh
./build/imu_web_demo/imu_web_demo --port 8081 --web-root examples/imu_web_demo/web
```

浏览器访问 `http://<开发板IP>:8081/`。可用 `IMU_I2C_DEVICE=/dev/i2c-1` 和 `IMU_I2C_ADDR=0x19` 覆盖默认值。初始化会校验 `WHO_AM_I=0x11`。SC7A20H 是三轴加速度计，不提供陀螺仪角速度，网页中的角速度固定显示为 0。
