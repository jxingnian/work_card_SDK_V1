#include "key_native.h"

#include <sstream>
#include <utility>

namespace hardware_test {

NativeKey::NativeKey(std::string device_name) : device_name_(std::move(device_name)) {}

NativeKey::~NativeKey()
{
    stop();
}

bool NativeKey::start(std::string *error_message)
{
    if (started_) return true;
    int result = ehal_key_create(&key_);
    if (result == EHAL_OK) {
        ehal_key_config_t config{};
        config.device_name = device_name_.c_str();
        config.long_press_ms = 1500U;
        result = ehal_key_configure(key_, &config);
    }
    if (result == EHAL_OK) {
        result = ehal_key_set_callback(key_, &NativeKey::key_callback, this);
    }
    if (result == EHAL_OK) result = ehal_key_start(key_);
    if (result != EHAL_OK) {
        if (error_message != nullptr) {
            *error_message = std::string(ehal_key_error_string(result)) +
                             " (code=" + std::to_string(result) +
                             ", device=" + device_name_ + ")";
        }
        ehal_key_destroy(key_);
        key_ = nullptr;
        return false;
    }
    started_ = true;
    return true;
}

void NativeKey::stop()
{
    if (key_ != nullptr) {
        ehal_key_destroy(key_);
        key_ = nullptr;
    }
    started_ = false;
}

void NativeKey::key_callback(const ehal_key_event_t *event, void *user_data)
{
    if (event != nullptr && user_data != nullptr) {
        static_cast<NativeKey *>(user_data)->on_key(event);
    }
}

void NativeKey::on_key(const ehal_key_event_t *event)
{
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(*event);
    while (events_.size() > 32U) events_.pop_front();
}

const char *NativeKey::key_name(ehal_key_code_t key)
{
    switch (key) {
    case EHAL_KEY_FUNCTION: return "function";
    case EHAL_KEY_VOLUME_UP: return "volume_up";
    case EHAL_KEY_POWER: return "power";
    case EHAL_KEY_VOLUME_DOWN: return "volume_down";
    default: return "unknown";
    }
}

const char *NativeKey::event_name(ehal_key_event_type_t type)
{
    switch (type) {
    case EHAL_KEY_EVENT_PRESS: return "press";
    case EHAL_KEY_EVENT_RELEASE: return "release";
    case EHAL_KEY_EVENT_LONG_PRESS: return "long_press";
    default: return "unknown";
    }
}

std::string NativeKey::json_escape(const std::string &value)
{
    std::ostringstream result;
    for (const unsigned char ch : value) {
        if (ch == '"' || ch == '\\') result << '\\' << ch;
        else result << static_cast<char>(ch);
    }
    return result.str();
}

std::string NativeKey::status_json() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream output;
    output << "{\"ok\":true,\"started\":" << (started_ ? "true" : "false")
           << ",\"device\":\"" << json_escape(device_name_) << "\",\"keys\":[";
    for (int index = 0; index < EHAL_KEY_COUNT; ++index) {
        ehal_key_state_t state{};
        const int result = key_ == nullptr ? EHAL_ERR_STATE :
            ehal_key_get_state(key_, static_cast<ehal_key_code_t>(index), &state);
        if (index != 0) output << ',';
        output << "{\"name\":\"" << key_name(static_cast<ehal_key_code_t>(index))
               << "\",\"available\":" << (result == EHAL_OK ? "true" : "false")
               << ",\"pressed\":" << (state.pressed != 0 ? "true" : "false")
               << ",\"press_count\":" << state.press_count
               << ",\"release_count\":" << state.release_count
               << ",\"long_press_count\":" << state.long_press_count << '}';
    }
    output << "],\"events\":[";
    bool first = true;
    for (const ehal_key_event_t &event : events_) {
        if (!first) output << ',';
        first = false;
        output << "{\"key\":\"" << key_name(event.key)
               << "\",\"type\":\"" << event_name(event.type)
               << "\",\"duration_ms\":" << event.duration_ms
               << ",\"timestamp_ms\":" << event.timestamp_ms << '}';
    }
    output << "]}";
    return output.str();
}

} // namespace hardware_test
