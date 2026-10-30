#ifndef HARDWARE_TEST_LED_NATIVE_H
#define HARDWARE_TEST_LED_NATIVE_H

#include <mutex>
#include <string>

namespace hardware_test {

/*
 * LED 测试模块使用板端 xn-ws2812 驱动导出的两个 sysfs 节点：
 * brightness 节点控制整体亮度，color 节点控制 RGB 三个颜色分量。
 * 两个路径均由启动参数提供，避免把特定固件路径散落在业务代码中。
 */
class NativeLed {
public:
    NativeLed(std::string brightness_path, std::string color_path);

    /* 写入整体亮度，保留当前 RGB 颜色，供原有开关接口继续使用。 */
    bool set(bool enabled, std::string *error_message);

    /* 写入 RGB 颜色和整体亮度，完成一次完整的变色操作。 */
    bool set_color(unsigned int red, unsigned int green, unsigned int blue,
                   unsigned int brightness, std::string *error_message);

    /* 读取内核节点的真实亮度和 RGB 值，验证写入是否已经落到驱动。 */
    std::string status_json() const;

private:
    static std::string json_escape(const std::string &value);
    bool write_brightness(unsigned int value, std::string *error_message);
    bool write_color(unsigned int red, unsigned int green, unsigned int blue,
                     std::string *error_message);
    std::string brightness_path_;
    std::string color_path_;
    mutable std::mutex mutex_;
    unsigned int brightness_ = 255;
    unsigned int restore_brightness_ = 255;
    unsigned int red_ = 255;
    unsigned int green_ = 255;
    unsigned int blue_ = 255;
    bool known_ = false;
};

} // namespace hardware_test

#endif
