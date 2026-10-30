#include "led_native.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

namespace hardware_test {

NativeLed::NativeLed(std::string brightness_path, std::string color_path)
    : brightness_path_(std::move(brightness_path)),
      color_path_(std::move(color_path)) {}

bool NativeLed::set(bool enabled, std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    /* 熄灭时不丢失用户设置的亮度；再次点亮时恢复该亮度，默认恢复到 255。 */
    const unsigned int target = enabled ? restore_brightness_ : 0U;
    return write_brightness(target, error_message);
}

bool NativeLed::write_brightness(unsigned int value, std::string *error_message)
{
    std::ofstream file(brightness_path_);
    if (!file.is_open()) {
        if (error_message != nullptr) {
            *error_message = "open LED brightness failed: " + brightness_path_ +
                              ": " + std::strerror(errno);
        }
        known_ = false;
        return false;
    }
    file << value << '\n';
    if (!file.good()) {
        if (error_message != nullptr) {
            *error_message = "write LED brightness failed: " + brightness_path_ +
                              ": " + std::strerror(errno);
        }
        known_ = false;
        return false;
    }
    brightness_ = value;
    if (value != 0U) restore_brightness_ = value;
    known_ = true;
    return true;
}

bool NativeLed::write_color(unsigned int red, unsigned int green, unsigned int blue,
                            std::string *error_message)
{
    std::ofstream file(color_path_);
    if (!file.is_open()) {
        if (error_message != nullptr) {
            *error_message = "open LED color failed: " + color_path_ +
                              ": " + std::strerror(errno);
        }
        known_ = false;
        return false;
    }
    file << red << ' ' << green << ' ' << blue << '\n';
    if (!file.good()) {
        if (error_message != nullptr) {
            *error_message = "write LED color failed: " + color_path_ +
                              ": " + std::strerror(errno);
        }
        known_ = false;
        return false;
    }
    red_ = red;
    green_ = green;
    blue_ = blue;
    known_ = true;
    return true;
}

bool NativeLed::set_color(unsigned int red, unsigned int green, unsigned int blue,
                          unsigned int brightness, std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!write_color(red, green, blue, error_message)) return false;
    return write_brightness(brightness, error_message);
}

std::string NativeLed::json_escape(const std::string &value)
{
    std::ostringstream result;
    for (const unsigned char ch : value) {
        if (ch == '"' || ch == '\\') result << '\\' << ch;
        else result << static_cast<char>(ch);
    }
    return result.str();
}

std::string NativeLed::status_json() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream brightness_file(brightness_path_);
    unsigned int brightness = 0;
    const bool brightness_available = brightness_file.is_open() &&
                                      static_cast<bool>(brightness_file >> brightness);
    std::ifstream color_file(color_path_);
    unsigned int red = 0;
    unsigned int green = 0;
    unsigned int blue = 0;
    const bool color_available = color_file.is_open() &&
                                 static_cast<bool>(color_file >> red >> green >> blue);
    const bool available = brightness_available && color_available;
    if (!brightness_available) brightness = brightness_;
    if (!color_available) {
        red = red_;
        green = green_;
        blue = blue_;
    }
    std::ostringstream result;
    result << "{\"ok\":true,\"available\":" << (available ? "true" : "false")
           << ",\"brightness_path\":\"" << json_escape(brightness_path_)
           << "\",\"color_path\":\"" << json_escape(color_path_)
           << "\",\"enabled\":" << (brightness != 0U ? "true" : "false")
           << ",\"brightness\":" << brightness
           << ",\"value\":" << brightness
           << ",\"red\":" << red
           << ",\"green\":" << green
           << ",\"blue\":" << blue << '}';
    return result.str();
}

} // namespace hardware_test
