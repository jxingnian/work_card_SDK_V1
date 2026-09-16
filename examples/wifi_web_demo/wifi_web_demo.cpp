#include "ehal_wifi.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kDefaultPort = 8090;
constexpr size_t kMaxHeaderSize = 16384U;
constexpr size_t kMaxBodySize = 8192U;

volatile sig_atomic_t g_exit_requested = 0;

static void handle_signal(int signal_number)
{
    if (signal_number == SIGINT || signal_number == SIGTERM) {
        g_exit_requested = 1;
    }
}

struct Config {
    int port = kDefaultPort;
    std::string web_root = "web";
    std::string interface_name = "wlan0";
    std::string wpa_config_path = "/etc/wireless/wpa_supplicant.conf";
};

static std::string read_file(const std::string &path)
{
    std::ifstream file(path.c_str(), std::ios::binary);
    if (!file.is_open()) {
        return "";
    }
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

static std::string content_type(const std::string &path)
{
    if (path.size() >= 5U && path.compare(path.size() - 5U, 5U, ".html") == 0) {
        return "text/html; charset=utf-8";
    }
    if (path.size() >= 4U && path.compare(path.size() - 4U, 4U, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (path.size() >= 3U && path.compare(path.size() - 3U, 3U, ".js") == 0) {
        return "application/javascript; charset=utf-8";
    }
    return "application/octet-stream";
}

static std::string json_escape(const std::string &value)
{
    std::string out;
    out.reserve(value.size() + 8U);
    for (char ch : value) {
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += ch; break;
        }
    }
    return out;
}

static std::string url_decode(const std::string &value)
{
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2U < value.size()) {
            char hex[3] = {value[i + 1U], value[i + 2U], '\0'};
            out += static_cast<char>(std::strtol(hex, nullptr, 16));
            i += 2U;
        } else if (value[i] == '+') {
            out += ' ';
        } else {
            out += value[i];
        }
    }
    return out;
}

static std::map<std::string, std::string> parse_form(const std::string &body)
{
    std::map<std::string, std::string> form;
    size_t start = 0U;
    while (start <= body.size()) {
        size_t end = body.find('&', start);
        std::string item = body.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t equal = item.find('=');
        if (equal != std::string::npos) {
            form[url_decode(item.substr(0U, equal))] = url_decode(item.substr(equal + 1U));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1U;
    }
    return form;
}

static const char *state_name(ehal_wifi_state_t state)
{
    switch (state) {
    case EHAL_WIFI_STATE_DISABLED: return "disabled";
    case EHAL_WIFI_STATE_DISCONNECTED: return "disconnected";
    case EHAL_WIFI_STATE_SCANNING: return "scanning";
    case EHAL_WIFI_STATE_CONNECTING: return "connecting";
    case EHAL_WIFI_STATE_CONNECTED: return "connected";
    case EHAL_WIFI_STATE_AP: return "ap";
    default: return "unknown";
    }
}

static const char *auth_name(ehal_wifi_auth_t auth)
{
    switch (auth) {
    case EHAL_WIFI_AUTH_OPEN: return "open";
    case EHAL_WIFI_AUTH_WPA_PSK: return "wpa";
    case EHAL_WIFI_AUTH_WPA2_PSK: return "wpa2";
    case EHAL_WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
    default: return "unknown";
    }
}

static bool send_all(int fd, const std::string &data)
{
    size_t sent = 0U;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

static void send_response(int fd,
                          int status,
                          const std::string &type,
                          const std::string &body)
{
    const char *reason = status == 200 ? "OK" :
                         status == 400 ? "Bad Request" :
                         status == 404 ? "Not Found" :
                         status == 500 ? "Internal Server Error" :
                         "Error";
    std::ostringstream header;
    header << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
           << "Content-Type: " << type << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n"
           << "Cache-Control: no-store\r\n\r\n";
    (void)send_all(fd, header.str());
    (void)send_all(fd, body);
}

static void send_json(int fd, int status, const std::string &body)
{
    send_response(fd, status, "application/json; charset=utf-8", body);
}

class WifiWebDemo {
public:
    ~WifiWebDemo()
    {
        if (wifi_ != nullptr) {
            ehal_wifi_destroy(wifi_);
        }
    }

    int init(const Config &config)
    {
        config_ = config;
        int ret = ehal_wifi_create(&wifi_);
        if (ret != EHAL_OK) {
            return ret;
        }
        ehal_wifi_config_t wifi_config{};
        wifi_config.interface_name = config_.interface_name.c_str();
        wifi_config.wpa_config_path = config_.wpa_config_path.c_str();
        wifi_config.connect_timeout_ms = 15000U;
        wifi_config.dhcp_timeout_ms = 10000U;
        return ehal_wifi_configure(wifi_, &wifi_config);
    }

    void run()
    {
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            std::cerr << "create socket failed: " << std::strerror(errno) << '\n';
            return;
        }
        int reuse = 1;
        (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(static_cast<uint16_t>(config_.port));
        if (bind(server_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
            listen(server_fd, 8) < 0) {
            std::cerr << "start web server failed: " << std::strerror(errno) << '\n';
            close(server_fd);
            return;
        }

        std::cout << "wifi web demo listening on port " << config_.port << '\n';
        while (!g_exit_requested) {
            int client_fd = accept(server_fd, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            std::thread(&WifiWebDemo::handle_client, this, client_fd).detach();
        }
        close(server_fd);
    }

private:
    void handle_client(int fd)
    {
        std::string request;
        char buffer[1024];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < kMaxHeaderSize) {
            ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                close(fd);
                return;
            }
            request.append(buffer, static_cast<size_t>(n));
        }

        size_t header_end = request.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            send_json(fd, 400, "{\"ok\":false,\"error\":\"bad request\"}");
            close(fd);
            return;
        }

        std::istringstream line_stream(request.substr(0U, header_end));
        std::string method;
        std::string path;
        std::string version;
        line_stream >> method >> path >> version;

        size_t content_length = 0U;
        std::string line;
        while (std::getline(line_stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            std::string lower = line;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.rfind("content-length:", 0) == 0) {
                content_length = static_cast<size_t>(std::strtoul(line.c_str() + 15, nullptr, 10));
            }
        }
        if (content_length > kMaxBodySize) {
            send_json(fd, 400, "{\"ok\":false,\"error\":\"body too large\"}");
            close(fd);
            return;
        }

        std::string body = request.substr(header_end + 4U);
        while (body.size() < content_length) {
            ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                close(fd);
                return;
            }
            body.append(buffer, static_cast<size_t>(n));
        }
        if (body.size() > content_length) {
            body.resize(content_length);
        }

        if (path.rfind("/api/", 0) == 0) {
            handle_api(fd, method, path, body);
        } else {
            handle_static(fd, path);
        }
        close(fd);
    }

    void handle_static(int fd, const std::string &path)
    {
        std::string relative = path == "/" ? "/index.html" : path;
        if (relative.find("..") != std::string::npos) {
            send_response(fd, 404, "text/plain; charset=utf-8", "not found");
            return;
        }
        std::string full_path = config_.web_root + relative;
        std::string body = read_file(full_path);
        if (body.empty()) {
            send_response(fd, 404, "text/plain; charset=utf-8", "not found");
            return;
        }
        send_response(fd, 200, content_type(full_path), body);
    }

    void handle_api(int fd,
                    const std::string &method,
                    const std::string &path,
                    const std::string &body)
    {
        if (method == "GET" && path == "/api/scan") {
            api_scan(fd);
        } else if (method == "GET" && path == "/api/status") {
            api_status(fd);
        } else if (method == "POST" && path == "/api/connect") {
            api_connect(fd, parse_form(body));
        } else if (method == "POST" && path == "/api/disconnect") {
            int ret = ehal_wifi_disconnect(wifi_);
            api_result(fd, ret);
        } else if (method == "POST" && path == "/api/ap/start") {
            api_ap_start(fd, parse_form(body));
        } else if (method == "POST" && path == "/api/ap/stop") {
            int ret = ehal_wifi_stop_ap(wifi_);
            api_result(fd, ret);
        } else {
            send_json(fd, 404, "{\"ok\":false,\"error\":\"not found\"}");
        }
    }

    void api_result(int fd, int ret)
    {
        if (ret == EHAL_OK) {
            send_json(fd, 200, "{\"ok\":true}");
        } else {
            std::ostringstream out;
            out << "{\"ok\":false,\"code\":" << ret
                << ",\"error\":\"" << json_escape(ehal_wifi_error_string(ret)) << "\"}";
            send_json(fd, 500, out.str());
        }
    }

    void api_scan(int fd)
    {
        ehal_wifi_ap_info_t aps[48];
        uint32_t count = 0U;
        int ret = ehal_wifi_scan(wifi_, aps, 48U, &count);
        if (ret != EHAL_OK) {
            api_result(fd, ret);
            return;
        }
        std::ostringstream out;
        out << "{\"ok\":true,\"count\":" << count << ",\"aps\":[";
        uint32_t stored = count < 48U ? count : 48U;
        for (uint32_t i = 0U; i < stored; ++i) {
            if (i != 0U) {
                out << ',';
            }
            out << "{\"ssid\":\"" << json_escape(aps[i].ssid) << "\","
                << "\"bssid\":\"" << json_escape(aps[i].bssid) << "\","
                << "\"signal\":" << aps[i].signal_dbm << ','
                << "\"frequency\":" << aps[i].frequency_mhz << ','
                << "\"auth\":\"" << auth_name(aps[i].auth) << "\"}";
        }
        out << "]}";
        send_json(fd, 200, out.str());
    }

    void api_connect(int fd, const std::map<std::string, std::string> &form)
    {
        auto ssid = form.find("ssid");
        if (ssid == form.end() || ssid->second.empty()) {
            send_json(fd, 400, "{\"ok\":false,\"error\":\"ssid required\"}");
            return;
        }
        std::string password = form.count("password") ? form.at("password") : "";
        ehal_wifi_connect_config_t config{};
        config.ssid = ssid->second.c_str();
        config.password = password.c_str();
        config.auth = password.empty() ? EHAL_WIFI_AUTH_OPEN : EHAL_WIFI_AUTH_WPA_WPA2_PSK;
        config.timeout_ms = 15000U;
        config.save_config = 1;
        int ret = ehal_wifi_connect(wifi_, &config);
        api_result(fd, ret);
    }

    void api_ap_start(int fd, const std::map<std::string, std::string> &form)
    {
        auto ssid = form.find("ssid");
        if (ssid == form.end() || ssid->second.empty()) {
            send_json(fd, 400, "{\"ok\":false,\"error\":\"ssid required\"}");
            return;
        }
        std::string password = form.count("password") ? form.at("password") : "";
        std::string ip = form.count("ip") ? form.at("ip") : "192.168.4.1";
        std::string channel_text = form.count("channel") ? form.at("channel") : "6";
        uint32_t channel = static_cast<uint32_t>(std::strtoul(channel_text.c_str(), nullptr, 10));

        ehal_wifi_ap_config_t config{};
        config.ssid = ssid->second.c_str();
        config.password = password.c_str();
        config.channel = channel;
        config.ip = ip.c_str();
        config.netmask = "255.255.255.0";
        config.dhcp_start = "192.168.4.100";
        config.dhcp_end = "192.168.4.200";
        config.lease_seconds = 86400U;
        int ret = ehal_wifi_start_ap(wifi_, &config);
        api_result(fd, ret);
    }

    void api_status(int fd)
    {
        ehal_wifi_status_t sta{};
        ehal_wifi_ip_info_t ip{};
        ehal_wifi_ap_status_t ap{};
        int sta_ret = ehal_wifi_get_status(wifi_, &sta);
        int ip_ret = ehal_wifi_get_ip_info(wifi_, &ip);
        int ap_ret = ehal_wifi_get_ap_status(wifi_, &ap);

        std::ostringstream out;
        out << "{\"ok\":true";
        if (sta_ret == EHAL_OK) {
            out << ",\"sta\":{\"state\":\"" << state_name(sta.state) << "\","
                << "\"ssid\":\"" << json_escape(sta.ssid) << "\","
                << "\"bssid\":\"" << json_escape(sta.bssid) << "\","
                << "\"signal\":" << sta.signal_dbm << ','
                << "\"ip\":\"" << json_escape(sta.ip) << "\"}";
        }
        if (ip_ret == EHAL_OK) {
            out << ",\"ip\":{\"ip\":\"" << json_escape(ip.ip) << "\","
                << "\"netmask\":\"" << json_escape(ip.netmask) << "\","
                << "\"gateway\":\"" << json_escape(ip.gateway) << "\","
                << "\"dns1\":\"" << json_escape(ip.dns1) << "\","
                << "\"dns2\":\"" << json_escape(ip.dns2) << "\"}";
        }
        if (ap_ret == EHAL_OK) {
            out << ",\"ap\":{\"running\":" << (ap.running ? "true" : "false") << ','
                << "\"ssid\":\"" << json_escape(ap.ssid) << "\","
                << "\"ip\":\"" << json_escape(ap.ip) << "\","
                << "\"netmask\":\"" << json_escape(ap.netmask) << "\","
                << "\"channel\":" << ap.channel << ','
                << "\"stations\":" << ap.station_count << "}";
        }
        out << "}";
        send_json(fd, 200, out.str());
    }

    Config config_;
    ehal_wifi_t *wifi_ = nullptr;
};

static void usage(const char *program)
{
    std::cout << "usage: " << program
              << " [--port 8090] [--web-root web] [--interface wlan0]"
              << " [--wpa-config /etc/wireless/wpa_supplicant.conf]\n";
}

static bool parse_args(int argc, char **argv, Config *config)
{
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            config->port = std::atoi(argv[++i]);
        } else if (arg == "--web-root" && i + 1 < argc) {
            config->web_root = argv[++i];
        } else if (arg == "--interface" && i + 1 < argc) {
            config->interface_name = argv[++i];
        } else if (arg == "--wpa-config" && i + 1 < argc) {
            config->wpa_config_path = argv[++i];
        } else if (arg == "--help") {
            usage(argv[0]);
            return false;
        } else {
            usage(argv[0]);
            return false;
        }
    }
    return config->port > 0 && config->port <= 65535;
}

} // namespace

int main(int argc, char **argv)
{
    Config config;
    if (!parse_args(argc, argv, &config)) {
        return 1;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    WifiWebDemo demo;
    int ret = demo.init(config);
    if (ret != EHAL_OK) {
        std::cerr << "init wifi failed: " << ehal_wifi_error_string(ret) << '\n';
        return 1;
    }
    demo.run();
    return 0;
}
