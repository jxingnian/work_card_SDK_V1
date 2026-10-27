#include "wifi_native.h"

#include "native_command.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <map>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace hardware_test {
namespace {

/* 对网页返回的字符串进行 JSON 转义，避免原生输出破坏 JSON 格式。 */
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

/* 把 wpa_cli 的 key=value 输出读取为键值表。 */
std::map<std::string, std::string> parse_key_values(const std::string &output)
{
    std::map<std::string, std::string> values;
    std::istringstream stream(output);
    std::string line;
    while (std::getline(stream, line)) {
        const size_t equal = line.find('=');
        if (equal == std::string::npos) {
            continue;
        }
        values[line.substr(0, equal)] = line.substr(equal + 1U);
    }
    return values;
}

/* 去除命令输出中的行尾空白，便于解析 wpa_cli 返回值。 */
std::string trim(const std::string &value)
{
    const char *spaces = " \t\r\n";
    const size_t begin = value.find_first_not_of(spaces);
    if (begin == std::string::npos) {
        return "";
    }
    const size_t end = value.find_last_not_of(spaces);
    return value.substr(begin, end - begin + 1U);
}

/* 把用户输入转换为 wpa_cli 的带引号值，防止引号或反斜杠破坏参数。 */
std::string wpa_quoted(const std::string &value)
{
    std::string escaped;
    escaped.reserve(value.size() + 2U);
    escaped.push_back('"');
    for (char ch : value) {
        if (ch == '\\' || ch == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

/* 校验网络 ID，确保它只能作为十进制 wpa_supplicant 网络编号。 */
bool valid_network_id(const std::string &network_id)
{
    return !network_id.empty() &&
           std::all_of(network_id.begin(), network_id.end(), [](unsigned char ch) {
               return std::isdigit(ch) != 0;
           });
}

/* 校验 SSID 长度；wpa_supplicant 的 SSID 最大长度为 32 字节。 */
bool valid_ssid(const std::string &ssid)
{
    return !ssid.empty() && ssid.size() <= 32U;
}

/* 校验 WPA-PSK 密码长度，避免把明确无效的配置写入板端。 */
bool valid_password(const std::string &password)
{
    return password.size() >= 8U && password.size() <= 63U;
}

/* 从 Linux 原生无线统计文件读取当前网卡的信号强度。 */
int parse_wireless_signal(const std::string &output, const std::string &interface_name)
{
    std::istringstream stream(output);
    std::string line;
    const std::string prefix = interface_name + ":";
    while (std::getline(stream, line)) {
        const size_t begin = line.find(prefix);
        if (begin == std::string::npos) {
            continue;
        }
        std::istringstream fields(line.substr(begin + prefix.size()));
        std::string status;
        std::string quality;
        std::string level;
        if (!(fields >> status >> quality >> level)) {
            return 0;
        }
        return static_cast<int>(std::strtol(level.c_str(), nullptr, 10));
    }
    return 0;
}

/* 生成单个成功结果，所有数据都来自刚刚完成的原生操作。 */
WifiResult success(const std::string &data_json)
{
    WifiResult result;
    result.ok = true;
    result.http_status = 200;
    result.code = "OK";
    result.message = "success";
    result.data_json = data_json;
    return result;
}

} // namespace

NativeWifi::NativeWifi(std::string interface_name, std::string control_path)
    : interface_name_(std::move(interface_name)), control_path_(std::move(control_path))
{
}

WifiResult NativeWifi::command_failure(const std::string &code,
                                       const std::string &message,
                                       const CommandResult &command) const
{
    WifiResult result;
    result.ok = false;
    result.http_status = command.timed_out ? 504 : 502;
    result.code = command.timed_out ? "NATIVE_COMMAND_TIMEOUT" : code;
    result.message = command.timed_out ? "native command timeout" : message;
    result.native_exit_code = command.exit_code;
    result.native_output = command.output;
    return result;
}

WifiResult NativeWifi::run_wpa(const std::vector<std::string> &arguments,
                               int timeout_ms)
{
    std::vector<std::string> command;
    command.reserve(arguments.size() + 3U);
    command.push_back("wpa_cli");
    command.push_back("-p");
    command.push_back(control_path_);
    command.push_back("-i");
    command.push_back(interface_name_);
    command.insert(command.end(), arguments.begin(), arguments.end());

    CommandResult native = run_native_command(command, timeout_ms);
    if (!native.started || native.exit_code != 0 || native.timed_out ||
        trim(native.output).find("FAIL") != std::string::npos) {
        return command_failure("WPA_CLI_FAILED", "wpa_cli command failed", native);
    }
    return success("{}");
}

WifiResult NativeWifi::status()
{
    std::lock_guard<std::mutex> lock(operation_mutex_);
    std::vector<std::string> command = {
        "wpa_cli", "-p", control_path_, "-i", interface_name_, "status"};
    CommandResult native = run_native_command(command, 5000);
    if (!native.started || native.exit_code != 0 || native.timed_out) {
        return command_failure("WPA_STATUS_FAILED", "read WiFi status failed", native);
    }

    const std::map<std::string, std::string> values = parse_key_values(native.output);
    std::vector<std::string> ip_command = {"ip", "-4", "addr", "show", "dev", interface_name_};
    CommandResult ip = run_native_command(ip_command, 3000);
    std::vector<std::string> route_command = {"ip", "route", "show", "dev", interface_name_};
    CommandResult route = run_native_command(route_command, 3000);
    std::vector<std::string> wireless_command = {"cat", "/proc/net/wireless"};
    CommandResult wireless = run_native_command(wireless_command, 3000);
    const int signal_dbm = parse_wireless_signal(wireless.output, interface_name_);

    std::string ip_output = ip.output;
    std::string route_output = route.output;
    std::ostringstream data;
    data << "{"
         << "\"interface\":\"" << json_escape(interface_name_) << "\","
         << "\"wpa_state\":\"" << json_escape(values.count("wpa_state") ? values.at("wpa_state") : "") << "\","
         << "\"ssid\":\"" << json_escape(values.count("ssid") ? values.at("ssid") : "") << "\","
         << "\"bssid\":\"" << json_escape(values.count("bssid") ? values.at("bssid") : "") << "\","
         << "\"signal_dbm\":" << signal_dbm << ","
         << "\"ip_address\":\"" << json_escape(values.count("ip_address") ? values.at("ip_address") : "") << "\","
         << "\"ip_command_output\":\"" << json_escape(ip_output) << "\","
         << "\"route_command_output\":\"" << json_escape(route_output) << "\""
         << "}";
    return success(data.str());
}

WifiResult NativeWifi::saved_networks()
{
    std::lock_guard<std::mutex> lock(operation_mutex_);
    std::vector<std::string> command = {
        "wpa_cli", "-p", control_path_, "-i", interface_name_, "list_networks"};
    CommandResult native = run_native_command(command, 5000);
    if (!native.started || native.exit_code != 0 || native.timed_out) {
        return command_failure("WPA_LIST_NETWORKS_FAILED", "read saved WiFi failed", native);
    }

    std::istringstream stream(native.output);
    std::string line;
    bool header_seen = false;
    std::ostringstream networks;
    networks << "[";
    bool first = true;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        if (!header_seen) {
            header_seen = true;
            continue;
        }

        std::vector<std::string> fields;
        size_t begin = 0U;
        while (begin <= line.size()) {
            size_t end = line.find('\t', begin);
            if (end == std::string::npos) {
                end = line.size();
            }
            fields.push_back(line.substr(begin, end - begin));
            if (end == line.size()) {
                break;
            }
            begin = end + 1U;
        }
        if (fields.size() < 4U) {
            continue;
        }
        if (!first) {
            networks << ',';
        }
        first = false;
        networks << "{\"network_id\":\"" << json_escape(fields[0])
                 << "\",\"ssid\":\"" << json_escape(fields[1])
                 << "\",\"bssid\":\"" << json_escape(fields[2])
                 << "\",\"flags\":\"" << json_escape(fields[3]) << "\"}";
    }
    networks << "]";
    return success("{\"networks\":" + networks.str() + "}");
}

WifiResult NativeWifi::scan()
{
    std::lock_guard<std::mutex> lock(operation_mutex_);

    /*
     * 扫描前只读取状态，不停止 wpa_supplicant、不关闭网卡、不执行
     * disconnect/reassociate/select_network，确保当前连接不会被本 Demo
     * 主动切断。扫描本身通过现有 wpa_supplicant 控制接口完成。
     */
    std::vector<std::string> status_command = {
        "wpa_cli", "-p", control_path_, "-i", interface_name_, "status"};
    CommandResult status_native = run_native_command(status_command, 5000);
    if (!status_native.started || status_native.exit_code != 0 || status_native.timed_out) {
        return command_failure("WPA_STATUS_FAILED", "read WiFi status before scan failed", status_native);
    }

    std::vector<std::string> scan_command = {
        "wpa_cli", "-p", control_path_, "-i", interface_name_, "scan"};
    CommandResult scan_native = run_native_command(scan_command, 5000);
    if (!scan_native.started || scan_native.exit_code != 0 || scan_native.timed_out ||
        trim(scan_native.output).find("FAIL") != std::string::npos) {
        return command_failure("WPA_SCAN_FAILED", "WiFi scan failed", scan_native);
    }

    CommandResult result_native;
    bool completed = false;
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::vector<std::string> results_command = {
            "wpa_cli", "-p", control_path_, "-i", interface_name_, "scan_results"};
        result_native = run_native_command(results_command, 5000);
        if (result_native.started && result_native.exit_code == 0 &&
            trim(result_native.output).find("FAIL") == std::string::npos) {
            completed = true;
            break;
        }
    }
    if (!completed) {
        return command_failure("WPA_SCAN_RESULTS_FAILED", "read WiFi scan results failed", result_native);
    }

    std::istringstream stream(result_native.output);
    std::string line;
    bool header_seen = false;
    std::ostringstream aps;
    aps << "[";
    bool first = true;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        if (!header_seen) {
            header_seen = true;
            continue;
        }
        std::vector<std::string> fields;
        size_t begin = 0U;
        while (begin <= line.size()) {
            size_t end = line.find('\t', begin);
            if (end == std::string::npos) {
                end = line.size();
            }
            fields.push_back(line.substr(begin, end - begin));
            if (end == line.size()) {
                break;
            }
            begin = end + 1U;
        }
        if (fields.size() < 5U) {
            continue;
        }
        if (!first) {
            aps << ',';
        }
        first = false;
        aps << "{\"bssid\":\"" << json_escape(fields[0])
            << "\",\"frequency_mhz\":\"" << json_escape(fields[1])
            << "\",\"signal_dbm\":\"" << json_escape(fields[2])
            << "\",\"flags\":\"" << json_escape(fields[3])
            << "\",\"ssid\":\"" << json_escape(fields[4]) << "\"}";
    }
    aps << "]";
    return success("{\"aps\":" + aps.str() + "}");
}

WifiResult NativeWifi::add_network(const std::string &ssid,
                                   const std::string &password,
                                   const std::string &auth,
                                   bool connect_now)
{
    std::unique_lock<std::mutex> lock(operation_mutex_);
    if (!valid_ssid(ssid)) {
        WifiResult result;
        result.http_status = 400;
        result.code = "INVALID_SSID";
        result.message = "SSID must contain 1 to 32 bytes";
        return result;
    }
    const bool open = auth == "OPEN" || auth == "open";
    if (!open && !valid_password(password)) {
        WifiResult result;
        result.http_status = 400;
        result.code = "INVALID_PASSWORD";
        result.message = "WPA password must contain 8 to 63 bytes";
        return result;
    }

    std::vector<std::string> add_command = {
        "wpa_cli", "-p", control_path_, "-i", interface_name_, "add_network"};
    CommandResult added = run_native_command(add_command, 5000);
    if (!added.started || added.exit_code != 0 || added.timed_out) {
        return command_failure("WPA_ADD_NETWORK_FAILED", "add WiFi network failed", added);
    }
    const std::string network_id = trim(added.output);
    if (!valid_network_id(network_id)) {
        return command_failure("WPA_INVALID_NETWORK_ID", "wpa_cli returned invalid network id", added);
    }

    auto set_network = [&](const std::string &key, const std::string &value) -> WifiResult {
        std::vector<std::string> command = {
            "wpa_cli", "-p", control_path_, "-i", interface_name_,
            "set_network", network_id, key, value};
        CommandResult native = run_native_command(command, 5000);
        if (!native.started || native.exit_code != 0 || native.timed_out ||
            trim(native.output).find("FAIL") != std::string::npos) {
            return command_failure("WPA_SET_NETWORK_FAILED", "set WiFi network failed", native);
        }
        return success("{}");
    };

    WifiResult result = set_network("ssid", wpa_quoted(ssid));
    if (!result.ok) {
        (void)run_wpa({"remove_network", network_id});
        return result;
    }
    if (open) {
        result = set_network("key_mgmt", "NONE");
    } else {
        result = set_network("key_mgmt", "WPA-PSK");
        if (result.ok) {
            result = set_network("psk", wpa_quoted(password));
        }
    }
    if (!result.ok) {
        (void)run_wpa({"remove_network", network_id});
        return result;
    }

    result = set_network("disabled", connect_now ? "0" : "1");
    if (!result.ok) {
        (void)run_wpa({"remove_network", network_id});
        return result;
    }
    if (connect_now) {
        result = run_wpa({"select_network", network_id}, 10000);
        if (!result.ok) {
            (void)run_wpa({"remove_network", network_id});
            return result;
        }
    }

    /* 让 wpa_supplicant 写入真实配置文件，并用 list_networks 做持久化确认。 */
    result = run_wpa({"set", "update_config", "1"});
    if (!result.ok) {
        return result;
    }
    result = run_wpa({"save_config"}, 10000);
    if (!result.ok) {
        return result;
    }
    /* 释放互斥锁后重新读取，避免同一线程重复加锁造成死锁。 */
    lock.unlock();
    result = saved_networks();
    if (!result.ok || result.data_json.find("\"network_id\":\"" + network_id + "\"") == std::string::npos) {
        WifiResult verify;
        verify.http_status = 500;
        verify.code = "WPA_PERSISTENCE_VERIFY_FAILED";
        verify.message = "saved WiFi verification failed";
        return verify;
    }
    return success("{\"network_id\":\"" + network_id + "\",\"persisted\":true}");
}

WifiResult NativeWifi::delete_network(const std::string &network_id)
{
    std::unique_lock<std::mutex> lock(operation_mutex_);
    if (!valid_network_id(network_id)) {
        WifiResult result;
        result.http_status = 400;
        result.code = "INVALID_NETWORK_ID";
        result.message = "network_id must be numeric";
        return result;
    }

    std::vector<std::string> status_command = {
        "wpa_cli", "-p", control_path_, "-i", interface_name_, "status"};
    CommandResult status_native = run_native_command(status_command, 5000);
    if (!status_native.started || status_native.exit_code != 0 || status_native.timed_out) {
        return command_failure("WPA_STATUS_FAILED", "read current WiFi before delete failed", status_native);
    }
    const std::map<std::string, std::string> status_values = parse_key_values(status_native.output);
    if (status_values.count("id") != 0U && status_values.at("id") == network_id &&
        status_values.count("wpa_state") != 0U && status_values.at("wpa_state") == "COMPLETED") {
        WifiResult result;
        result.http_status = 409;
        result.code = "ACTIVE_NETWORK_DELETE_BLOCKED";
        result.message = "disconnect the active WiFi before deleting it";
        return result;
    }

    WifiResult result = run_wpa({"remove_network", network_id});
    if (!result.ok) {
        return result;
    }
    result = run_wpa({"set", "update_config", "1"});
    if (!result.ok) {
        return result;
    }
    result = run_wpa({"save_config"}, 10000);
    if (!result.ok) {
        return result;
    }
    /* 释放互斥锁后重新读取，避免同一线程重复加锁造成死锁。 */
    lock.unlock();
    result = saved_networks();
    if (!result.ok || result.data_json.find("\"network_id\":\"" + network_id + "\"") != std::string::npos) {
        WifiResult verify;
        verify.http_status = 500;
        verify.code = "WPA_DELETE_VERIFY_FAILED";
        verify.message = "deleted WiFi verification failed";
        return verify;
    }
    return success("{\"network_id\":\"" + network_id + "\",\"persisted\":true}");
}

WifiResult NativeWifi::connect_network(const std::string &network_id)
{
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (!valid_network_id(network_id)) {
        WifiResult result;
        result.http_status = 400;
        result.code = "INVALID_NETWORK_ID";
        result.message = "network_id must be numeric";
        return result;
    }
    return run_wpa({"select_network", network_id}, 10000);
}

WifiResult NativeWifi::disconnect()
{
    std::lock_guard<std::mutex> lock(operation_mutex_);
    return run_wpa({"disconnect"}, 5000);
}

} // namespace hardware_test
