#include "network_native.h"

#include "native_command.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace hardware_test {
namespace {

/* 4G 测速的默认下载资源；网页和后端同时保留该默认值，避免绕过网页直接调用 API 时地址为空。 */
constexpr const char *kDefaultSpeedUrl = "http://43.165.125.21/flash_download_tool.zip";

/* 统一转义 JSON 字符串，避免网卡名称、命令输出破坏响应格式。 */
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
        default: output << static_cast<char>(ch); break;
        }
    }
    return output.str();
}

NetworkResult success(const std::string &data)
{
    NetworkResult result;
    result.ok = true;
    result.http_status = 200;
    result.code = "OK";
    result.message = "操作成功";
    result.data_json = data;
    return result;
}

NetworkResult failure(int status, const std::string &code, const std::string &message)
{
    NetworkResult result;
    result.ok = false;
    result.http_status = status;
    result.code = code;
    result.message = message;
    return result;
}

std::string read_text_file(const std::string &path)
{
    std::ifstream input(path);
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

std::string trim(std::string value)
{
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    size_t first = 0;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t' || value[first] == '\n' || value[first] == '\r')) ++first;
    return value.substr(first);
}

/*
 * 从用户输入的主机名、IPv4 地址或 HTTP/HTTPS 网站地址中提取 ping 目标。
 * ping 只接受主机名/IP，不把整段 URL 传给系统命令，避免得到无效测试结果。
 */
std::string ping_host_from_target(std::string target)
{
    target = trim(std::move(target));
    const size_t scheme = target.find("://");
    if (scheme != std::string::npos) target = target.substr(scheme + 3U);
    const size_t path = target.find('/');
    if (path != std::string::npos) target.erase(path);
    const size_t port = target.rfind(':');
    if (port != std::string::npos && target.find(']') == std::string::npos) target.erase(port);
    return trim(std::move(target));
}

std::string command_output(const std::vector<std::string> &arguments)
{
    const CommandResult result = run_native_command(arguments, 3000, 32768U);
    return trim(result.output);
}

std::string sysfs_value(const std::string &interface_name, const std::string &name)
{
    return trim(read_text_file("/sys/class/net/" + interface_name + "/" + name));
}

/* 从系统路由表读取默认网关，确认 4G 流量经过 CH390D 以太网口。 */
std::string default_gateway(const std::string &interface_name)
{
    std::ifstream input("/proc/net/route");
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string iface;
        std::string destination;
        std::string gateway;
        std::string flags;
        if (!(row >> iface >> destination >> gateway >> flags) || iface != interface_name || destination != "00000000") continue;
        unsigned long value = 0;
        if (std::sscanf(gateway.c_str(), "%lx", &value) != 1) return "";
        struct in_addr address{};
        address.s_addr = static_cast<in_addr_t>(value);
        return inet_ntoa(address);
    }
    return "";
}

/* 通过 ip 命令读取 IPv4 地址，保持对板端 BusyBox 环境的兼容。 */
std::string interface_ipv4(const std::string &interface_name)
{
    const std::string output = command_output({"ip", "-4", "addr", "show", "dev", interface_name});
    const size_t marker = output.find("inet ");
    if (marker == std::string::npos) return "";
    const size_t start = marker + 5U;
    const size_t end = output.find('/', start);
    return end == std::string::npos ? "" : output.substr(start, end - start);
}

/* 创建带超时的 TCP 连接，避免定位或测速服务器异常时卡住网页请求。 */
int connect_with_timeout(const std::string &host, uint16_t port, int timeout_seconds, const std::string &interface_name)
{
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *addresses = nullptr;
    const std::string port_text = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &addresses) != 0 || addresses == nullptr) return -1;

    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) { freeaddrinfo(addresses); return -1; }
    if (!interface_name.empty()) {
        (void)setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE, interface_name.c_str(), interface_name.size() + 1U);
    }
    const int flags = fcntl(socket_fd, F_GETFL, 0);
    (void)fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK);
    const int connected = connect(socket_fd, addresses->ai_addr, addresses->ai_addrlen);
    freeaddrinfo(addresses);
    if (connected != 0 && errno != EINPROGRESS) { close(socket_fd); return -1; }
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(socket_fd, &writable);
    struct timeval timeout{timeout_seconds, 0};
    if (select(socket_fd + 1, nullptr, &writable, nullptr, &timeout) <= 0) { close(socket_fd); return -1; }
    int error = 0;
    socklen_t error_size = sizeof(error);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error, &error_size) != 0 || error != 0) { close(socket_fd); return -1; }
    (void)fcntl(socket_fd, F_SETFL, flags);
    return socket_fd;
}

/* 请求 CH390D 的固定定位接口，并原样保留 GNSS/LBS 字段供前端分别显示。 */
NetworkResult request_location(const std::string &interface_name)
{
    const int socket_fd = connect_with_timeout("192.168.5.1", 8080, 5, interface_name);
    if (socket_fd < 0) return failure(504, "LOCATION_CONNECT_FAILED", "无法通过以太网连接 CH390D 定位接口 192.168.5.1:8080");
    const std::string request = "GET /location HTTP/1.1\r\nHost: 192.168.5.1\r\nConnection: close\r\n\r\n";
    if (send(socket_fd, request.data(), request.size(), 0) < 0) { close(socket_fd); return failure(502, "LOCATION_SEND_FAILED", "定位请求发送失败"); }
    std::string response;
    char buffer[2048];
    while (response.size() < 65536U) {
        const ssize_t count = recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        response.append(buffer, static_cast<size_t>(count));
    }
    close(socket_fd);
    const size_t body_start = response.find("\r\n\r\n");
    if (body_start == std::string::npos) return failure(502, "LOCATION_INVALID_RESPONSE", "CH390D 返回的定位响应不完整");
    const std::string body = trim(response.substr(body_start + 4U));
    if (body.find("\"gnss\"") == std::string::npos || body.find("\"lbs\"") == std::string::npos) return failure(502, "LOCATION_INVALID_JSON", "CH390D 定位响应缺少 gnss 或 lbs 字段");
    return success(body);
}

} // namespace

NativeNetwork::NativeNetwork(std::string interface_name)
    : interface_name_(std::move(interface_name))
{
}

NativeNetwork::~NativeNetwork()
{
    speed_cancel_.store(true);
    if (speed_thread_.joinable()) speed_thread_.join();
}

NetworkResult NativeNetwork::ethernet_status() const
{
    const std::string path = "/sys/class/net/" + interface_name_;
    struct stat info{};
    if (stat(path.c_str(), &info) != 0) return failure(404, "ETHERNET_NOT_FOUND", "以太网接口 " + interface_name_ + " 不存在");
    const std::string carrier = sysfs_value(interface_name_, "carrier");
    const std::string state = sysfs_value(interface_name_, "operstate");
    const std::string ip = interface_ipv4(interface_name_);
    const std::string gateway = default_gateway(interface_name_);
    const std::string dns = command_output({"sh", "-c", "awk '/^nameserver/{print $2; exit}' /etc/resolv.conf"});
    const std::string speed = sysfs_value(interface_name_, "speed");
    const std::string duplex = sysfs_value(interface_name_, "duplex");
    const bool link_up = carrier == "1" && (state == "up" || state == "unknown");
    std::ostringstream data;
    data << "{\"interface\":\"" << json_escape(interface_name_) << "\",\"link_up\":" << (link_up ? "true" : "false")
         << ",\"operstate\":\"" << json_escape(state) << "\",\"carrier\":\"" << json_escape(carrier)
         << "\",\"mac\":\"" << json_escape(sysfs_value(interface_name_, "address")) << "\",\"ipv4\":\"" << json_escape(ip)
         << "\",\"gateway\":\"" << json_escape(gateway) << "\",\"dns\":\"" << json_escape(dns)
         << "\",\"speed_mbps\":\"" << json_escape(speed) << "\",\"duplex\":\"" << json_escape(duplex)
         << "\",\"default_route\":" << (!gateway.empty() ? "true" : "false") << '}';
    return success(data.str());
}

NetworkResult NativeNetwork::ping_test(const std::string &target) const
{
    /* 允许输入网站 URL、域名或 IP，统一转换为 ping 可识别的主机目标。 */
    const std::string host = ping_host_from_target(target);
    if (host.empty()) return failure(400, "PING_TARGET_INVALID", "ping 目标不能为空");

    /* 使用固定参数执行 4 次 ICMP 测试，并绑定 CH390D 对应的网卡接口。 */
    const CommandResult command = run_native_command(
        {"ping", "-I", interface_name_, "-c", "4", "-W", "2", host}, 12000, 32768U);
    const std::string output = trim(command.output);
    if (!command.started) return failure(500, "PING_START_FAILED", "无法启动 ping 命令");
    if (command.timed_out) return failure(504, "PING_TIMEOUT", "ping 测试超时");
    if (command.exit_code != 0) {
        return failure(502, "PING_FAILED", "ping 测试失败：" + (output.empty() ? std::string("无命令输出") : output));
    }

    /* 返回原始命令输出和退出码，网页可以据此核对每次测试的实际结果。 */
    std::ostringstream data;
    data << "{\"target\":\"" << json_escape(host) << "\",\"interface\":\""
         << json_escape(interface_name_) << "\",\"exit_code\":" << command.exit_code
         << ",\"output\":\"" << json_escape(output) << "\"}";
    return success(data.str());
}

NetworkResult NativeNetwork::location() const
{
    return request_location(interface_name_);
}

NetworkResult NativeNetwork::start_speed_test(const std::string &url)
{
    /* 上一次任务已经结束但线程对象仍可 join，先回收线程再创建下一次任务。 */
    if (speed_thread_.joinable() && !speed_running_.load()) speed_thread_.join();
    std::lock_guard<std::mutex> lock(speed_mutex_);
    if (speed_running_.load()) return failure(409, "SPEED_TEST_RUNNING", "测速已经在运行");
    const std::string actual_url = url.empty() ? kDefaultSpeedUrl : url;
    if (actual_url.rfind("http://", 0) != 0) return failure(400, "SPEED_TEST_URL_INVALID", "测速地址必须使用 http://，并且必须是可从 CH390D 访问的地址");
    speed_cancel_.store(false);
    speed_running_.store(true);
    speed_status_json_ = "{\"running\":true,\"stage\":\"connecting\",\"message\":\"正在连接测速服务器\"}";
    speed_thread_ = std::thread(&NativeNetwork::speed_worker, this, actual_url);
    return success("{\"running\":true}");
}

NetworkResult NativeNetwork::stop_speed_test()
{
    speed_cancel_.store(true);
    return success("{\"stopping\":true}");
}

NetworkResult NativeNetwork::speed_status() const
{
    std::lock_guard<std::mutex> lock(speed_mutex_);
    return success(speed_status_json_);
}

/*
 * 后台下载测速任务。
 *
 * URL 采用 http://IPv4[:port]/path 形式，测试连接绑定到 eth0，实际测量的
 * 是 CH390D 以太网出口的下载吞吐量，而不是 WiFi 或其他默认出口。
 */
void NativeNetwork::speed_worker(std::string url)
{
    const size_t scheme_end = 7U;
    const size_t path_pos = url.find('/', scheme_end);
    const std::string host_port = url.substr(scheme_end, path_pos == std::string::npos ? std::string::npos : path_pos - scheme_end);
    const std::string path = path_pos == std::string::npos ? "/" : url.substr(path_pos);
    std::string host = host_port;
    uint16_t port = 80;
    const size_t colon = host_port.rfind(':');
    if (colon != std::string::npos) {
        host = host_port.substr(0, colon);
        try {
            const unsigned long parsed_port = std::stoul(host_port.substr(colon + 1U));
            if (parsed_port == 0U || parsed_port > 65535U) throw std::out_of_range("port");
            port = static_cast<uint16_t>(parsed_port);
        } catch (...) {
            std::lock_guard<std::mutex> lock(speed_mutex_);
            speed_status_json_ = "{\"running\":false,\"stage\":\"failed\",\"message\":\"测速地址端口无效\"}";
            speed_running_.store(false);
            return;
        }
    }
    const int socket_fd = connect_with_timeout(host, port, 8, interface_name_);
    if (socket_fd < 0) {
        std::lock_guard<std::mutex> lock(speed_mutex_);
        speed_status_json_ = "{\"running\":false,\"stage\":\"failed\",\"message\":\"测速服务器连接失败\"}";
        speed_running_.store(false);
        return;
    }
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    if (send(socket_fd, request.data(), request.size(), 0) < 0) { close(socket_fd); speed_running_.store(false); return; }
    const auto started = std::chrono::steady_clock::now();
    size_t bytes = 0;
    char buffer[8192];
    while (!speed_cancel_.load()) {
        const ssize_t count = recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        bytes += static_cast<size_t>(count);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const double mbps = seconds > 0.0 ? static_cast<double>(bytes) * 8.0 / seconds / 1000000.0 : 0.0;
        std::lock_guard<std::mutex> lock(speed_mutex_);
        std::ostringstream status;
        status << "{\"running\":true,\"stage\":\"downloading\",\"bytes\":" << bytes << ",\"mbps\":" << mbps << '}';
        speed_status_json_ = status.str();
    }
    close(socket_fd);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const double mbps = seconds > 0.0 ? static_cast<double>(bytes) * 8.0 / seconds / 1000000.0 : 0.0;
    std::lock_guard<std::mutex> lock(speed_mutex_);
    std::ostringstream status;
    status << "{\"running\":false,\"stage\":\"finished\",\"bytes\":" << bytes << ",\"duration_seconds\":" << seconds << ",\"download_mbps\":" << mbps << '}';
    speed_status_json_ = status.str();
    speed_running_.store(false);
}

} // namespace hardware_test
