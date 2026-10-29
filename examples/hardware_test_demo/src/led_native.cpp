#include "led_native.h"

#include <fstream>
#include <sstream>
#include <utility>

namespace hardware_test {

NativeLed::NativeLed(std::string brightness_path)
    : brightness_path_(std::move(brightness_path)) {}

bool NativeLed::set(bool enabled, std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream file(brightness_path_);
    if (!file.is_open()) {
        if (error_message != nullptr) *error_message = "open LED brightness failed: " + brightness_path_;
        known_ = false;
        return false;
    }
    file << (enabled ? '1' : '0') << '\n';
    if (!file.good()) {
        if (error_message != nullptr) *error_message = "write LED brightness failed: " + brightness_path_;
        known_ = false;
        return false;
    }
    enabled_ = enabled;
    known_ = true;
    return true;
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
    std::ifstream file(brightness_path_);
    int value = -1;
    if (file.is_open()) file >> value;
    const bool available = value >= 0;
    return "{\"ok\":true,\"available\":" + std::string(available ? "true" : "false") +
           ",\"path\":\"" + json_escape(brightness_path_) + "\",\"enabled\":" +
           (available ? (value != 0 ? "true" : "false") : (known_ && enabled_ ? "true" : "false")) +
           ",\"value\":" + std::to_string(value) + "}";
}

} // namespace hardware_test
