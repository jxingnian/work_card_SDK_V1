#ifndef HARDWARE_TEST_BLUETOOTH_NATIVE_H
#define HARDWARE_TEST_BLUETOOTH_NATIVE_H

#include <mutex>
#include <string>

#include "ehal_bluetooth.h"

namespace hardware_test {

/* 蓝牙设备模块的统一封装，所有硬件操作都通过 ehal_bluetooth SDK 完成。 */
class NativeBluetooth {
public:
    NativeBluetooth();
    ~NativeBluetooth();

    /* 初始化并启动蓝牙适配器服务。 */
    bool start(std::string *error_message);

    /* 停止蓝牙服务并释放 SDK 对象。 */
    void stop();

    /* 返回适配器状态、版本和扫描状态。 */
    std::string status_json();

    /* 返回当前 SDK 发现到的设备列表。 */
    std::string devices_json();

    /* 开始或停止设备扫描。 */
    bool start_scan(std::string *error_message);
    bool stop_scan(std::string *error_message);

    /* 执行基础设备管理操作。 */
    bool pair(const std::string &address, std::string *error_message);
    bool cancel_pair(const std::string &address, std::string *error_message);
    bool remove(const std::string &address, std::string *error_message);
    bool trust(const std::string &address, bool enabled, std::string *error_message);
    bool block(const std::string &address, bool enabled, std::string *error_message);
    bool connect(const std::string &address, std::string *error_message);
    bool disconnect(const std::string &address, std::string *error_message);

private:
    /* 将 SDK 返回码转换为可直接显示给操作者的错误文本。 */
    bool check_result(int result, const char *operation, std::string *error_message);

    std::mutex mutex_;
    ehal_bluetooth_t *bluetooth_ = nullptr;
    bool started_ = false;
};

} // namespace hardware_test

#endif
