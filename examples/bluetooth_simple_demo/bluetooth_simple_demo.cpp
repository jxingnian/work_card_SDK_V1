#include "ehal_bluetooth.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>

namespace {

volatile sig_atomic_t g_stop = 0;

void on_signal(int)
{
    g_stop = 1;
}

std::string url_decode(const std::string &value)
{
    std::string result;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2U < value.size()) {
            char hex[3] = {value[i + 1U], value[i + 2U], '\0'};
            char *end = nullptr;
            long parsed = std::strtol(hex, &end, 16);
            if (end != nullptr && *end == '\0') {
                result.push_back(static_cast<char>(parsed));
                i += 2U;
                continue;
            }
        }
        result.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return result;
}

std::string json_escape(const std::string &value)
{
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20U) {
                out << "\\u00";
                const char *hex = "0123456789abcdef";
                out << hex[(ch >> 4U) & 0x0fU] << hex[ch & 0x0fU];
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    return out.str();
}

bool read_file(const std::string &path, std::string *content)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    *content = stream.str();
    return true;
}

bool send_all(int fd, const char *data, size_t size)
{
    while (size > 0U) {
        ssize_t sent = send(fd, data, size, 0);
        if (sent <= 0) {
            return false;
        }
        data += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

void send_response(int fd, int status, const std::string &type, const std::string &body)
{
    const char *reason = status == 200 ? "OK" : status == 404 ? "Not Found" : "Error";
    std::ostringstream header;
    header << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
           << "Content-Type: " << type << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Cache-Control: no-store\r\n"
           << "Connection: close\r\n\r\n";
    std::string text = header.str();
    (void)send_all(fd, text.data(), text.size());
    (void)send_all(fd, body.data(), body.size());
}

std::map<std::string, std::string> parse_query(const std::string &query)
{
    std::map<std::string, std::string> result;
    size_t start = 0U;
    while (start <= query.size()) {
        size_t end = query.find('&', start);
        if (end == std::string::npos) {
            end = query.size();
        }
        std::string item = query.substr(start, end - start);
        size_t equal = item.find('=');
        std::string key = equal == std::string::npos ? item : item.substr(0, equal);
        std::string value = equal == std::string::npos ? "" : item.substr(equal + 1U);
        if (!key.empty()) {
            result[url_decode(key)] = url_decode(value);
        }
        if (end == query.size()) {
            break;
        }
        start = end + 1U;
    }
    return result;
}

std::string value_or(const std::map<std::string, std::string> &query,
                     const char *key,
                     const char *fallback = "")
{
    auto it = query.find(key);
    return it == query.end() ? fallback : it->second;
}

class BluetoothSimpleDemo {
public:
    BluetoothSimpleDemo(int port, std::string web_root)
        : port_(port), web_root_(std::move(web_root))
    {
    }

    ~BluetoothSimpleDemo()
    {
        if (bt_ != nullptr) {
            (void)ehal_bt_stop(bt_);
            ehal_bt_destroy(bt_);
        }
    }

    int initialize()
    {
        ehal_bt_config_t config{};
        config.adapter = "hci0";
        config.command_timeout_ms = 5000U;
        config.scan_timeout_ms = 10000U;

        int ret = ehal_bt_create(&bt_);
        if (ret == EHAL_OK) {
            ret = ehal_bt_configure(bt_, &config);
        }
        if (ret == EHAL_OK) {
            ret = ehal_bt_start(bt_);
        }
        if (ret != EHAL_OK) {
            std::fprintf(stderr, "蓝牙初始化失败: %s (%d)\n",
                         ehal_bt_error_string(ret), ret);
        }
        return ret;
    }

    int run()
    {
        int server = socket(AF_INET, SOCK_STREAM, 0);
        if (server < 0) {
            std::perror("socket");
            return 1;
        }
        int reuse = 1;
        (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(static_cast<uint16_t>(port_));

        if (bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
            listen(server, 8) < 0) {
            std::perror("bind/listen");
            close(server);
            return 1;
        }

        std::printf("蓝牙演示服务启动: http://0.0.0.0:%d\n", port_);
        std::fflush(stdout);

        while (!g_stop) {
            int client = accept(server, nullptr, nullptr);
            if (client < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            std::thread([this, client]() {
                handle_client(client);
                close(client);
            }).detach();
        }
        close(server);
        return 0;
    }

private:
    std::string result_json(int ret, const std::string &extra = "")
    {
        std::ostringstream out;
        out << "{\"ok\":" << (ret == EHAL_OK ? "true" : "false")
            << ",\"code\":" << ret
            << ",\"error\":\"" << json_escape(ehal_bt_error_string(ret)) << "\"";
        if (!extra.empty()) {
            out << ',' << extra;
        }
        out << '}';
        return out.str();
    }

    std::string devices_json()
    {
        ehal_bt_device_info_t devices[64] = {};
        uint32_t count = 0U;
        int ret = ehal_bt_get_devices(bt_, devices, 64U, &count);
        if (ret != EHAL_OK) {
            return result_json(ret);
        }

        std::ostringstream out;
        out << "[";
        for (uint32_t i = 0U; i < count && i < 64U; ++i) {
            if (i != 0U) {
                out << ',';
            }
            out << "{\"address\":\"" << json_escape(devices[i].address)
                << "\",\"name\":\"" << json_escape(devices[i].name)
                << "\",\"rssi\":" << devices[i].rssi
                << ",\"paired\":" << devices[i].paired
                << ",\"connected\":" << devices[i].connected << '}';
        }
        out << "]";

        return result_json(ret, "\"devices\":" + out.str());
    }

    std::string action_json(const std::map<std::string, std::string> &query)
    {
        const std::string action = value_or(query, "action");
        const std::string address = value_or(query, "address");
        int ret = EHAL_ERR_PARAM;

        if (action == "start_scan") {
            ret = ehal_bt_start_scan(bt_);
        } else if (action == "stop_scan") {
            ret = ehal_bt_stop_scan(bt_);
        } else if (action == "pair") {
            ret = ehal_bt_pair(bt_, address.c_str());
        } else if (action == "connect") {
            ret = ehal_bt_connect(bt_, address.c_str());
        } else if (action == "disconnect") {
            ret = ehal_bt_disconnect(bt_, address.c_str());
        } else if (action == "remove") {
            ret = ehal_bt_remove_device(bt_, address.c_str());
        } else if (action == "pair_and_connect") {
            // 配对并连接
            ret = ehal_bt_pair(bt_, address.c_str());
            if (ret == EHAL_OK) {
                ret = ehal_bt_set_trusted(bt_, address.c_str(), 1);
            }
            if (ret == EHAL_OK) {
                ret = ehal_bt_connect(bt_, address.c_str());
            }
        }

        return result_json(ret);
    }

    void handle_client(int client)
    {
        char buffer[8192] = {};
        ssize_t size = recv(client, buffer, sizeof(buffer) - 1U, 0);
        if (size <= 0) {
            return;
        }

        std::string request(buffer, static_cast<size_t>(size));
        size_t line_end = request.find("\r\n");
        if (line_end == std::string::npos) {
            send_response(client, 400, "text/plain", "bad request\n");
            return;
        }

        std::istringstream line(request.substr(0, line_end));
        std::string method;
        std::string target;
        line >> method >> target;

        if (method != "GET") {
            send_response(client, 405, "text/plain", "GET only\n");
            return;
        }

        size_t question = target.find('?');
        std::string path = question == std::string::npos ? target : target.substr(0, question);
        std::map<std::string, std::string> query =
            parse_query(question == std::string::npos ? "" : target.substr(question + 1U));

        if (path == "/" || path == "/index.html") {
            std::string html;
            if (!read_file(web_root_ + "/index.html", &html)) {
                send_response(client, 404, "text/plain", "web/index.html not found\n");
            } else {
                send_response(client, 200, "text/html; charset=utf-8", html);
            }
        } else if (path == "/api/devices") {
            send_response(client, 200, "application/json; charset=utf-8", devices_json());
        } else if (path == "/api/action") {
            send_response(client, 200, "application/json; charset=utf-8", action_json(query));
        } else {
            send_response(client, 404, "text/plain", "not found\n");
        }
    }

    int port_;
    std::string web_root_;
    ehal_bluetooth_t *bt_ = nullptr;
};

} // namespace

int main(int argc, char **argv)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int port = 8080;
    std::string web_root = "web";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (arg == "--web-root" && i + 1 < argc) {
            web_root = argv[++i];
        }
    }

    BluetoothSimpleDemo demo(port, web_root);
    int ret = demo.initialize();
    if (ret != EHAL_OK) {
        return 1;
    }
    return demo.run();
}
