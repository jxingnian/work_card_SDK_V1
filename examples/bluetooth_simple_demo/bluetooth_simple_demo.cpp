/*
 * 蓝牙设备管理演示程序 - 底层实现版本
 * 直接使用 bluetoothctl 命令，不依赖 ehal_bluetooth 封装库
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;

void on_signal(int)
{
    g_stop = 1;
}

// ========== 工具函数 ==========

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

// ========== bluetoothctl 命令封装 ==========

struct BluetoothDevice {
    std::string address;
    std::string name;
    int rssi = 0;
    bool paired = false;
    bool connected = false;
};

std::string run_command(const std::string &command)
{
    std::array<char, 512> buffer{};
    std::string result;
    FILE *pipe = popen((command + " 2>&1").c_str(), "r");
    if (pipe == nullptr) {
        return "";
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        result += buffer.data();
    }
    pclose(pipe);
    return result;
}

std::string run_bluetoothctl(const std::string &args, int timeout_sec = 3)
{
    std::string cmd = "timeout " + std::to_string(timeout_sec + 3) +
                      " bluetoothctl --timeout " + std::to_string(timeout_sec) +
                      " " + args;
    return run_command(cmd);
}

std::vector<BluetoothDevice> parse_devices_output(const std::string &output)
{
    std::vector<BluetoothDevice> devices;
    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.find("Device ") == 0) {
            BluetoothDevice dev;
            size_t addr_start = 7; // "Device " 长度
            size_t addr_end = line.find(' ', addr_start);
            if (addr_end != std::string::npos) {
                dev.address = line.substr(addr_start, addr_end - addr_start);
                dev.name = trim(line.substr(addr_end + 1));
                if (dev.name.empty()) {
                    dev.name = dev.address;
                }
                devices.push_back(dev);
            }
        }
    }
    return devices;
}

bool parse_device_info(const std::string &info_output, BluetoothDevice &dev)
{
    std::istringstream stream(info_output);
    std::string line;

    while (std::getline(stream, line)) {
        line = trim(line);
        if (line.find("Name:") == 0) {
            dev.name = trim(line.substr(5));
        } else if (line.find("Alias:") == 0 && dev.name.empty()) {
            dev.name = trim(line.substr(6));
        } else if (line.find("Paired:") == 0) {
            dev.paired = (line.find("yes") != std::string::npos);
        } else if (line.find("Connected:") == 0) {
            dev.connected = (line.find("yes") != std::string::npos);
        } else if (line.find("RSSI:") == 0) {
            std::string rssi_str = trim(line.substr(5));
            try {
                dev.rssi = std::stoi(rssi_str);
            } catch (...) {
                dev.rssi = 0;
            }
        }
    }
    return true;
}

// ========== 蓝牙管理类 ==========

class BluetoothManager {
public:
    BluetoothManager()
        : agent_pid_(-1), agent_write_fd_(-1), scan_running_(false)
    {
    }

    ~BluetoothManager()
    {
        stop_scan();
        stop_agent();
    }

    bool initialize()
    {
        // 检查 bluetoothctl 是否可用
        std::string version = run_command("command -v bluetoothctl");
        if (version.empty()) {
            std::fprintf(stderr, "错误: bluetoothctl 未安装\n");
            return false;
        }

        // 启动配对代理
        return start_agent();
    }

    bool start_agent()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (agent_pid_ > 0 && agent_write_fd_ >= 0) {
            return true;
        }

        int input_pipe[2] = {-1, -1};
        if (pipe(input_pipe) != 0) {
            std::perror("pipe");
            return false;
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(input_pipe[0]);
            close(input_pipe[1]);
            std::perror("fork");
            return false;
        }

        if (pid == 0) {
            // 子进程：运行持久化的 bluetoothctl
            close(input_pipe[1]);
            dup2(input_pipe[0], STDIN_FILENO);
            close(input_pipe[0]);

            int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd >= 0) {
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
                close(null_fd);
            }

            execlp("bluetoothctl", "bluetoothctl", static_cast<char *>(nullptr));
            _exit(127);
        }

        // 父进程
        close(input_pipe[0]);
        agent_pid_ = pid;
        agent_write_fd_ = input_pipe[1];

        // 初始化代理
        const char *init_cmds = "agent NoInputNoOutput\ndefault-agent\npairable on\n";
        if (write(agent_write_fd_, init_cmds, strlen(init_cmds)) < 0) {
            stop_agent();
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::printf("✓ 配对代理已启动\n");
        return true;
    }

    void stop_agent()
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (agent_write_fd_ >= 0) {
            close(agent_write_fd_);
            agent_write_fd_ = -1;
        }

        if (agent_pid_ > 0) {
            int status = 0;
            if (waitpid(agent_pid_, &status, WNOHANG) == 0) {
                kill(agent_pid_, SIGTERM);
                waitpid(agent_pid_, &status, 0);
            }
            agent_pid_ = -1;
        }
    }

    bool start_scan()
    {
        if (scan_running_) {
            return true;
        }

        std::string result = run_bluetoothctl("scan on", 2);
        scan_running_ = true;
        std::printf("✓ 开始扫描蓝牙设备\n");
        return true;
    }

    bool stop_scan()
    {
        if (!scan_running_) {
            return true;
        }

        std::string result = run_bluetoothctl("scan off", 2);
        scan_running_ = false;
        std::printf("✓ 停止扫描\n");
        return true;
    }

    std::vector<BluetoothDevice> get_devices()
    {
        std::vector<BluetoothDevice> all_devices;

        // 获取所有设备
        std::string devices_output = run_bluetoothctl("devices", 1);
        std::vector<BluetoothDevice> devices = parse_devices_output(devices_output);

        // 获取已配对设备
        std::string paired_output = run_bluetoothctl("devices Paired", 1);
        std::vector<BluetoothDevice> paired = parse_devices_output(paired_output);

        // 为每个设备获取详细信息
        for (auto &dev : devices) {
            std::string info = run_bluetoothctl("info " + dev.address, 1);
            parse_device_info(info, dev);

            // 检查是否在已配对列表中
            for (const auto &p : paired) {
                if (p.address == dev.address) {
                    dev.paired = true;
                    break;
                }
            }

            all_devices.push_back(dev);
        }

        return all_devices;
    }

    bool pair_device(const std::string &address)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (agent_pid_ <= 0 || agent_write_fd_ < 0) {
            std::fprintf(stderr, "错误: 配对代理未运行\n");
            return false;
        }

        std::string cmd = "pair " + address + "\n";
        if (write(agent_write_fd_, cmd.c_str(), cmd.size()) < 0) {
            std::perror("write pair command");
            return false;
        }

        // 等待配对完成
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::string info = run_bluetoothctl("info " + address, 1);
            if (info.find("Paired: yes") != std::string::npos) {
                std::printf("✓ 配对成功: %s\n", address.c_str());
                return true;
            }
        }

        std::fprintf(stderr, "✗ 配对超时: %s\n", address.c_str());
        return false;
    }

    bool connect_device(const std::string &address)
    {
        std::string result = run_bluetoothctl("connect " + address, 5);
        bool success = (result.find("Connection successful") != std::string::npos ||
                       result.find("Connected: yes") != std::string::npos);
        if (success) {
            std::printf("✓ 连接成功: %s\n", address.c_str());
        } else {
            std::fprintf(stderr, "✗ 连接失败: %s\n", address.c_str());
        }
        return success;
    }

    bool disconnect_device(const std::string &address)
    {
        std::string result = run_bluetoothctl("disconnect " + address, 3);
        std::printf("✓ 断开连接: %s\n", address.c_str());
        return true;
    }

    bool remove_device(const std::string &address)
    {
        std::string result = run_bluetoothctl("remove " + address, 3);
        bool success = (result.find("Device has been removed") != std::string::npos ||
                       result.find("not available") == std::string::npos);
        if (success) {
            std::printf("✓ 移除设备: %s\n", address.c_str());
        }
        return success;
    }

    bool set_trusted(const std::string &address, bool trusted)
    {
        std::string cmd = trusted ? "trust " : "untrust ";
        std::string result = run_bluetoothctl(cmd + address, 2);
        return true;
    }

private:
    std::mutex mutex_;
    pid_t agent_pid_;
    int agent_write_fd_;
    bool scan_running_;
};

// ========== HTTP 服务器 ==========

class BluetoothSimpleDemo {
public:
    BluetoothSimpleDemo(int port, std::string web_root)
        : port_(port), web_root_(std::move(web_root))
    {
    }

    int initialize()
    {
        if (!bt_mgr_.initialize()) {
            std::fprintf(stderr, "蓝牙管理器初始化失败\n");
            return 1;
        }
        return 0;
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
    std::string result_json(bool ok, const std::string &error = "", const std::string &extra = "")
    {
        std::ostringstream out;
        out << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"error\":\"" << json_escape(error) << "\"";
        if (!extra.empty()) {
            out << ',' << extra;
        }
        out << '}';
        return out.str();
    }

    std::string devices_json()
    {
        std::vector<BluetoothDevice> devices = bt_mgr_.get_devices();

        std::ostringstream out;
        out << "[";
        for (size_t i = 0; i < devices.size(); ++i) {
            if (i != 0) {
                out << ',';
            }
            out << "{\"address\":\"" << json_escape(devices[i].address)
                << "\",\"name\":\"" << json_escape(devices[i].name)
                << "\",\"rssi\":" << devices[i].rssi
                << ",\"paired\":" << (devices[i].paired ? "true" : "false")
                << ",\"connected\":" << (devices[i].connected ? "true" : "false") << '}';
        }
        out << "]";

        return result_json(true, "", "\"devices\":" + out.str());
    }

    std::string action_json(const std::map<std::string, std::string> &query)
    {
        const std::string action = value_or(query, "action");
        const std::string address = value_or(query, "address");
        bool success = false;

        if (action == "start_scan") {
            success = bt_mgr_.start_scan();
        } else if (action == "stop_scan") {
            success = bt_mgr_.stop_scan();
        } else if (action == "pair") {
            success = bt_mgr_.pair_device(address);
        } else if (action == "connect") {
            success = bt_mgr_.connect_device(address);
        } else if (action == "disconnect") {
            success = bt_mgr_.disconnect_device(address);
        } else if (action == "remove") {
            success = bt_mgr_.remove_device(address);
        } else if (action == "pair_and_connect") {
            success = bt_mgr_.pair_device(address);
            if (success) {
                bt_mgr_.set_trusted(address, true);
                success = bt_mgr_.connect_device(address);
            }
        }

        return result_json(success, success ? "" : "操作失败");
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
    BluetoothManager bt_mgr_;
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
    if (ret != 0) {
        return 1;
    }
    return demo.run();
}
