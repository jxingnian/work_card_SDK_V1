#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char *LOCATION_HOST = "192.168.5.1";
constexpr uint16_t LOCATION_PORT = 8080;
constexpr const char *HTTP_REQUEST =
    "GET /location HTTP/1.1\r\n"
    "Host: 192.168.5.1\r\n"
    "Connection: close\r\n"
    "\r\n";

int get_location(char *buffer, size_t buffer_size)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::perror("socket");
        return -1;
    }

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(LOCATION_PORT);
    if (inet_pton(AF_INET, LOCATION_HOST, &server_addr.sin_addr) <= 0) {
        std::perror("inet_pton");
        close(sock);
        return -1;
    }

    if (connect(sock, reinterpret_cast<struct sockaddr*>(&server_addr),
                sizeof(server_addr)) < 0) {
        std::perror("connect");
        close(sock);
        return -1;
    }

    ssize_t sent = send(sock, HTTP_REQUEST, std::strlen(HTTP_REQUEST), 0);
    if (sent < 0) {
        std::perror("send");
        close(sock);
        return -1;
    }

    size_t total_received = 0;
    while (total_received < buffer_size - 1) {
        ssize_t received = recv(sock, buffer + total_received,
                               buffer_size - total_received - 1, 0);
        if (received <= 0) {
            break;
        }
        total_received += static_cast<size_t>(received);
    }
    buffer[total_received] = '\0';

    close(sock);

    // 查找 JSON 数据（HTTP body）
    const char *json_start = std::strstr(buffer, "\r\n\r\n");
    if (json_start) {
        json_start += 4;
        std::memmove(buffer, json_start, std::strlen(json_start) + 1);
    }

    return 0;
}

} // namespace

int main()
{
    char buffer[4096];

    std::printf("正在获取定位数据...\n");

    if (get_location(buffer, sizeof(buffer)) != 0) {
        std::fprintf(stderr, "获取定位失败\n");
        return 1;
    }

    std::printf("\n定位数据:\n%s\n", buffer);
    return 0;
}
