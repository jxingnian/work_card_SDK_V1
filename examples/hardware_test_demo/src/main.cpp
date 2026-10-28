#include "http_server.h"
#include "camera_native.h"
#include "audio_native.h"
#include "wifi_native.h"
#include "bluetooth_native.h"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace hardware_test {
namespace {

/* 对网页返回的所有文本进行 JSON 转义，避免原生输出破坏响应格式。 */
std::string json_escape(const std::string &value)
{
    std::ostringstream output;
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (ch < 0x20U) {
                output << "\\u00";
                const char *hex = "0123456789abcdef";
                output << hex[(ch >> 4U) & 0x0fU] << hex[ch & 0x0fU];
            } else {
                output << static_cast<char>(ch);
            }
            break;
        }
    }
    return output.str();
}

/* 从简单 JSON 对象中读取字符串字段；本 Demo 不接受嵌套对象。 */
bool json_string_field(const std::string &json,
                       const std::string &key,
                       std::string *value)
{
    const std::string marker = "\"" + key + "\"";
    const size_t marker_pos = json.find(marker);
    if (marker_pos == std::string::npos) {
        return false;
    }
    const size_t colon = json.find(':', marker_pos + marker.size());
    if (colon == std::string::npos) {
        return false;
    }
    size_t cursor = colon + 1U;
    while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t')) {
        ++cursor;
    }
    if (cursor >= json.size() || json[cursor] != '"') {
        return false;
    }
    ++cursor;
    std::ostringstream parsed;
    while (cursor < json.size()) {
        char ch = json[cursor++];
        if (ch == '"') {
            *value = parsed.str();
            return true;
        }
        if (ch != '\\') {
            parsed << ch;
            continue;
        }
        if (cursor >= json.size()) {
            return false;
        }
        const char escaped = json[cursor++];
        switch (escaped) {
        case '"': parsed << '"'; break;
        case '\\': parsed << '\\'; break;
        case '/': parsed << '/'; break;
        case 'n': parsed << '\n'; break;
        case 'r': parsed << '\r'; break;
        case 't': parsed << '\t'; break;
        default: return false;
        }
    }
    return false;
}

/* 从简单 JSON 对象中读取布尔字段；缺失字段由调用方提供默认值。 */
bool json_bool_field(const std::string &json,
                    const std::string &key,
                    bool default_value)
{
    const std::string marker = "\"" + key + "\"";
    const size_t marker_pos = json.find(marker);
    if (marker_pos == std::string::npos) {
        return default_value;
    }
    const size_t colon = json.find(':', marker_pos + marker.size());
    if (colon == std::string::npos) {
        return default_value;
    }
    const std::string tail = json.substr(colon + 1U);
    return tail.find("true") < tail.find("false") ? true :
           (tail.find("false") < tail.find("true") ? false : default_value);
}

/* 读取简单 JSON 对象中的无符号整数，并拒绝负数和溢出值。 */
    bool json_uint_field(const std::string &json, const std::string &key,
                    uint32_t default_value, uint32_t *value)
{
    const std::string marker = "\"" + key + "\"";
    const size_t marker_pos = json.find(marker);
    if (marker_pos == std::string::npos) {
        *value = default_value;
        return true;
    }
    const size_t colon = json.find(':', marker_pos + marker.size());
    if (colon == std::string::npos) return false;
    size_t cursor = colon + 1U;
    while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t')) ++cursor;
    if (cursor >= json.size() || json[cursor] < '0' || json[cursor] > '9') return false;
    uint64_t parsed = 0;
    while (cursor < json.size() && json[cursor] >= '0' && json[cursor] <= '9') {
        parsed = parsed * 10U + static_cast<uint64_t>(json[cursor] - '0');
        if (parsed > 0xffffffffULL) return false;
        ++cursor;
    }
    *value = static_cast<uint32_t>(parsed);
    return true;
}

/* 从简单 JSON 对象中读取必填字符串，避免蓝牙地址缺失时调用 SDK。 */
bool required_string_field(const std::string &json, const std::string &key,
                           std::string *value)
{
    return json_string_field(json, key, value) && !value->empty();
}

/* 构造统一的模块 API 响应，并保留底层命令的可验证错误信息。 */
HttpResponse wifi_response(const WifiResult &result)
{
    std::ostringstream body;
    body << "{\"ok\":" << (result.ok ? "true" : "false")
         << ",\"code\":\"" << json_escape(result.code)
         << "\",\"message\":\"" << json_escape(result.message) << "\"";
    if (result.ok) {
        body << ",\"data\":" << result.data_json;
    } else {
        body << ",\"native_exit_code\":" << result.native_exit_code
             << ",\"native_output\":\"" << json_escape(result.native_output) << "\"";
    }
    body << '}';
    return {result.http_status, "application/json; charset=utf-8", body.str()};
}

/* 构造不依赖底层硬件命令的 HTTP 错误响应。 */
HttpResponse error_response(int status, const std::string &code, const std::string &message)
{
    std::ostringstream body;
    body << "{\"ok\":false,\"code\":\"" << json_escape(code)
         << "\",\"message\":\"" << json_escape(message) << "\"}";
    return {status, "application/json; charset=utf-8", body.str()};
}

/* 读取静态网页文件；路径只允许落在指定 web 根目录内。 */
HttpResponse static_file(const std::string &web_root, const std::string &path)
{
    std::string relative = path == "/" ? "/index.html" : path;
    if (relative.find("..") != std::string::npos || relative.find('\\') != std::string::npos) {
        return error_response(404, "NOT_FOUND", "static file not found");
    }
    std::ifstream file(web_root + relative, std::ios::binary);
    if (!file.is_open()) {
        return error_response(404, "NOT_FOUND", "static file not found");
    }
    std::ostringstream content;
    content << file.rdbuf();
    std::string content_type = "text/plain; charset=utf-8";
    if (relative.size() >= 5U && relative.substr(relative.size() - 5U) == ".html") {
        content_type = "text/html; charset=utf-8";
    } else if (relative.size() >= 3U && relative.substr(relative.size() - 3U) == ".js") {
        content_type = "application/javascript; charset=utf-8";
    } else if (relative.size() >= 4U && relative.substr(relative.size() - 4U) == ".css") {
        content_type = "text/css; charset=utf-8";
    }
    return {200, content_type, content.str()};
}

class HardwareTestApplication {
public:
    HardwareTestApplication(std::string interface_name,
                            std::string control_path,
                            std::string web_root,
                            std::string camera_default_config,
                            std::string camera_runtime_config,
                            std::string camera_sensor,
                            std::string audio_default_config,
                            std::string audio_runtime_config)
        : wifi_(std::move(interface_name), std::move(control_path)),
          web_root_(std::move(web_root)),
          camera_(std::move(camera_default_config),
                  std::move(camera_runtime_config),
                  std::move(camera_sensor)),
          audio_(std::move(audio_default_config),
                 std::move(audio_runtime_config))
    {
        std::string error_message;
        (void)camera_.start(&error_message);
        if (!audio_.start(&error_message)) {
            std::fprintf(stderr, "audio start failed: %s\n", error_message.c_str());
        }
        if (!bluetooth_.start(&error_message)) {
            std::fprintf(stderr, "bluetooth start failed: %s\n", error_message.c_str());
        }
    }

    HttpResponse handle(const HttpRequest &request)
    {
        if (request.path == "/api/v1/health" && request.method == "GET") {
            return {200, "application/json; charset=utf-8",
                    "{\"ok\":true,\"service\":\"hardware_test_demo\"}"};
        }
        if (request.path == "/api/v1/modules" && request.method == "GET") {
            return {200, "application/json; charset=utf-8",
                    "{\"ok\":true,\"modules\":[{\"id\":\"wifi\",\"name\":\"WiFi\",\"state\":\"enabled\"},{\"id\":\"camera\",\"name\":\"Camera\",\"state\":\"enabled\"},{\"id\":\"audio\",\"name\":\"Audio\",\"state\":\"enabled\"},{\"id\":\"bluetooth\",\"name\":\"Bluetooth\",\"state\":\"enabled\"}]}"};
        }
        if (request.path == "/api/v1/bluetooth/status" && request.method == "GET") {
            return {200, "application/json; charset=utf-8", bluetooth_.status_json()};
        }
        if (request.path == "/api/v1/bluetooth/devices" && request.method == "GET") {
            return {200, "application/json; charset=utf-8", bluetooth_.devices_json()};
        }
        if (request.path == "/api/v1/bluetooth/scan/start" && request.method == "POST") {
            std::string error_message;
            if (!bluetooth_.start_scan(&error_message)) return error_response(409, "BLUETOOTH_SCAN_START_FAILED", error_message);
            return {200, "application/json; charset=utf-8", "{\"ok\":true}"};
        }
        if (request.path == "/api/v1/bluetooth/scan/stop" && request.method == "POST") {
            std::string error_message;
            if (!bluetooth_.stop_scan(&error_message)) return error_response(409, "BLUETOOTH_SCAN_STOP_FAILED", error_message);
            return {200, "application/json; charset=utf-8", "{\"ok\":true}"};
        }
        if (request.path == "/api/v1/bluetooth/action" && request.method == "POST") {
            std::string action;
            std::string address;
            if (!required_string_field(request.body, "action", &action) ||
                !required_string_field(request.body, "address", &address)) {
                return error_response(400, "INVALID_REQUEST", "action and address are required");
            }
            const bool enabled = json_bool_field(request.body, "enabled", true);
            std::string error_message;
            bool success = false;
            if (action == "pair") success = bluetooth_.pair(address, &error_message);
            else if (action == "cancel_pair") success = bluetooth_.cancel_pair(address, &error_message);
            else if (action == "remove") success = bluetooth_.remove(address, &error_message);
            else if (action == "trust") success = bluetooth_.trust(address, enabled, &error_message);
            else if (action == "block") success = bluetooth_.block(address, enabled, &error_message);
            else if (action == "connect") success = bluetooth_.connect(address, &error_message);
            else if (action == "disconnect") success = bluetooth_.disconnect(address, &error_message);
            else return error_response(400, "INVALID_ACTION", "unsupported bluetooth action");
            if (!success) return error_response(409, "BLUETOOTH_ACTION_FAILED", error_message);
            return {200, "application/json; charset=utf-8", "{\"ok\":true}"};
        }
        if (request.path == "/api/v1/camera/status" && request.method == "GET") {
            return {200, "application/json; charset=utf-8", camera_.status_json()};
        }
        if (request.path == "/api/v1/camera/snapshot.jpg" && request.method == "GET") {
            std::string jpeg_data;
            std::string error_message;
            if (!camera_.snapshot(&jpeg_data, &error_message)) {
                return error_response(503, "CAMERA_SNAPSHOT_FAILED", error_message);
            }
            return {200, "image/jpeg", std::move(jpeg_data)};
        }
        if (request.path == "/api/v1/audio/status" && request.method == "GET") {
            return {200, "application/json; charset=utf-8", audio_.status_json()};
        }
        if (request.path == "/api/v1/audio/record/start" && request.method == "POST") {
            std::string channel;
            if (!json_string_field(request.body, "channel", &channel)) channel = "left";
            std::string error_message;
            if (!audio_.start_record(channel, &error_message)) {
                return error_response(409, "AUDIO_RECORD_START_FAILED", error_message);
            }
            return {200, "application/json; charset=utf-8", "{\"ok\":true}"};
        }
        if (request.path == "/api/v1/audio/record/stop" && request.method == "POST") {
            std::string error_message;
            if (!audio_.stop_record(&error_message)) {
                return error_response(409, "AUDIO_RECORD_STOP_FAILED", error_message);
            }
            return {200, "application/json; charset=utf-8", "{\"ok\":true}"};
        }
        if (request.path == "/api/v1/audio/record/download" && request.method == "GET") {
            std::string data;
            std::string error_message;
            if (!audio_.read_recording(&data, &error_message)) {
                return error_response(404, "AUDIO_RECORDING_NOT_FOUND", error_message);
            }
            return {200, "audio/ogg", std::move(data)};
        }
        if (request.path == "/api/v1/audio/record/play" && request.method == "POST") {
            uint32_t loops = 1;
            if (!json_uint_field(request.body, "loop_count", 1, &loops)) {
                return error_response(400, "INVALID_REQUEST", "invalid loop count");
            }
            const bool infinite = json_bool_field(request.body, "infinite_loop", false);
            std::string error_message;
            if (!audio_.play("recording", 0, 0, 80, loops, infinite, &error_message)) {
                return error_response(409, "AUDIO_RECORD_PLAY_FAILED", error_message);
            }
            return {200, "application/json; charset=utf-8", "{\"ok\":true}"};
        }
        if (request.path == "/api/v1/audio/upload" && request.method == "POST") {
            std::string error_message;
            if (!audio_.upload(request.body, &error_message)) {
                return error_response(400, "AUDIO_UPLOAD_FAILED", error_message);
            }
            return {200, "application/json; charset=utf-8", "{\"ok\":true}"};
        }
        if (request.path == "/api/v1/audio/play" && request.method == "POST") {
            std::string source;
            if (!json_string_field(request.body, "source", &source)) source = "voice";
            uint32_t frequency = 1000;
            uint32_t duration = 1000;
            uint32_t volume = 80;
            uint32_t loops = 1;
            if (!json_uint_field(request.body, "frequency_hz", frequency, &frequency) ||
                !json_uint_field(request.body, "duration_ms", duration, &duration) ||
                !json_uint_field(request.body, "volume", volume, &volume) ||
                !json_uint_field(request.body, "loop_count", loops, &loops)) {
                return error_response(400, "INVALID_REQUEST", "invalid numeric audio parameter");
            }
            const bool infinite = json_bool_field(request.body, "infinite_loop", false);
            std::string error_message;
            if (!audio_.play(source, frequency, duration, volume, loops, infinite, &error_message)) {
                return error_response(409, "AUDIO_PLAY_FAILED", error_message);
            }
            return {200, "application/json; charset=utf-8", "{\"ok\":true}"};
        }
        if (request.path == "/api/v1/audio/stop" && request.method == "POST") {
            audio_.stop_playback();
            return {200, "application/json; charset=utf-8", "{\"ok\":true}"};
        }
        if (request.path == "/api/v1/audio/volume" && request.method == "POST") {
            uint32_t volume = 0;
            if (!json_uint_field(request.body, "volume", 0, &volume)) {
                return error_response(400, "INVALID_REQUEST", "volume is required");
            }
            std::string error_message;
            if (!audio_.set_volume(volume, &error_message)) {
                return error_response(400, "AUDIO_VOLUME_FAILED", error_message);
            }
            return {200, "application/json; charset=utf-8", "{\"ok\":true}"};
        }
        if (request.path == "/api/v1/audio/gain" && request.method == "POST") {
            uint32_t gain = 0;
            if (!json_uint_field(request.body, "gain", 0, &gain)) {
                return error_response(400, "INVALID_REQUEST", "gain is required");
            }
            std::string error_message;
            if (!audio_.set_gain(gain, &error_message)) {
                return error_response(400, "AUDIO_GAIN_FAILED", error_message);
            }
            return {200, "application/json; charset=utf-8", "{\"ok\":true}"};
        }
        if (request.path == "/api/v1/wifi/status" && request.method == "GET") {
            return wifi_response(wifi_.status());
        }
        if (request.path == "/api/v1/wifi/saved" && request.method == "GET") {
            return wifi_response(wifi_.saved_networks());
        }
        if (request.path == "/api/v1/wifi/scan" && request.method == "POST") {
            return wifi_response(wifi_.scan());
        }
        if (request.path == "/api/v1/wifi/saved" && request.method == "POST") {
            std::string ssid;
            std::string password;
            std::string auth;
            if (!json_string_field(request.body, "ssid", &ssid)) {
                return error_response(400, "INVALID_REQUEST", "ssid is required");
            }
            (void)json_string_field(request.body, "password", &password);
            if (!json_string_field(request.body, "auth", &auth)) {
                auth = "WPA-PSK";
            }
            const bool connect_now = json_bool_field(request.body, "connect_now", false);
            return wifi_response(wifi_.add_network(ssid, password, auth, connect_now));
        }
        const std::string saved_prefix = "/api/v1/wifi/saved/";
        if (request.path.compare(0U, saved_prefix.size(), saved_prefix) == 0) {
            const std::string network_id = request.path.substr(saved_prefix.size());
            if (request.method == "DELETE") {
                return wifi_response(wifi_.delete_network(network_id));
            }
        }
        if (request.path == "/api/v1/wifi/connect" && request.method == "POST") {
            std::string network_id;
            if (!json_string_field(request.body, "network_id", &network_id)) {
                return error_response(400, "INVALID_REQUEST", "network_id is required");
            }
            return wifi_response(wifi_.connect_network(network_id));
        }
        if (request.path == "/api/v1/wifi/disconnect" && request.method == "POST") {
            return wifi_response(wifi_.disconnect());
        }
        if (request.method == "GET") {
            return static_file(web_root_, request.path);
        }
        return error_response(405, "METHOD_NOT_ALLOWED", "method is not supported");
    }

private:
    NativeWifi wifi_;
    std::string web_root_;
    NativeCamera camera_;
    NativeAudio audio_;
    NativeBluetooth bluetooth_;
};

} // namespace
} // namespace hardware_test

int main(int argc, char **argv)
{
    std::signal(SIGPIPE, SIG_IGN);
    int port = 8090;
    std::string interface_name = "wlan0";
    std::string control_path = "/etc/wireless/wpa_supplicant";
    std::string web_root = "web";
    std::string camera_default_config = "configs/hal_default.json";
    std::string camera_runtime_config = "configs/hal.json";
    /* SDK 配置名必须使用 sc2356；实际板端驱动由链接的 fpx1002 库提供。 */
    std::string camera_sensor = "sc2356";
    std::string audio_default_config = "configs/hal_default.json";
    std::string audio_runtime_config = "configs/hal.json";

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--port" && index + 1 < argc) {
            port = std::atoi(argv[++index]);
        } else if (argument == "--iface" && index + 1 < argc) {
            interface_name = argv[++index];
        } else if (argument == "--wpa-control-path" && index + 1 < argc) {
            control_path = argv[++index];
        } else if (argument == "--web-root" && index + 1 < argc) {
            web_root = argv[++index];
        } else if (argument == "--camera-default" && index + 1 < argc) {
            camera_default_config = argv[++index];
        } else if (argument == "--camera-runtime" && index + 1 < argc) {
            camera_runtime_config = argv[++index];
        } else if (argument == "--camera-sensor" && index + 1 < argc) {
            camera_sensor = argv[++index];
        }
    }
    if (port < 1 || port > 65535 || interface_name.empty()) {
        return 2;
    }

    hardware_test::HardwareTestApplication application(interface_name, control_path, web_root,
                                                        camera_default_config,
                                                        camera_runtime_config,
                                                        camera_sensor,
                                                        audio_default_config,
                                                        audio_runtime_config);
    hardware_test::HttpServer server(port, [&application](const hardware_test::HttpRequest &request) {
        return application.handle(request);
    });
    return server.run();
}
