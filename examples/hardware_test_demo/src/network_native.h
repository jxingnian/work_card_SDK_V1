#ifndef HARDWARE_TEST_NETWORK_NATIVE_H
#define HARDWARE_TEST_NETWORK_NATIVE_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace hardware_test {

/*
 * 4G/以太网测试结果的统一表示。
 *
 * 该结构把底层命令、CH390D HTTP 接口和测速任务的结果统一转换成
 * HTTP 层可以直接返回的 JSON，页面因此可以显示明确的错误阶段。
 */
struct NetworkResult {
    bool ok = false;
    int http_status = 500;
    std::string code;
    std::string message;
    std::string data_json = "{}";
};

/*
 * 独立 Demo 的 4G/以太网测试服务。
 *
 * 该类只负责读取以太网状态、请求 CH390D 定位接口和执行 HTTP 下载测速，
 * 不修改网卡配置、不切换默认路由，也不影响 WiFi、蓝牙、音频和摄像头。
 */
class NativeNetwork {
public:
    explicit NativeNetwork(std::string interface_name);
    ~NativeNetwork();

    NetworkResult ethernet_status() const;
    NetworkResult ping_test(const std::string &target) const;
    NetworkResult location() const;
    NetworkResult start_speed_test(const std::string &url);
    NetworkResult stop_speed_test();
    NetworkResult speed_status() const;

private:
    void speed_worker(std::string url);

    std::string interface_name_;
    mutable std::mutex speed_mutex_;
    std::thread speed_thread_;
    std::atomic<bool> speed_running_{false};
    std::atomic<bool> speed_cancel_{false};
    std::string speed_status_json_ =
        "{\"running\":false,\"stage\":\"idle\",\"message\":\"未开始测速\"}";
};

} // namespace hardware_test

#endif
