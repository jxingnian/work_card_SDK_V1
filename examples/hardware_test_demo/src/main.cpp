#include "http_server.h"
#include "camera_native.h"
#include "wifi_native.h"

#include <csignal>
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
                            std::string camera_sensor)
        : wifi_(std::move(interface_name), std::move(control_path)),
          web_root_(std::move(web_root)),
          camera_(std::move(camera_default_config),
                  std::move(camera_runtime_config),
                  std::move(camera_sensor))
    {
        std::string error_message;
        (void)camera_.start(&error_message);
    }

    HttpResponse handle(const HttpRequest &request)
    {
        if (request.path == "/api/v1/health" && request.method == "GET") {
            return {200, "application/json; charset=utf-8",
                    "{\"ok\":true,\"service\":\"hardware_test_demo\"}"};
        }
        if (request.path == "/api/v1/modules" && request.method == "GET") {
            return {200, "application/json; charset=utf-8",
                    "{\"ok\":true,\"modules\":[{\"id\":\"wifi\",\"name\":\"WiFi\",\"state\":\"enabled\"},{\"id\":\"camera\",\"name\":\"Camera\",\"state\":\"enabled\"}]}"};
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
                                                        camera_sensor);
    hardware_test::HttpServer server(port, [&application](const hardware_test::HttpRequest &request) {
        return application.handle(request);
    });
    return server.run();
}
