#include "ehal_camera.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kDefaultPort = 8080;
constexpr size_t kMaxHttpHeaderSize = 16384U;
constexpr size_t kMaxHttpBodySize = 4096U;
constexpr char kWebSocketMagic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

volatile sig_atomic_t g_exit_requested = 0;

static void handle_signal(int signal_number)
{
    if (signal_number == SIGINT || signal_number == SIGTERM) {
        g_exit_requested = 1;
    }
}

struct WebSocketClient {
    explicit WebSocketClient(int socket_fd)
        : fd(socket_fd)
    {
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000;
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }

    int fd;
    std::mutex send_mutex;
    std::atomic<bool> closed{false};
};

struct DemoConfig {
    std::string default_config_path = "configs/hal_default.json";
    std::string runtime_config_path = "configs/hal.json";
    std::string user_config_path;
    std::string pipeline_name = "video_record";
    std::string sensor_name = "sc2356";
    int channel = 0;
    uint32_t width = 1600U;
    uint32_t height = 1200U;
    uint32_t fps = 25U;
    uint32_t bitrate_kbps = 2048U;
    ehal_video_codec_t codec = EHAL_VIDEO_CODEC_H264;
    int port = kDefaultPort;
    std::string web_root = "web";
};

class CameraWebDemo {
public:
    CameraWebDemo()
    {
        camera_ = nullptr;
    }

    ~CameraWebDemo()
    {
        stop();
    }

    int initialize(const DemoConfig &config)
    {
        std::lock_guard<std::mutex> lock(camera_mutex_);
        config_ = config;
        int result = ehal_camera_create(&camera_);
        if (result != EHAL_OK) {
            camera_ = nullptr;
            return result;
        }
        return configure_locked(config_);
    }

    int start()
    {
        std::lock_guard<std::mutex> lock(camera_mutex_);
        if (camera_ == nullptr) {
            return EHAL_ERR_STATE;
        }
        int result = ehal_camera_start(camera_);
        if (result == EHAL_OK) {
            running_ = true;
        }
        return result;
    }

    int reconfigure(const std::map<std::string, std::string> &parameters)
    {
        DemoConfig next_config;
        {
            std::lock_guard<std::mutex> lock(camera_mutex_);
            next_config = config_;
        }

        if (!assign_config(parameters, &next_config)) {
            return EHAL_ERR_PARAM;
        }

        std::lock_guard<std::mutex> lock(camera_mutex_);
        if (camera_ == nullptr) {
            return EHAL_ERR_STATE;
        }
        bool was_running = running_;
        if (was_running) {
            int stop_result = ehal_camera_stop(camera_);
            if (stop_result != EHAL_OK) {
                return stop_result;
            }
            running_ = false;
        }

        int configure_result = configure_locked(next_config);
        if (configure_result != EHAL_OK) {
            return configure_result;
        }
        config_ = next_config;

        if (was_running) {
            int start_result = ehal_camera_start(camera_);
            if (start_result != EHAL_OK) {
                return start_result;
            }
            running_ = true;
        }
        return EHAL_OK;
    }

    void run()
    {
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            std::cerr << "create web socket failed: " << std::strerror(errno) << '\n';
            return;
        }

        int reuse_address = 1;
        (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                         &reuse_address, sizeof(reuse_address));

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

        server_fd_ = server_fd;
        std::cout << "camera web demo listening on port " << config_.port << '\n';
        while (!g_exit_requested) {
            sockaddr_in client_address{};
            socklen_t client_length = sizeof(client_address);
            int client_fd = accept(server_fd,
                                   reinterpret_cast<sockaddr *>(&client_address),
                                   &client_length);
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (g_exit_requested) {
                    break;
                }
                continue;
            }
            std::thread(&CameraWebDemo::handle_client, this, client_fd).detach();
        }
        close(server_fd);
        server_fd_ = -1;
    }

    void stop()
    {
        int server_fd = server_fd_.exchange(-1);
        if (server_fd >= 0) {
            shutdown(server_fd, SHUT_RDWR);
            close(server_fd);
        }

        std::lock_guard<std::mutex> lock(camera_mutex_);
        if (camera_ != nullptr) {
            if (running_) {
                (void)ehal_camera_stop(camera_);
                running_ = false;
            }
            ehal_camera_destroy(camera_);
            camera_ = nullptr;
        }
    }

private:
    int configure_locked(const DemoConfig &config)
    {
        ehal_camera_config_t camera_config{};
        camera_config.default_config_path = config.default_config_path.c_str();
        camera_config.runtime_config_path = config.runtime_config_path.c_str();
        camera_config.user_config_path = config.user_config_path.empty() ?
            nullptr : config.user_config_path.c_str();
        camera_config.pipeline_name = config.pipeline_name.c_str();
        camera_config.sensor_name = config.sensor_name.c_str();
        camera_config.channel_count = 1U;
        camera_config.channels[0].channel = config.channel;
        camera_config.channels[0].width = config.width;
        camera_config.channels[0].height = config.height;
        camera_config.channels[0].fps = config.fps;
        camera_config.channels[0].bitrate_kbps = config.bitrate_kbps;
        camera_config.channels[0].codec = config.codec;
        camera_config.video_callback = on_video_frame;
        camera_config.user_data = this;
        return ehal_camera_configure(camera_, &camera_config);
    }

    static void on_video_frame(const ehal_video_frame_t *frame, void *user_data)
    {
        CameraWebDemo *demo = static_cast<CameraWebDemo *>(user_data);
        if (demo != nullptr && frame != nullptr && frame->data != nullptr && frame->size > 0U) {
            demo->broadcast_frame(*frame);
        }
    }

    void broadcast_frame(const ehal_video_frame_t &frame)
    {
        std::vector<std::shared_ptr<WebSocketClient>> clients;
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients = clients_;
        }

        bool key_frame = detect_key_frame(frame);
        for (const std::shared_ptr<WebSocketClient> &client : clients) {
            if (!send_binary_frame(client, frame.data, frame.size)) {
                remove_client(client);
            }
        }
        last_key_frame_.store(key_frame, std::memory_order_relaxed);
        frame_count_.fetch_add(1U, std::memory_order_relaxed);
    }

    bool detect_key_frame(const ehal_video_frame_t &frame) const
    {
        const uint8_t *data = frame.data;
        for (uint32_t index = 0U; index + 4U < frame.size; ++index) {
            size_t start_code_size = 0U;
            if (data[index] == 0U && data[index + 1U] == 0U &&
                data[index + 2U] == 1U) {
                start_code_size = 3U;
            } else if (index + 4U < frame.size && data[index] == 0U &&
                       data[index + 1U] == 0U && data[index + 2U] == 0U &&
                       data[index + 3U] == 1U) {
                start_code_size = 4U;
            }
            if (start_code_size == 0U || index + start_code_size >= frame.size) {
                continue;
            }
            uint8_t nal_header = data[index + start_code_size];
            if (frame.codec == EHAL_VIDEO_CODEC_H264 && (nal_header & 0x1FU) == 5U) {
                return true;
            }
            if (frame.codec == EHAL_VIDEO_CODEC_H265 &&
                (((nal_header >> 1U) & 0x3FU) >= 19U) &&
                (((nal_header >> 1U) & 0x3FU) <= 21U)) {
                return true;
            }
        }
        return false;
    }

    static bool send_all(int socket_fd, const uint8_t *data, size_t size)
    {
        size_t sent_size = 0U;
        while (sent_size < size) {
            ssize_t result = send(socket_fd, data + sent_size, size - sent_size, MSG_NOSIGNAL);
            if (result <= 0) {
                return false;
            }
            sent_size += static_cast<size_t>(result);
        }
        return true;
    }

    static bool send_binary_frame(const std::shared_ptr<WebSocketClient> &client,
                                  const uint8_t *data,
                                  uint32_t size)
    {
        if (client == nullptr || client->closed.load() || data == nullptr || size == 0U) {
            return false;
        }

        std::vector<uint8_t> header;
        header.push_back(0x82U);
        if (size < 126U) {
            header.push_back(static_cast<uint8_t>(size));
        } else if (size <= 0xFFFFU) {
            header.push_back(126U);
            header.push_back(static_cast<uint8_t>((size >> 8U) & 0xFFU));
            header.push_back(static_cast<uint8_t>(size & 0xFFU));
        } else {
            header.push_back(127U);
            uint64_t payload_size = size;
            for (int shift = 56; shift >= 0; shift -= 8) {
                header.push_back(static_cast<uint8_t>((payload_size >> shift) & 0xFFU));
            }
        }

        std::lock_guard<std::mutex> lock(client->send_mutex);
        if (!send_all(client->fd, header.data(), header.size()) ||
            !send_all(client->fd, data, size)) {
            if (!client->closed.exchange(true)) {
                shutdown(client->fd, SHUT_RDWR);
                close(client->fd);
            }
            return false;
        }
        return true;
    }

    static bool send_text(int socket_fd, const std::string &text)
    {
        uint8_t header[10] = {};
        size_t header_size = 0U;
        header[0] = 0x81U;
        if (text.size() < 126U) {
            header[1] = static_cast<uint8_t>(text.size());
            header_size = 2U;
        } else {
            header[1] = 126U;
            header[2] = static_cast<uint8_t>((text.size() >> 8U) & 0xFFU);
            header[3] = static_cast<uint8_t>(text.size() & 0xFFU);
            header_size = 4U;
        }
        return send_all(socket_fd, header, header_size) &&
               send_all(socket_fd,
                        reinterpret_cast<const uint8_t *>(text.data()),
                        text.size());
    }

    void add_client(const std::shared_ptr<WebSocketClient> &client)
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.push_back(client);
    }

    void remove_client(const std::shared_ptr<WebSocketClient> &client)
    {
        if (client == nullptr) {
            return;
        }
        if (!client->closed.exchange(true)) {
            shutdown(client->fd, SHUT_RDWR);
            close(client->fd);
        }
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
    }

    static std::string base64_encode(const uint8_t *data, size_t size)
    {
        static const char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        result.reserve(((size + 2U) / 3U) * 4U);
        for (size_t index = 0U; index < size; index += 3U) {
            uint32_t value = static_cast<uint32_t>(data[index]) << 16U;
            if (index + 1U < size) {
                value |= static_cast<uint32_t>(data[index + 1U]) << 8U;
            }
            if (index + 2U < size) {
                value |= data[index + 2U];
            }
            result.push_back(alphabet[(value >> 18U) & 0x3FU]);
            result.push_back(alphabet[(value >> 12U) & 0x3FU]);
            result.push_back(index + 1U < size ? alphabet[(value >> 6U) & 0x3FU] : '=');
            result.push_back(index + 2U < size ? alphabet[value & 0x3FU] : '=');
        }
        return result;
    }

    static std::string websocket_accept_key(const std::string &key)
    {
        std::string source = key + kWebSocketMagic;
        uint8_t digest[SHA_DIGEST_LENGTH] = {};
        SHA1(reinterpret_cast<const uint8_t *>(source.data()), source.size(), digest);
        return base64_encode(digest, sizeof(digest));
    }

    static std::string trim(const std::string &value)
    {
        size_t begin = 0U;
        while (begin < value.size() &&
               (value[begin] == ' ' || value[begin] == '\t' ||
                value[begin] == '\r' || value[begin] == '\n')) {
            ++begin;
        }
        size_t end = value.size();
        while (end > begin &&
               (value[end - 1U] == ' ' || value[end - 1U] == '\t' ||
                value[end - 1U] == '\r' || value[end - 1U] == '\n')) {
            --end;
        }
        return value.substr(begin, end - begin);
    }

    static std::string lower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        return value;
    }

    static std::string url_decode(const std::string &value)
    {
        std::string result;
        for (size_t index = 0U; index < value.size(); ++index) {
            if (value[index] == '+') {
                result.push_back(' ');
            } else if (value[index] == '%' && index + 2U < value.size()) {
                unsigned int decoded = 0U;
                if (std::sscanf(value.substr(index + 1U, 2U).c_str(),
                                "%02x", &decoded) == 1) {
                    result.push_back(static_cast<char>(decoded));
                    index += 2U;
                } else {
                    result.push_back(value[index]);
                }
            } else {
                result.push_back(value[index]);
            }
        }
        return result;
    }

    static std::map<std::string, std::string> parse_form(const std::string &body)
    {
        std::map<std::string, std::string> result;
        size_t begin = 0U;
        while (begin < body.size()) {
            size_t end = body.find('&', begin);
            if (end == std::string::npos) {
                end = body.size();
            }
            std::string item = body.substr(begin, end - begin);
            size_t separator = item.find('=');
            if (separator != std::string::npos) {
                result[url_decode(item.substr(0U, separator))] =
                    url_decode(item.substr(separator + 1U));
            }
            begin = end + 1U;
        }
        return result;
    }

    static bool read_http_request(int socket_fd,
                                  std::string *header,
                                  std::string *body)
    {
        std::string request;
        char buffer[1024];
        size_t header_end = std::string::npos;
        while (request.size() < kMaxHttpHeaderSize) {
            ssize_t received = recv(socket_fd, buffer, sizeof(buffer), 0);
            if (received <= 0) {
                return false;
            }
            request.append(buffer, static_cast<size_t>(received));
            header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                break;
            }
        }
        if (header_end == std::string::npos) {
            return false;
        }

        *header = request.substr(0U, header_end + 4U);
        size_t body_begin = header_end + 4U;
        size_t content_length = 0U;
        std::istringstream header_stream(*header);
        std::string line;
        while (std::getline(header_stream, line)) {
            std::string lower_line = lower(line);
            if (lower_line.rfind("content-length:", 0U) == 0U) {
                content_length = static_cast<size_t>(
                    std::strtoul(trim(line.substr(15U)).c_str(), nullptr, 10));
            }
        }
        if (content_length > kMaxHttpBodySize) {
            return false;
        }
        while (request.size() - body_begin < content_length) {
            ssize_t received = recv(socket_fd, buffer, sizeof(buffer), 0);
            if (received <= 0) {
                return false;
            }
            request.append(buffer, static_cast<size_t>(received));
        }
        *body = request.substr(body_begin, content_length);
        return true;
    }

    static std::map<std::string, std::string> parse_headers(const std::string &header)
    {
        std::map<std::string, std::string> result;
        std::istringstream stream(header);
        std::string line;
        std::getline(stream, line);
        while (std::getline(stream, line)) {
            size_t separator = line.find(':');
            if (separator == std::string::npos) {
                continue;
            }
            result[lower(trim(line.substr(0U, separator)))] =
                trim(line.substr(separator + 1U));
        }
        return result;
    }

    static bool send_http_response(int socket_fd,
                                   int status,
                                   const std::string &content_type,
                                   const std::string &body)
    {
        std::ostringstream response;
        response << "HTTP/1.1 " << status << (status == 200 ? " OK" : " Error") << "\r\n"
                 << "Content-Type: " << content_type << "\r\n"
                 << "Content-Length: " << body.size() << "\r\n"
                 << "Connection: close\r\n\r\n";
        std::string header = response.str();
        return send_all(socket_fd,
                        reinterpret_cast<const uint8_t *>(header.data()), header.size()) &&
               send_all(socket_fd,
                        reinterpret_cast<const uint8_t *>(body.data()), body.size());
    }

    static std::string json_escape(const std::string &value)
    {
        std::string result;
        for (char character : value) {
            if (character == '\\' || character == '"') {
                result.push_back('\\');
            }
            result.push_back(character);
        }
        return result;
    }

    std::string status_json() const
    {
        ehal_camera_stats_t stats{};
        std::string codec_name;
        DemoConfig current_config;
        {
            std::lock_guard<std::mutex> lock(camera_mutex_);
            current_config = config_;
            if (camera_ != nullptr) {
                (void)ehal_camera_get_stats(camera_, &stats);
            }
        }
        codec_name = current_config.codec == EHAL_VIDEO_CODEC_H264 ? "H264" : "H265";
        bool running;
        {
            std::lock_guard<std::mutex> lock(camera_mutex_);
            running = running_;
        }
        std::ostringstream output;
        output << "{\"running\":" << (running ? "true" : "false")
               << ",\"codec\":\"" << codec_name << "\""
               << ",\"width\":" << current_config.width
               << ",\"height\":" << current_config.height
               << ",\"fps\":" << current_config.fps
               << ",\"bitrate_kbps\":" << current_config.bitrate_kbps
               << ",\"video_frames\":" << stats.video_frames
               << ",\"video_bytes\":" << stats.video_bytes
               << ",\"websocket_clients\":" << websocket_client_count()
               << "}";
        return output.str();
    }

    size_t websocket_client_count() const
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        return clients_.size();
    }

    void handle_websocket(int socket_fd, const std::map<std::string, std::string> &headers)
    {
        auto key_iterator = headers.find("sec-websocket-key");
        if (key_iterator == headers.end()) {
            close(socket_fd);
            return;
        }

        std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " +
            websocket_accept_key(key_iterator->second) + "\r\n\r\n";
        if (!send_all(socket_fd,
                      reinterpret_cast<const uint8_t *>(response.data()),
                      response.size())) {
            close(socket_fd);
            return;
        }

        std::shared_ptr<WebSocketClient> client = std::make_shared<WebSocketClient>(socket_fd);
        add_client(client);
        receive_websocket(client);
        remove_client(client);
    }

    static bool receive_exact(int socket_fd, uint8_t *data, size_t size)
    {
        size_t received_size = 0U;
        while (received_size < size) {
            ssize_t received = recv(socket_fd, data + received_size,
                                    size - received_size, 0);
            if (received <= 0) {
                return false;
            }
            received_size += static_cast<size_t>(received);
        }
        return true;
    }

    void receive_websocket(const std::shared_ptr<WebSocketClient> &client)
    {
        while (!g_exit_requested && client != nullptr && !client->closed.load()) {
            uint8_t frame_header[2] = {};
            if (!receive_exact(client->fd, frame_header, sizeof(frame_header))) {
                break;
            }
            uint8_t opcode = frame_header[0] & 0x0FU;
            bool masked = (frame_header[1] & 0x80U) != 0U;
            uint64_t payload_size = frame_header[1] & 0x7FU;
            if (payload_size == 126U) {
                uint8_t size_bytes[2] = {};
                if (!receive_exact(client->fd, size_bytes, sizeof(size_bytes))) {
                    break;
                }
                payload_size = (static_cast<uint64_t>(size_bytes[0]) << 8U) |
                               size_bytes[1];
            } else if (payload_size == 127U) {
                uint8_t size_bytes[8] = {};
                if (!receive_exact(client->fd, size_bytes, sizeof(size_bytes))) {
                    break;
                }
                payload_size = 0U;
                for (uint8_t size_byte : size_bytes) {
                    payload_size = (payload_size << 8U) | size_byte;
                }
            }
            if (payload_size > kMaxHttpBodySize || !masked) {
                break;
            }
            uint8_t mask[4] = {};
            if (!receive_exact(client->fd, mask, sizeof(mask))) {
                break;
            }
            std::vector<uint8_t> payload(static_cast<size_t>(payload_size));
            if (!payload.empty() && !receive_exact(client->fd, payload.data(), payload.size())) {
                break;
            }
            for (size_t index = 0U; index < payload.size(); ++index) {
                payload[index] ^= mask[index % 4U];
            }
            if (opcode == 0x8U) {
                break;
            }
            if (opcode == 0x9U) {
                std::lock_guard<std::mutex> lock(client->send_mutex);
                uint8_t pong_header[2] = {0x8AU, static_cast<uint8_t>(payload.size())};
                if (!send_all(client->fd, pong_header, sizeof(pong_header)) ||
                    (!payload.empty() && !send_all(client->fd, payload.data(), payload.size()))) {
                    break;
                }
            }
        }
    }

    void handle_client(int socket_fd)
    {
        std::string header;
        std::string body;
        if (!read_http_request(socket_fd, &header, &body)) {
            close(socket_fd);
            return;
        }

        std::istringstream request_stream(header);
        std::string method;
        std::string path;
        std::string version;
        request_stream >> method >> path >> version;
        std::map<std::string, std::string> headers = parse_headers(header);
        if (path == "/ws" && lower(headers["upgrade"]) == "websocket") {
            handle_websocket(socket_fd, headers);
            return;
        }

        if (method == "GET" && (path == "/" || path == "/index.html")) {
            send_file(socket_fd, config_.web_root + "/index.html", "text/html; charset=utf-8");
        } else if (method == "GET" && path == "/api/status") {
            send_http_response(socket_fd, 200, "application/json", status_json());
        } else if (method == "POST" && path == "/api/config") {
            int result = reconfigure(parse_form(body));
            std::ostringstream output;
            output << "{\"code\":" << result << ",\"message\":\""
                   << json_escape(ehal_camera_error_string(result)) << "\"}";
            send_http_response(socket_fd, result == EHAL_OK ? 200 : 400,
                               "application/json", output.str());
        } else if (method == "GET" && path == "/snapshot.jpg") {
            send_snapshot(socket_fd);
        } else {
            send_http_response(socket_fd, 404, "text/plain; charset=utf-8", "Not Found\n");
        }
        close(socket_fd);
    }

    static void send_file(int socket_fd,
                          const std::string &path,
                          const std::string &content_type)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input.good()) {
            send_http_response(socket_fd, 404, "text/plain; charset=utf-8",
                               "Web page not found\n");
            return;
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        send_http_response(socket_fd, 200, content_type, buffer.str());
    }

    void send_snapshot(int socket_fd)
    {
        const std::string path = "/tmp/camera_web_demo_snapshot.jpg";
        int result;
        {
            std::lock_guard<std::mutex> lock(camera_mutex_);
            result = camera_ == nullptr ? EHAL_ERR_STATE :
                ehal_camera_snapshot(camera_, config_.channel, path.c_str());
        }
        if (result != EHAL_OK) {
            send_http_response(socket_fd, 500, "text/plain; charset=utf-8",
                               ehal_camera_error_string(result));
            close(socket_fd);
            return;
        }
        send_file(socket_fd, path, "image/jpeg");
        (void)unlink(path.c_str());
    }

    static bool parse_unsigned(const std::map<std::string, std::string> &parameters,
                               const char *name,
                               uint32_t minimum,
                               uint32_t maximum,
                               uint32_t *value)
    {
        auto iterator = parameters.find(name);
        if (iterator == parameters.end()) {
            return true;
        }
        char *end = nullptr;
        unsigned long parsed = std::strtoul(iterator->second.c_str(), &end, 10);
        if (end == iterator->second.c_str() || *end != '\0' ||
            parsed < minimum || parsed > maximum) {
            return false;
        }
        *value = static_cast<uint32_t>(parsed);
        return true;
    }

    static bool assign_config(const std::map<std::string, std::string> &parameters,
                              DemoConfig *config)
    {
        if (config == nullptr) {
            return false;
        }
        auto assign_string = [&parameters](const char *name, std::string *value) {
            auto iterator = parameters.find(name);
            if (iterator != parameters.end()) {
                *value = iterator->second;
            }
        };
        assign_string("default_config_path", &config->default_config_path);
        assign_string("runtime_config_path", &config->runtime_config_path);
        assign_string("user_config_path", &config->user_config_path);
        assign_string("pipeline_name", &config->pipeline_name);
        assign_string("sensor_name", &config->sensor_name);

        uint32_t channel = static_cast<uint32_t>(config->channel);
        if (!parse_unsigned(parameters, "channel", 0U, 3U, &channel) ||
            !parse_unsigned(parameters, "width", 160U, 4096U, &config->width) ||
            !parse_unsigned(parameters, "height", 120U, 4096U, &config->height) ||
            !parse_unsigned(parameters, "fps", 1U, 60U, &config->fps) ||
            !parse_unsigned(parameters, "bitrate_kbps", 16U, 50000U,
                            &config->bitrate_kbps)) {
            return false;
        }
        config->channel = static_cast<int>(channel);
        auto codec_iterator = parameters.find("codec");
        if (codec_iterator != parameters.end()) {
            if (codec_iterator->second == "H264") {
                config->codec = EHAL_VIDEO_CODEC_H264;
            } else if (codec_iterator->second == "H265") {
                config->codec = EHAL_VIDEO_CODEC_H265;
            } else {
                return false;
            }
        }
        return config->sensor_name == "sc2356" &&
               !config->pipeline_name.empty() &&
               !config->default_config_path.empty() &&
               !config->runtime_config_path.empty();
    }

    ehal_camera_t *camera_ = nullptr;
    DemoConfig config_;
    mutable std::mutex camera_mutex_;
    bool running_ = false;
    std::atomic<int> server_fd_{-1};
    mutable std::mutex clients_mutex_;
    std::vector<std::shared_ptr<WebSocketClient>> clients_;
    std::atomic<uint64_t> frame_count_{0U};
    std::atomic<bool> last_key_frame_{false};
};

static bool parse_integer(const std::string &text, int *value)
{
    char *end = nullptr;
    long parsed = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || parsed < 1L || parsed > 65535L) {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

static DemoConfig parse_arguments(int argc, char **argv)
{
    DemoConfig config;
    for (int index = 1; index < argc; ++index) {
        std::string argument = argv[index];
        auto next_value = [&index, argc, argv]() -> std::string {
            return index + 1 < argc ? argv[++index] : std::string();
        };
        if (argument == "--port") {
            int port = config.port;
            std::string value = next_value();
            if (parse_integer(value, &port)) {
                config.port = port;
            }
        } else if (argument == "--web-root") {
            config.web_root = next_value();
        } else if (argument == "--default") {
            config.default_config_path = next_value();
        } else if (argument == "--runtime") {
            config.runtime_config_path = next_value();
        } else if (argument == "--user") {
            config.user_config_path = next_value();
        }
    }
    return config;
}

} // namespace

int main(int argc, char **argv)
{
    DemoConfig config = parse_arguments(argc, argv);
    ::signal(SIGINT, handle_signal);
    ::signal(SIGTERM, handle_signal);

    CameraWebDemo demo;
    int result = demo.initialize(config);
    if (result != EHAL_OK) {
        std::cerr << "camera configure failed: "
                  << ehal_camera_error_string(result) << '\n';
        return 1;
    }
    result = demo.start();
    if (result != EHAL_OK) {
        std::cerr << "camera start failed: "
                  << ehal_camera_error_string(result) << '\n';
        return 1;
    }

    demo.run();
    return 0;
}
