# GPIO1_5 电池 ADC 采集 demo

## 编译

在项目根目录的 PowerShell 执行：

```powershell
wsl bash work_card_SDK_V1/build_battery_adc_demo.sh
```

所有构建产物位于 `work_card_SDK_V1/build/battery_adc_demo/`，其中
`battery_adc_demo.tar.gz` 包含可执行文件、启动脚本和说明。
只调用厂商 LSADC ioctl，不依赖摄像头、音频或 EHAL 多媒体库。

## 板端运行

将完整压缩包解压到独立目录，例如 `/root/battery_adc_demo`，以 root 执行：

```sh
cd /root/battery_adc_demo
./run_battery_adc_demo.sh 20
```

第一个参数为采样次数，默认 10 次，每 200ms 输出一次；第二个参数为参考电压
（mV），默认 3300，例如 `./run_battery_adc_demo.sh 20 3300`。
脚本在设备节点不存在时加载板上 `/komod/ot_adc.ko`。
不修改系统启动配置。运行期间独占 ADC，勿同时运行其他 ADC 程序或配置 GPIO1_5。

程序保存 `0x11130000` 的原配置，设置功能位 `[3:0]=4`、关闭下拉位 `[9]`，
随后使用 `/dev/ot_lsadc` 连续扫描 CH1。正常结束或 Ctrl-C/SIGTERM 时停止采样、
关闭通道、恢复原引脚配置；SIGKILL 不执行清理。

## 硬件与换算依据

- 模块规格书 PIN7：`LSADC_CH1/I2C1_SDA/PWM0_OUT2/UART2_RXD/GPIO1_5`。
- Hi3516CV610 管脚配置表：`io1_cfg_reg0=0x11130000`，ADC 功能选择值 4。
- Smart ID 原理图第 2 页：VBAT+V 经 R6=200kΩ、R11=100kΩ 分压得到 BAT_ADC，倍率 3。
  该采样点在电源 MOS 管 Q4 后端，并非直接跨接电池接线端。
- 厂商 `ot_adc/arch/hi3516cv610/ot_adc_hal.h`：10 位 ADC。
- 厂商 `ot_adc/adc.c` 和 `sample/hi3516cv610/sample_adc.c`：
  `LSADC_IOC_GET_CHNVAL` 的返回值即原始码，不是传入的 channel 参数。
- Hi3516CV610 用户指南 12.7.2：LSADC 电源电压 3.3V。

图中 R6=200kΩ 在 VBAT+V 与 BAT_ADC 之间，R11=100kΩ 在 BAT_ADC 与 GND
之间，所以稳态分压关系为：

```text
BAT_ADC = VBAT × R11 / (R6 + R11) = VBAT / 3
VBAT = BAT_ADC × 3
```

C57、C58 是并联滤波电容，只影响采样响应时间和纹波，不改变直流分压比例。
Demo 使用标称线性换算：

```text
ADC引脚电压 = raw × vref_mv / 1023
电池采样点电压 = ADC引脚电压 × 3
```

默认 3300mV 为标称换算参数；未用万用表校准参考电压、分压电阻及 ADC 误差，
输出不是校准后的绝对电压。原始码始终保留供核对，满码 1023 标注 SATURATED。
实测电池采样点电压为 V、平均原始码为 N 时，可按 `vref_mv=V×1000×1023/(N×3)`
计算等效校准参数。

## 2026-10-08 板端验证

板端：192.168.137.172，Linux 5.10.221，ARMv7。

- 原先没有 `/dev/ot_lsadc`，加载板内 `ot_adc.ko` 后节点生成。
- 复用读回：`0x00001200 -> 0x00001004`。
- 10 次实测原始码：394、400、393、400、391、396、398、391、389、400。
- 标称换算范围：3.761～3.867V；平均原始码 395.2，平均电压 3.821V。
- 程序退出码 0。该记录为真实板端采样，未与万用表对照。

后续完整压缩包部署及 SIGTERM 回归因 SSH 连接超时未执行；串口 COM21 打开返回拒绝访问。
上述 10 次数据来自此前已成功运行的同一版可执行程序，不代表信号退出流程已通过板端回归。
