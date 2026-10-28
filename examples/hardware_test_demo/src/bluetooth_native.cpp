#include "bluetooth_native.h"

#include "ehal_bluetooth.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <sstream>

namespace hardware_test {
namespace {

/* 对蓝牙名称和 SDK 错误文本进行 JSON 转义，保证接口始终返回合法 JSON。 */
std::string json_escape(const char *value)
{
    std::ostringstream output;
    if (value == nullptr) return output.str();
    for (const unsigned char ch : std::string(value)) {
        switch (ch) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (ch < 0x20U) output << ' ';
            else output << static_cast<char>(ch);
            break;
        }
    }
    return output.str();
}

/* 将布尔值统一输出为 JSON 布尔字面量。 */
const char *json_bool(int value)
{
    return value != 0 ? "true" : "false";
}

/* 把设备类型枚举转换为页面可读的稳定文本。 */
const char *state_name(ehal_bt_state_t state)
{
    switch (state) {
    case EHAL_BT_STATE_ON: return "on";
    case EHAL_BT_STATE_DISCOVERING: return "discovering";
    default: return "off";
    }
}

} // namespace

NativeBluetooth::NativeBluetooth() = default;

NativeBluetooth::~NativeBluetooth()
{
    stop();
}

bool NativeBluetooth::check_result(int result, const char *operation,
                                   std::string *error_message)
{
    if (result == EHAL_OK) return true;
    const char *detail = ehal_bt_error_string(result);
    if (error_message != nullptr) {
        std::ostringstream output;
        output << operation << " failed, code=" << result << ": "
               << (detail != nullptr ? detail : "unknown bluetooth error");
        *error_message = output.str();
    }
    return false;
}

bool NativeBluetooth::start(std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) return true;

    int result = ehal_bt_create(&bluetooth_);
    if (!check_result(result, "ehal_bt_create", error_message)) return false;

    ehal_bt_config_t config{};
    config.adapter = "hci0";
    config.command_timeout_ms = 5000U;
    config.scan_timeout_ms = 10000U;
    result = ehal_bt_configure(bluetooth_, &config);
    if (result == EHAL_OK) result = ehal_bt_start(bluetooth_);
    if (!check_result(result, "bluetooth start", error_message)) {
        ehal_bt_destroy(bluetooth_);
        bluetooth_ = nullptr;
        return false;
    }
    started_ = true;
    return true;
}

void NativeBluetooth::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (bluetooth_ != nullptr) {
        if (started_) (void)ehal_bt_stop(bluetooth_);
        ehal_bt_destroy(bluetooth_);
        bluetooth_ = nullptr;
    }
    started_ = false;
}

std::string NativeBluetooth::status_json()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ehal_bt_adapter_info_t adapter{};
    int scanning = 0;
    int result = bluetooth_ != nullptr ? ehal_bt_get_adapter_info(bluetooth_, &adapter) : -1;
    if (result == EHAL_OK) result = ehal_bt_is_scanning(bluetooth_, &scanning);
    std::ostringstream output;
    output << "{\"ok\":" << json_bool(result == EHAL_OK)
           << ",\"running\":" << json_bool(started_)
           << ",\"version\":\"" << json_escape(ehal_bt_version()) << "\"";
    if (result != EHAL_OK) {
        output << ",\"error_code\":" << result << "}";
        return output.str();
    }
    const ehal_bt_state_t state = scanning != 0 ? EHAL_BT_STATE_DISCOVERING :
        (adapter.powered != 0 ? EHAL_BT_STATE_ON : EHAL_BT_STATE_OFF);
    output << ",\"state\":\"" << state_name(state)
           << "\",\"address\":\"" << json_escape(adapter.address)
           << "\",\"name\":\"" << json_escape(adapter.name)
           << "\",\"alias\":\"" << json_escape(adapter.alias)
           << "\",\"manufacturer\":\"" << json_escape(adapter.manufacturer)
           << "\",\"powered\":" << json_bool(adapter.powered)
           << ",\"pairable\":" << json_bool(adapter.pairable)
           << ",\"discoverable\":" << json_bool(adapter.discoverable)
           << ",\"scanning\":" << json_bool(scanning) << '}';
    return output.str();
}

std::string NativeBluetooth::devices_json()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::array<ehal_bt_device_info_t, 128> devices{};
    uint32_t actual_count = 0U;
    const int result = bluetooth_ != nullptr ?
        ehal_bt_get_devices(bluetooth_, devices.data(), devices.size(), &actual_count) : -1;
    std::ostringstream output;
    output << "{\"ok\":" << json_bool(result == EHAL_OK);
    if (result != EHAL_OK) {
        output << ",\"error_code\":" << result << ",\"devices\":[]}";
        return output.str();
    }
    output << ",\"devices\":[";
    for (uint32_t index = 0U; index < actual_count; ++index) {
        if (index != 0U) output << ',';
        const auto &device = devices[index];
        output << "{\"address\":\"" << json_escape(device.address)
               << "\",\"name\":\"" << json_escape(device.name)
               << "\",\"alias\":\"" << json_escape(device.alias)
               << "\",\"rssi\":" << device.rssi
               << ",\"paired\":" << json_bool(device.paired)
               << ",\"trusted\":" << json_bool(device.trusted)
               << ",\"connected\":" << json_bool(device.connected)
               << ",\"blocked\":" << json_bool(device.blocked)
               << ",\"classic\":" << json_bool(device.classic)
               << ",\"le\":" << json_bool(device.le) << '}';
    }
    output << "]}";
    return output.str();
}

bool NativeBluetooth::start_scan(std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return check_result(bluetooth_ != nullptr ? ehal_bt_start_scan(bluetooth_) : -1,
                        "start scan", error_message);
}

bool NativeBluetooth::stop_scan(std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return check_result(bluetooth_ != nullptr ? ehal_bt_stop_scan(bluetooth_) : -1,
                        "stop scan", error_message);
}

bool NativeBluetooth::pair(const std::string &address, std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return check_result(bluetooth_ != nullptr ? ehal_bt_pair(bluetooth_, address.c_str()) : -1,
                        "pair", error_message);
}

bool NativeBluetooth::cancel_pair(const std::string &address, std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return check_result(bluetooth_ != nullptr ? ehal_bt_cancel_pair(bluetooth_, address.c_str()) : -1,
                        "cancel pair", error_message);
}

bool NativeBluetooth::remove(const std::string &address, std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return check_result(bluetooth_ != nullptr ? ehal_bt_remove_device(bluetooth_, address.c_str()) : -1,
                        "remove device", error_message);
}

bool NativeBluetooth::trust(const std::string &address, bool enabled,
                            std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return check_result(bluetooth_ != nullptr ? ehal_bt_set_trusted(bluetooth_, address.c_str(), enabled) : -1,
                        enabled ? "trust" : "untrust", error_message);
}

bool NativeBluetooth::block(const std::string &address, bool enabled,
                            std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return check_result(bluetooth_ != nullptr ? ehal_bt_set_blocked(bluetooth_, address.c_str(), enabled) : -1,
                        enabled ? "block" : "unblock", error_message);
}

bool NativeBluetooth::connect(const std::string &address, std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return check_result(bluetooth_ != nullptr ? ehal_bt_connect(bluetooth_, address.c_str()) : -1,
                        "connect", error_message);
}

bool NativeBluetooth::disconnect(const std::string &address, std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return check_result(bluetooth_ != nullptr ? ehal_bt_disconnect(bluetooth_, address.c_str()) : -1,
                        "disconnect", error_message);
}

} // namespace hardware_test
