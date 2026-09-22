#include "ehal_bluetooth.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;

void on_signal(int)
{
    g_stop = 1;
}

std::string trim(const std::string &value)
{
    const char *spaces = " \t\r\n";
    size_t first = value.find_first_not_of(spaces);
    if (first == std::string::npos) {
        return "";
    }
    size_t last = value.find_last_not_of(spaces);
    return value.substr(first, last - first + 1U);
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
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
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

std::string shell_quote(const std::string &value)
{
    std::string result = "'";
    for (char ch : value) {
        if (ch == '\'') {
            result += "'\\''";
        } else {
            result += ch;
        }
    }
    result += "'";
    return result;
}

std::string run_command(const std::string &command)
{
    std::array<char, 512> buffer{};
    std::string output;
    FILE *pipe = popen((command + " 2>&1").c_str(), "r");
    if (pipe == nullptr) {
        return "popen failed: " + std::string(std::strerror(errno));
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    (void)pclose(pipe);
    return output;
}

int run_command_status(const std::string &command, std::string *output)
{
    std::array<char, 512> buffer{};
    FILE *pipe = popen((command + " 2>&1").c_str(), "r");
    if (pipe == nullptr) {
        if (output != nullptr) {
            *output = "popen failed: " + std::string(std::strerror(errno));
        }
        return -1;
    }
    if (output != nullptr) {
        output->clear();
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        if (output != nullptr) {
            *output += buffer.data();
        }
    }
    int status = pclose(pipe);
    return status >= 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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

void send_response(int fd,
                   int status,
                   const std::string &type,
                   const std::string &body)
{
    const char *reason = status == 200 ? "OK" :
                         status == 404 ? "Not Found" : "Error";
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

std::string value_or(const std::map<std::string, std::string> &query,
                     const char *key,
                     const char *fallback = "")
{
    auto it = query.find(key);
    return it == query.end() ? fallback : it->second;
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

ehal_bt_profile_t profile_from_name(const std::string &value)
{
    if (value == "a2dp-source") return EHAL_BT_PROFILE_A2DP_SOURCE;
    if (value == "a2dp-sink") return EHAL_BT_PROFILE_A2DP_SINK;
    if (value == "hfp-ag") return EHAL_BT_PROFILE_HFP_AG;
    if (value == "hfp-hf") return EHAL_BT_PROFILE_HFP_HF;
    if (value == "hsp-ag") return EHAL_BT_PROFILE_HSP_AG;
    if (value == "hsp-hs") return EHAL_BT_PROFILE_HSP_HS;
    return EHAL_BT_PROFILE_ANY;
}

int bool_value(const std::string &value)
{
    return value == "1" || value == "on" || value == "true" ? 1 : 0;
}

class BluetoothWebDemo {
public:
    BluetoothWebDemo(int port, std::string web_root)
        : port_(port), web_root_(std::move(web_root))
    {
    }

    ~BluetoothWebDemo()
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
        config.command_timeout_ms = 3000U;
        config.scan_timeout_ms = 10000U;
        int ret = ehal_bt_create(&bt_);
        if (ret == EHAL_OK) {
            ret = ehal_bt_configure(bt_, &config);
        }
        if (ret == EHAL_OK) {
            ret = ehal_bt_start(bt_);
        }
        if (ret != EHAL_OK) {
            std::fprintf(stderr, "bluetooth SDK init failed: %s (%d)\n",
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

        std::printf("Bluetooth web demo listening on 0.0.0.0:%d\n", port_);
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
    std::string last_output() const
    {
        char output[8192] = {};
        if (bt_ != nullptr &&
            ehal_bt_get_last_output(bt_, output, sizeof(output)) == EHAL_OK) {
            return output;
        }
        return "";
    }

    std::string result_json(int ret, const std::string &extra = "")
    {
        std::ostringstream out;
        out << "{\"ok\":" << (ret == EHAL_OK ? "true" : "false")
            << ",\"code\":" << ret
            << ",\"error\":\"" << json_escape(ehal_bt_error_string(ret))
            << "\",\"last_output\":\"" << json_escape(last_output()) << "\"";
        if (!extra.empty()) {
            out << ',' << extra;
        }
        out << '}';
        return out.str();
    }

    std::string adapter_json()
    {
        ehal_bt_adapter_info_t info{};
        int ret = ehal_bt_get_adapter_info(bt_, &info);
        if (ret != EHAL_OK) {
            return result_json(ret);
        }
        std::ostringstream extra;
        extra << "\"adapter\":{"
              << "\"address\":\"" << json_escape(info.address) << "\","
              << "\"name\":\"" << json_escape(info.name) << "\","
              << "\"alias\":\"" << json_escape(info.alias) << "\","
              << "\"version\":\"" << json_escape(info.version) << "\","
              << "\"manufacturer\":\"" << json_escape(info.manufacturer) << "\","
              << "\"powered\":" << info.powered << ','
              << "\"pairable\":" << info.pairable << ','
              << "\"discoverable\":" << info.discoverable << ','
              << "\"discovering\":" << info.discovering << '}';
        return result_json(ret, extra.str());
    }

    std::string devices_json()
    {
        ehal_bt_device_info_t devices[64] = {};
        uint32_t count = 0U;
        int ret = ehal_bt_get_devices(bt_, devices, 64U, &count);
        if (ret != EHAL_OK) {
            return result_json(ret);
        }
        std::ostringstream extra;
        extra << "\"devices\":[";
        for (uint32_t i = 0U; i < count && i < 64U; ++i) {
            if (i != 0U) {
                extra << ',';
            }
            extra << "{\"address\":\"" << json_escape(devices[i].address)
                  << "\",\"name\":\"" << json_escape(devices[i].name)
                  << "\",\"alias\":\"" << json_escape(devices[i].alias)
                  << "\",\"rssi\":" << devices[i].rssi
                  << ",\"paired\":" << devices[i].paired
                  << ",\"trusted\":" << devices[i].trusted
                  << ",\"connected\":" << devices[i].connected
                  << ",\"blocked\":" << devices[i].blocked << '}';
        }
        extra << "],\"count\":" << count;
        return result_json(ret, extra.str());
    }

    std::string device_info_json(const std::string &address)
    {
        ehal_bt_device_info_t info{};
        int ret = ehal_bt_get_device_info(bt_, address.c_str(), &info);
        if (ret != EHAL_OK) {
            return result_json(ret);
        }
        std::ostringstream extra;
        extra << "\"device\":{\"address\":\"" << json_escape(info.address)
              << "\",\"name\":\"" << json_escape(info.name)
              << "\",\"alias\":\"" << json_escape(info.alias)
              << "\",\"rssi\":" << info.rssi
              << ",\"paired\":" << info.paired
              << ",\"trusted\":" << info.trusted
              << ",\"connected\":" << info.connected
              << ",\"blocked\":" << info.blocked << '}';
        return result_json(ret, extra.str());
    }

    std::string action_json(const std::map<std::string, std::string> &query)
    {
        const std::string action = value_or(query, "action");
        const std::string address = value_or(query, "address");
        int ret = EHAL_ERR_PARAM;
        std::string extra;

        if (action == "power") {
            ret = ehal_bt_set_power(bt_, bool_value(value_or(query, "value")));
        } else if (action == "pair_prepare") {
            std::string command_output;
            int status = run_command_status(
                "bluetoothctl --timeout 3 agent NoInputNoOutput; "
                "bluetoothctl --timeout 3 pairable on",
                &command_output);
            ret = status == 0 ? EHAL_OK : EHAL_ERR_RUNTIME;
            extra = "\"command_output\":\"" + json_escape(command_output) + "\"";
        } else if (action == "name" || action == "alias") {
            ret = ehal_bt_set_name(bt_, value_or(query, "value").c_str());
        } else if (action == "pairable") {
            ret = ehal_bt_set_pairable(bt_, bool_value(value_or(query, "value")));
        } else if (action == "discoverable") {
            ret = ehal_bt_set_discoverable(bt_, bool_value(value_or(query, "value")));
        } else if (action == "scan_start") {
            ret = ehal_bt_start_scan(bt_);
        } else if (action == "scan_stop") {
            ret = ehal_bt_stop_scan(bt_);
        } else if (action == "info") {
            return device_info_json(address);
        } else if (action == "pair") {
            ret = ehal_bt_pair(bt_, address.c_str());
        } else if (action == "cancel_pair") {
            ret = ehal_bt_cancel_pair(bt_, address.c_str());
        } else if (action == "remove") {
            ret = ehal_bt_remove_device(bt_, address.c_str());
        } else if (action == "trust") {
            ret = ehal_bt_set_trusted(bt_, address.c_str(), bool_value(value_or(query, "value")));
        } else if (action == "block") {
            ret = ehal_bt_set_blocked(bt_, address.c_str(), bool_value(value_or(query, "value")));
        } else if (action == "connect") {
            ret = ehal_bt_connect(bt_, address.c_str());
        } else if (action == "disconnect") {
            ret = ehal_bt_disconnect(bt_, address.c_str());
        } else if (action == "profile_connect" || action == "profile_disconnect") {
            ehal_bt_profile_t profile = profile_from_name(value_or(query, "profile"));
            ret = action == "profile_connect" ?
                ehal_bt_connect_profile(bt_, address.c_str(), profile) :
                ehal_bt_disconnect_profile(bt_, address.c_str(), profile);
        } else if (action == "gatt_services") {
            ret = ehal_bt_gatt_discover_services(bt_, address.c_str());
        } else if (action == "gatt_read") {
            ret = ehal_bt_gatt_read(bt_, address.c_str(),
                                    value_or(query, "characteristic").c_str());
        } else if (action == "gatt_write") {
            const std::string data = value_or(query, "data");
            ret = ehal_bt_gatt_write(bt_, address.c_str(),
                                     value_or(query, "characteristic").c_str(),
                                     reinterpret_cast<const uint8_t *>(data.data()),
                                     static_cast<uint32_t>(data.size()));
        } else if (action == "gatt_notify_start" || action == "gatt_notify_stop") {
            ret = action == "gatt_notify_start" ?
                ehal_bt_gatt_start_notify(bt_, address.c_str(),
                                          value_or(query, "characteristic").c_str()) :
                ehal_bt_gatt_stop_notify(bt_, address.c_str(),
                                         value_or(query, "characteristic").c_str());
        } else if (action == "a2dp_info") {
            ehal_bt_audio_info_t info{};
            ret = ehal_bt_a2dp_get_info(bt_, address.c_str(), &info);
            extra = "\"audio\":{\"codec\":\"" + json_escape(info.codec) +
                    "\",\"pcm_path\":\"" + json_escape(info.pcm_path) +
                    "\",\"sample_rate\":" + std::to_string(info.sample_rate) +
                    ",\"channels\":" + std::to_string(info.channels) +
                    ",\"bit_width\":" + std::to_string(info.bit_width) + '}';
        } else if (action == "a2dp_volume") {
            ret = ehal_bt_a2dp_set_volume(bt_, address.c_str(),
                                          std::atoi(value_or(query, "value").c_str()));
        } else if (action == "a2dp_mute") {
            ret = ehal_bt_a2dp_set_mute(bt_, address.c_str(),
                                        bool_value(value_or(query, "value")));
        } else if (action == "a2dp_play") {
            ret = ehal_bt_a2dp_play_file(bt_, address.c_str(),
                                         value_or(query, "path").c_str());
        } else if (action == "a2dp_stop") {
            ret = ehal_bt_a2dp_stop(bt_, address.c_str());
        } else if (action == "a2dp_write_pcm") {
            ret = ehal_bt_a2dp_write_pcm(bt_, address.c_str(), "test", 4U,
                                         8000U, 1U, 16U);
        } else if (action == "hfp_open") {
            ret = ehal_bt_hfp_open_sco(bt_, address.c_str());
        } else if (action == "hfp_close") {
            ret = ehal_bt_hfp_close_sco(bt_, address.c_str());
        } else if (action == "hfp_at") {
            ret = ehal_bt_hfp_send_at(bt_, address.c_str(),
                                      value_or(query, "command").c_str());
        } else if (action == "call_dial") {
            ret = ehal_bt_call_dial(bt_, address.c_str(),
                                    value_or(query, "number").c_str());
        } else if (action == "call_accept") {
            ret = ehal_bt_call_accept(bt_, address.c_str());
        } else if (action == "call_reject") {
            ret = ehal_bt_call_reject(bt_, address.c_str());
        } else if (action == "call_hangup") {
            ret = ehal_bt_call_hangup(bt_, address.c_str());
        } else if (action == "call_hold") {
            ret = ehal_bt_call_hold(bt_, address.c_str());
        } else if (action == "call_mute") {
            ret = ehal_bt_call_mute(bt_, address.c_str(),
                                    bool_value(value_or(query, "value")));
        } else if (action == "avrcp_play" || action == "avrcp_pause" ||
                   action == "avrcp_stop" || action == "avrcp_next" ||
                   action == "avrcp_previous") {
            if (action == "avrcp_play") ret = ehal_bt_avrcp_play(bt_, address.c_str());
            if (action == "avrcp_pause") ret = ehal_bt_avrcp_pause(bt_, address.c_str());
            if (action == "avrcp_stop") ret = ehal_bt_avrcp_stop(bt_, address.c_str());
            if (action == "avrcp_next") ret = ehal_bt_avrcp_next(bt_, address.c_str());
            if (action == "avrcp_previous") ret = ehal_bt_avrcp_previous(bt_, address.c_str());
        }
        return result_json(ret, extra);
    }

    std::string audio_diagnostics()
    {
        std::ostringstream out;
        out << "{\"ok\":true,\"cards\":\""
            << json_escape(run_command("cat /proc/asound/cards")) << "\","
            << "\"snd\":\"" << json_escape(run_command("ls -l /dev/snd 2>&1"))
            << "\",\"bluealsa\":\""
            << json_escape(run_command("bluealsa-aplay -l; bluealsa-aplay -L; bluealsa-cli list-pcms"))
            << "\",\"aplay\":\"" << json_escape(run_command("aplay -L 2>&1"))
            << "\",\"arecord\":\"" << json_escape(run_command("arecord -L 2>&1"))
            << "\"}";
        return out.str();
    }

    std::string audio_test(const std::map<std::string, std::string> &query)
    {
        const std::string action = value_or(query, "action");
        const std::string device = value_or(query, "device");
        if (device.empty()) {
            return "{\"ok\":false,\"error\":\"device is required\"}";
        }
        if (action == "record") {
            std::string command = "arecord -D " + shell_quote(device) +
                " -f S16_LE -r 8000 -c 1 -t wav -d 10 /tmp/bt_web_record.wav "
                ">/tmp/bt_web_record.log 2>&1";
            int status = std::system(command.c_str());
            std::string output = run_command("cat /tmp/bt_web_record.log");
            bool ok = status == 0;
            return std::string("{\"ok\":") + (ok ? "true" : "false") +
                   ",\"exit_status\":" + std::to_string(status) +
                   ",\"file\":\"/tmp/bt_web_record.wav\",\"output\":\"" +
                   json_escape(output) + "\"}";
        }
        if (action == "play") {
            std::string command = "aplay -D " + shell_quote(device) +
                " /tmp/bt_web_record.wav >/tmp/bt_web_play.log 2>&1";
            int status = std::system(command.c_str());
            std::string output = run_command("cat /tmp/bt_web_play.log");
            bool ok = status == 0;
            return std::string("{\"ok\":") + (ok ? "true" : "false") +
                   ",\"exit_status\":" + std::to_string(status) +
                   ",\"file\":\"/tmp/bt_web_record.wav\",\"output\":\"" +
                   json_escape(output) + "\"}";
        }
        return "{\"ok\":false,\"error\":\"unknown audio test\"}";
    }

    void handle_client(int client)
    {
        char buffer[32768] = {};
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
        } else if (path == "/api/adapter") {
            send_response(client, 200, "application/json; charset=utf-8", adapter_json());
        } else if (path == "/api/devices") {
            send_response(client, 200, "application/json; charset=utf-8", devices_json());
        } else if (path == "/api/action") {
            send_response(client, 200, "application/json; charset=utf-8", action_json(query));
        } else if (path == "/api/audio/diagnostics") {
            send_response(client, 200, "application/json; charset=utf-8", audio_diagnostics());
        } else if (path == "/api/audio/test") {
            send_response(client, 200, "application/json; charset=utf-8", audio_test(query));
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
    BluetoothWebDemo demo(port, web_root);
    int ret = demo.initialize();
    if (ret != EHAL_OK) {
        return 1;
    }
    return demo.run();
}
