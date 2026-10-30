#include "http_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <sstream>
#include <thread>

namespace hardware_test {
namespace {

/* HTTP 状态码文本只覆盖本 Demo 会返回的状态。 */
const char *reason_phrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 504: return "Gateway Timeout";
    default: return "Error";
    }
}

/* 从内存缓冲区发送全部内容，避免短写导致浏览器收到不完整 JSON。 */
bool send_all(int fd, const std::string &data)
{
    size_t sent_total = 0U;
    while (sent_total < data.size()) {
        ssize_t sent = send(fd, data.data() + sent_total, data.size() - sent_total, 0);
        if (sent <= 0) {
            return false;
        }
        sent_total += static_cast<size_t>(sent);
    }
    return true;
}

/* 读取请求头结束位置，限制最大请求头长度避免异常请求占用内存。 */
bool read_until_header_end(int fd, std::string *buffer)
{
    char chunk[2048];
    while (buffer->find("\r\n\r\n") == std::string::npos) {
        ssize_t count = recv(fd, chunk, sizeof(chunk), 0);
        if (count <= 0) {
            return false;
        }
        buffer->append(chunk, static_cast<size_t>(count));
        if (buffer->size() > 32768U) {
            return false;
        }
    }
    return true;
}

/* 读取十进制 Content-Length，拒绝分块传输和超大请求体。 */
size_t parse_content_length(const std::string &headers)
{
    std::istringstream stream(headers);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.size() >= 15U && line.compare(0, 15U, "Content-Length:") == 0) {
            const std::string value = line.substr(15U);
            try {
                size_t parsed = std::stoul(value);
                return parsed <= 8U * 1024U * 1024U ? parsed : 8U * 1024U * 1024U + 1U;
            } catch (...) {
                return 65537U;
            }
        }
    }
    return 0U;
}

} // namespace

HttpServer::HttpServer(int port, HttpHandler handler, std::function<bool()> stop_requested)
    : port_(port), handler_(std::move(handler)), stop_requested_(std::move(stop_requested))
{
}

int HttpServer::run()
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        std::perror("socket");
        return 1;
    }

    int reuse = 1;
    (void)setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port_));
    if (bind(server_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
        listen(server_fd, 16) != 0) {
        std::perror("bind/listen");
        close(server_fd);
        return 1;
    }

    std::printf("hardware test demo: http://0.0.0.0:%d\n", port_);
    std::fflush(stdout);
    while (true) {
        /* 信号到达后，即使系统调用被自动重启，也要在下一轮 accept 前退出。 */
        if (stop_requested_ && stop_requested_()) {
            break;
        }
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) {
                /* 收到 SIGTERM 后让 accept 循环退出，触发业务对象的正常析构。 */
                if (stop_requested_ && stop_requested_()) {
                    break;
                }
                continue;
            }
            break;
        }
        std::thread([this, client_fd]() {
            handle_client(client_fd);
            close(client_fd);
        }).detach();
    }
    close(server_fd);
    return 0;
}

bool HttpServer::read_request(int client_fd, HttpRequest *request)
{
    std::string buffer;
    if (!read_until_header_end(client_fd, &buffer)) {
        return false;
    }
    const size_t header_end = buffer.find("\r\n\r\n");
    const std::string headers = buffer.substr(0U, header_end);
    const size_t content_length = parse_content_length(headers);
    if (content_length > 8U * 1024U * 1024U) {
        return false;
    }
    while (buffer.size() < header_end + 4U + content_length) {
        char chunk[2048];
        ssize_t count = recv(client_fd, chunk, sizeof(chunk), 0);
        if (count <= 0) {
            return false;
        }
        buffer.append(chunk, static_cast<size_t>(count));
    }

    std::istringstream header_stream(headers);
    std::string request_line;
    if (!std::getline(header_stream, request_line)) {
        return false;
    }
    if (!request_line.empty() && request_line.back() == '\r') {
        request_line.pop_back();
    }
    std::istringstream request_line_stream(request_line);
    std::string target;
    std::string version;
    if (!(request_line_stream >> request->method >> target >> version)) {
        return false;
    }
    if (target.empty() || target[0] != '/' || target.size() > 2048U) {
        return false;
    }
    const size_t query_begin = target.find('?');
    request->path = target.substr(0U, query_begin == std::string::npos ? target.size() : query_begin);
    request->body = buffer.substr(header_end + 4U, content_length);
    return true;
}

void HttpServer::send_response(int client_fd, const HttpResponse &response)
{
    std::ostringstream header;
    header << "HTTP/1.1 " << response.status << ' ' << reason_phrase(response.status) << "\r\n"
           << "Content-Type: " << response.content_type << "\r\n"
           << "Content-Length: " << response.body.size() << "\r\n"
           << "Cache-Control: no-store\r\n"
           << "Connection: close\r\n\r\n";
    (void)send_all(client_fd, header.str());
    (void)send_all(client_fd, response.body);
}

void HttpServer::handle_client(int client_fd)
{
    HttpRequest request;
    if (!read_request(client_fd, &request)) {
        send_response(client_fd, {400, "application/json; charset=utf-8", "{\"ok\":false,\"code\":\"BAD_REQUEST\"}"});
        return;
    }
    HttpResponse response = handler_(request);
    send_response(client_fd, response);
}

} // namespace hardware_test
