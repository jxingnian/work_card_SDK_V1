# WiFi API 使用说明

WiFi SDK 通过 `include/ehal_wifi.h` 和 `lib/libehal_wifi.so` 提供。第一版支持 STA 模式，客户应用可完成扫描热点、连接热点、断开连接、查询状态和读取 IP 信息。

## 接口清单

| 模块 | 接口 | 作用 |
| ---- | ---- | ---- |
| 版本与错误 | `ehal_wifi_version` | 获取 WiFi SDK 版本 |
| 版本与错误 | `ehal_wifi_error_string` | 将错误码转换为错误说明 |
| 生命周期 | `ehal_wifi_create` | 创建 WiFi 实例 |
| 生命周期 | `ehal_wifi_configure` | 配置网卡名、wpa 配置路径、连接超时、DHCP 超时 |
| 生命周期 | `ehal_wifi_destroy` | 释放 WiFi 实例 |
| 热点扫描 | `ehal_wifi_scan` | 扫描周围热点，返回 SSID、BSSID、信号、频率、加密方式 |
| 连接控制 | `ehal_wifi_connect` | 连接指定 SSID，连接成功后执行 DHCP 获取 IP |
| 连接控制 | `ehal_wifi_disconnect` | 断开当前 WiFi |
| 状态查询 | `ehal_wifi_get_status` | 查询连接状态、SSID、BSSID、信号、IP |
| 状态查询 | `ehal_wifi_get_ip_info` | 查询 IP、子网掩码、网关、DNS |

## 示例程序

```bash
./wifi_demo scan
./wifi_demo connect <ssid> <password>
./wifi_demo status
./wifi_demo disconnect
```

## 板端运行依赖

板端系统需要提供以下命令：

```text
iwlist
iwconfig
ifconfig
route
wpa_supplicant
wpa_cli
udhcpc
```

## 第一版范围

第一版只做 STA 联网能力，不包含 AP 热点模式、静态 IP、多热点配置管理和自动重连策略接口。
