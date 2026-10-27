#ifndef HARDWARE_TEST_WIFI_NATIVE_H
#define HARDWARE_TEST_WIFI_NATIVE_H

#include "native_command.h"

#include <mutex>
#include <string>
#include <vector>

namespace hardware_test {

/*
 * WiFi 原生模块的统一结果。
 * data 字段保存已经经过 JSON 转义前的业务字段片段，error 字段保存原生
 * 命令失败依据。密码不会进入任何返回值或日志。
 */
struct WifiResult {
    bool ok = false;
    int http_status = 500;
    std::string code;
    std::string message;
    std::string data_json = "{}";
    int native_exit_code = -1;
    std::string native_output;
};

/*
 * 直接使用 wpa_cli、iw 和 ip 管理 wlan0，不调用 ehal_wifi 封装库。
 * 所有会改变 WiFi 状态的操作都由同一把互斥锁串行化。
 */
class NativeWifi {
public:
    NativeWifi(std::string interface_name, std::string control_path);

    WifiResult status();
    WifiResult saved_networks();
    WifiResult scan();
    WifiResult add_network(const std::string &ssid,
                           const std::string &password,
                           const std::string &auth,
                           bool connect_now);
    WifiResult delete_network(const std::string &network_id);
    WifiResult connect_network(const std::string &network_id);
    WifiResult disconnect();

private:
    WifiResult run_wpa(const std::vector<std::string> &arguments,
                       int timeout_ms = 5000);
    WifiResult command_failure(const std::string &code,
                              const std::string &message,
                              const CommandResult &command) const;

    std::string interface_name_;
    std::string control_path_;
    std::mutex operation_mutex_;
};

} // namespace hardware_test

#endif
