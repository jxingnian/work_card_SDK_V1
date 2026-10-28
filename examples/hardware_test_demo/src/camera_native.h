#ifndef HARDWARE_TEST_CAMERA_NATIVE_H
#define HARDWARE_TEST_CAMERA_NATIVE_H

#include <cstdint>
#include <mutex>
#include <string>

struct ehal_camera;

namespace hardware_test {

/*
 * hardware_test_demo 专用摄像头模块。
 * 摄像头的生命周期、固定分辨率和固定帧率全部由本模块管理，避免页面层
 * 或外部进程改变测试条件。
 */
class NativeCamera {
public:
    NativeCamera(std::string default_config_path,
                 std::string runtime_config_path,
                 std::string sensor_name);
    ~NativeCamera();

    NativeCamera(const NativeCamera &) = delete;
    NativeCamera &operator=(const NativeCamera &) = delete;

    /* 初始化并启动厂家摄像头，成功后固定输出 640x480@5 FPS。 */
    bool start(std::string *error_message);

    /* 停止并释放厂家摄像头资源。 */
    void stop();

    /* 生成当前帧 JPEG 数据，供同一个 HTTP 服务直接返回浏览器。 */
    bool snapshot(std::string *jpeg_data, std::string *error_message);

    /* 返回网页状态接口使用的 JSON 文本。 */
    std::string status_json() const;

private:
    mutable std::mutex mutex_;
    ehal_camera *camera_ = nullptr;
    std::string default_config_path_;
    std::string runtime_config_path_;
    std::string sensor_name_;
    std::string last_error_;
    bool running_ = false;
    uint64_t snapshot_count_ = 0U;
};

} // namespace hardware_test

#endif
