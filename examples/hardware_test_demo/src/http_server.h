#ifndef HARDWARE_TEST_HTTP_SERVER_H
#define HARDWARE_TEST_HTTP_SERVER_H

#include <functional>
#include <string>

namespace hardware_test {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

struct HttpResponse {
    int status = 500;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
};

using HttpHandler = std::function<HttpResponse(const HttpRequest &)>;

/*
 * 交付包内置的最小 HTTP 服务。
 * 它只负责 TCP、HTTP 请求解析和线程调度，不知道任何硬件细节，硬件操作
 * 全部由业务处理器显式调用对应原生模块完成。
 */
class HttpServer {
public:
    HttpServer(int port, HttpHandler handler);

    int run();

private:
    void handle_client(int client_fd);
    bool read_request(int client_fd, HttpRequest *request);
    void send_response(int client_fd, const HttpResponse &response);

    int port_;
    HttpHandler handler_;
};

} // namespace hardware_test

#endif
