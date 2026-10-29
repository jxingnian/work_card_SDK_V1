#include "bluetooth_native.h"

#include "ehal_bluetooth.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <vector>

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

/* 将十六进制文本严格转换为二进制，避免把非法字符传给 GATT 写接口。 */
bool parse_hex(const std::string &text, std::vector<uint8_t> *data)
{
    data->clear();
    if (text.empty() || (text.size() % 2U) != 0U) return false;
    auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    for (size_t index = 0U; index < text.size(); index += 2U) {
        const int high = hex_value(text[index]);
        const int low = hex_value(text[index + 1U]);
        if (high < 0 || low < 0) return false;
        data->push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return !data->empty();
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

/* 读取 SDK 最近一次文本输出，供 GATT 发现、读取和通知结果复核。 */
bool NativeBluetooth::gatt_discover(const std::string &address, std::string *output,
                                    std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const int result = bluetooth_ != nullptr ?
        ehal_bt_gatt_discover_services(bluetooth_, address.c_str()) : -1;
    if (!check_result(result, "gatt discover", error_message)) return false;
    char buffer[8192]{};
    (void)ehal_bt_get_last_output(bluetooth_, buffer, sizeof(buffer));
    if (output != nullptr) *output = buffer;
    return true;
}

bool NativeBluetooth::gatt_read(const std::string &address,
                                const std::string &characteristic,
                                std::string *output, std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const int result = bluetooth_ != nullptr ? ehal_bt_gatt_read(
        bluetooth_, address.c_str(), characteristic.c_str()) : -1;
    if (!check_result(result, "gatt read", error_message)) return false;
    char buffer[8192]{};
    (void)ehal_bt_get_last_output(bluetooth_, buffer, sizeof(buffer));
    if (output != nullptr) *output = buffer;
    return true;
}

bool NativeBluetooth::gatt_write(const std::string &address,
                                 const std::string &characteristic,
                                 const std::string &data_hex, std::string *output,
                                 std::string *error_message)
{
    std::vector<uint8_t> data;
    if (!parse_hex(data_hex, &data)) {
        if (error_message != nullptr) *error_message = "data_hex must contain an even number of hexadecimal characters";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const int result = bluetooth_ != nullptr ? ehal_bt_gatt_write(
        bluetooth_, address.c_str(), characteristic.c_str(), data.data(),
        static_cast<uint32_t>(data.size())) : -1;
    if (!check_result(result, "gatt write", error_message)) return false;
    char buffer[8192]{};
    (void)ehal_bt_get_last_output(bluetooth_, buffer, sizeof(buffer));
    if (output != nullptr) *output = buffer;
    return true;
}

bool NativeBluetooth::gatt_notify(const std::string &address,
                                  const std::string &characteristic, bool enabled,
                                  std::string *output, std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const int result = bluetooth_ != nullptr ? (enabled ? ehal_bt_gatt_start_notify(
        bluetooth_, address.c_str(), characteristic.c_str()) : ehal_bt_gatt_stop_notify(
        bluetooth_, address.c_str(), characteristic.c_str())) : -1;
    if (!check_result(result, enabled ? "gatt start notify" : "gatt stop notify", error_message)) return false;
    char buffer[8192]{};
    (void)ehal_bt_get_last_output(bluetooth_, buffer, sizeof(buffer));
    if (output != nullptr) *output = buffer;
    return true;
}

bool NativeBluetooth::a2dp_info(const std::string &address, std::string *output,
                                std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ehal_bt_audio_info_t info{};
    const int result = bluetooth_ != nullptr ? ehal_bt_a2dp_get_info(
        bluetooth_, address.c_str(), &info) : -1;
    if (!check_result(result, "a2dp info", error_message)) return false;
    std::ostringstream json;
    json << "{\"codec\":\"" << json_escape(info.codec)
         << "\",\"pcm_path\":\"" << json_escape(info.pcm_path)
         << "\",\"sample_rate\":" << info.sample_rate
         << ",\"channels\":" << info.channels
         << ",\"bit_width\":" << info.bit_width
         << ",\"volume\":" << info.volume
         << ",\"muted\":" << json_bool(info.muted) << '}';
    if (output != nullptr) *output = json.str();
    return true;
}

bool NativeBluetooth::a2dp_volume(const std::string &address, int volume,
                                  std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return check_result(bluetooth_ != nullptr ? ehal_bt_a2dp_set_volume(
        bluetooth_, address.c_str(), volume) : -1, "a2dp volume", error_message);
}

bool NativeBluetooth::a2dp_mute(const std::string &address, bool muted,
                                std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return check_result(bluetooth_ != nullptr ? ehal_bt_a2dp_set_mute(
        bluetooth_, address.c_str(), muted) : -1, "a2dp mute", error_message);
}

bool NativeBluetooth::a2dp_play(const std::string &address, const std::string &path,
                                std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return check_result(bluetooth_ != nullptr ? ehal_bt_a2dp_play_file(
        bluetooth_, address.c_str(), path.c_str()) : -1, "a2dp play", error_message);
}

bool NativeBluetooth::a2dp_stop(const std::string &address, std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return check_result(bluetooth_ != nullptr ? ehal_bt_a2dp_stop(
        bluetooth_, address.c_str()) : -1, "a2dp stop", error_message);
}

bool NativeBluetooth::hfp_action(const std::string &address, const std::string &action,
                                 const std::string &value, std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (bluetooth_ == nullptr) {
        if (error_message != nullptr) *error_message = "bluetooth is not running";
        return false;
    }
    int result = -1;
    if (action == "open_sco") result = ehal_bt_hfp_open_sco(bluetooth_, address.c_str());
    else if (action == "close_sco") result = ehal_bt_hfp_close_sco(bluetooth_, address.c_str());
    else if (action == "send_at") result = ehal_bt_hfp_send_at(bluetooth_, address.c_str(), value.c_str());
    else if (action == "dial") result = ehal_bt_call_dial(bluetooth_, address.c_str(), value.c_str());
    else if (action == "accept") result = ehal_bt_call_accept(bluetooth_, address.c_str());
    else if (action == "reject") result = ehal_bt_call_reject(bluetooth_, address.c_str());
    else if (action == "hangup") result = ehal_bt_call_hangup(bluetooth_, address.c_str());
    else if (action == "hold") result = ehal_bt_call_hold(bluetooth_, address.c_str());
    else if (action == "mute") result = ehal_bt_call_mute(bluetooth_, address.c_str(), value == "true");
    else {
        if (error_message != nullptr) *error_message = "unsupported hfp action";
        return false;
    }
    return check_result(result, action.c_str(), error_message);
}

bool NativeBluetooth::avrcp_action(const std::string &address, const std::string &action,
                                   std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (bluetooth_ == nullptr) {
        if (error_message != nullptr) *error_message = "bluetooth is not running";
        return false;
    }
    int result = -1;
    if (action == "play") result = ehal_bt_avrcp_play(bluetooth_, address.c_str());
    else if (action == "pause") result = ehal_bt_avrcp_pause(bluetooth_, address.c_str());
    else if (action == "stop") result = ehal_bt_avrcp_stop(bluetooth_, address.c_str());
    else if (action == "next") result = ehal_bt_avrcp_next(bluetooth_, address.c_str());
    else if (action == "previous") result = ehal_bt_avrcp_previous(bluetooth_, address.c_str());
    else {
        if (error_message != nullptr) *error_message = "unsupported avrcp action";
        return false;
    }
    return check_result(result, action.c_str(), error_message);
}

} // namespace hardware_test
