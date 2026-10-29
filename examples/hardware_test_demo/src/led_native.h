#ifndef HARDWARE_TEST_LED_NATIVE_H
#define HARDWARE_TEST_LED_NATIVE_H

#include <mutex>
#include <string>

namespace hardware_test {

/*
 * LED 测试模块使用 Linux LED class 的 brightness 节点。
 * 具体板卡 LED 名称由启动参数提供，避免把未经硬件原理图确认的 GPIO 编号写死。
 */
class NativeLed {
public:
    explicit NativeLed(std::string brightness_path);

    /* 写入 0/1 控制 LED，并返回可核对的错误信息。 */
    bool set(bool enabled, std::string *error_message);

    /* 读取当前 brightness，验证写入是否已经落到内核节点。 */
    std::string status_json() const;

private:
    static std::string json_escape(const std::string &value);
    std::string brightness_path_;
    mutable std::mutex mutex_;
    bool enabled_ = false;
    bool known_ = false;
};

} // namespace hardware_test

#endif
