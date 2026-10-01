#include "ehal_imu.h"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
std::atomic<bool> running{true};
void stop(int) { running = false; }
std::string json(const ehal_imu_sample_t &s) {
  std::ostringstream o;
  o << "{\"ok\":true,\"accel\":{"
    << "\"x\":" << s.accel_x << ",\"y\":" << s.accel_y << ",\"z\":" << s.accel_z << "},"
    << "\"gyro\":{" << "\"x\":" << s.gyro_x << ",\"y\":" << s.gyro_y << ",\"z\":" << s.gyro_z << "},"
    << "\"temperature\":" << s.temperature << ",\"status0\":" << static_cast<int>(s.status0)
    << ",\"status1\":" << static_cast<int>(s.status1) << ",\"timestamp_ms\":" << s.timestamp_ms << "}";
  return o.str();
}
void reply(int fd, int code, const std::string &type, const std::string &body) {
  std::ostringstream h; h << "HTTP/1.1 " << code << (code == 200 ? " OK" : " Error") << "\r\nContent-Type: " << type
    << "\r\nContent-Length: " << body.size() << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" << body;
  const auto data = h.str(); (void)send(fd, data.data(), data.size(), 0);
}
void client(int fd, ehal_imu_t *imu, const std::string &root) {
  char buf[4096]{}; const ssize_t n = recv(fd, buf, sizeof(buf)-1, 0);
  if (n <= 0) { close(fd); return; }
  std::istringstream req(std::string(buf, static_cast<size_t>(n))); std::string method, path, version; req >> method >> path >> version;
  if (method == "GET" && (path == "/" || path == "/index.html")) {
    std::ifstream f(root + "/index.html"); std::stringstream body; body << f.rdbuf(); reply(fd, f ? 200 : 404, "text/html; charset=utf-8", body.str());
  } else if (method == "GET" && path == "/api/data") {
    ehal_imu_sample_t s{}; const auto r = ehal_imu_read_sample(imu, &s);
    reply(fd, r == EHAL_OK ? 200 : 503, "application/json", r == EHAL_OK ? json(s) : "{\"ok\":false,\"error\":\"imu_read_failed\"}");
  } else if (method == "POST" && path == "/api/wom") {
    const auto r = ehal_imu_enable_wake_on_motion(imu, 40, 4);
    reply(fd, r == EHAL_OK ? 200 : 503, "application/json", r == EHAL_OK ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"wom_failed\"}");
  } else reply(fd, 404, "text/plain; charset=utf-8", "Not Found\n");
  close(fd);
}
}
int main(int argc, char **argv) {
  int port = 8081; std::string root = "web";
  for (int i=1;i<argc;i++) { if (!std::strcmp(argv[i], "--port") && i+1<argc) port=std::atoi(argv[++i]); else if (!std::strcmp(argv[i], "--web-root") && i+1<argc) root=argv[++i]; }
  std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
  ehal_imu_config_t cfg{}; cfg.i2c_device = std::getenv("IMU_I2C_DEVICE"); cfg.i2c_address = std::getenv("IMU_I2C_ADDR") ? std::strtoul(std::getenv("IMU_I2C_ADDR"), nullptr, 0) : 0x6A; cfg.enable_gyro=1;
  ehal_imu_t *imu=nullptr; const auto ir=ehal_imu_create(&cfg, &imu); if (ir != EHAL_OK) { std::cerr << "IMU init failed: " << ir << '\n'; return 2; }
  int s=socket(AF_INET,SOCK_STREAM,0); int yes=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes)); sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_ANY); a.sin_port=htons(static_cast<uint16_t>(port));
  if (bind(s,reinterpret_cast<sockaddr*>(&a),sizeof(a))<0 || listen(s,8)<0) { std::cerr<<"HTTP bind failed: "<<std::strerror(errno)<<'\n'; ehal_imu_destroy(imu); return 3; }
  std::cout << "IMU web demo: http://<device-ip>:" << port << "/\n";
  while (running) { sockaddr_in ca{}; socklen_t cl=sizeof(ca); int c=accept(s,reinterpret_cast<sockaddr*>(&ca),&cl); if(c>=0) std::thread(client,c,imu,root).detach(); }
  close(s); ehal_imu_destroy(imu); return 0;
}
