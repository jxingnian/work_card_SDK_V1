#include "camera_native.h"

#include "ehal_camera.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <unistd.h>

namespace hardware_test {
namespace {

constexpr int kCameraChannel = 0;
constexpr uint32_t kCameraWidth = 640U;
constexpr uint32_t kCameraHeight = 480U;
constexpr uint32_t kCameraFps = 5U;

/* 把厂家返回码转换成可直接定位问题的中文错误文本。 */
std::string camera_error(int code)
{
    const char *text = ehal_camera_error_string(code);
    std::ostringstream output;
    output << "camera error=" << code;
    if (text != nullptr) {
        output << ": " << text;
    }
    return output.str();
}

/* 使用独立临时文件承接厂家 JPEG 输出，完成后立即删除。 */
std::string snapshot_path()
{
    std::ostringstream path;
    path << "/tmp/hardware_test_demo_snapshot_" << static_cast<long long>(getpid()) << ".jpg";
    return path.str();
}

} // namespace

NativeCamera::NativeCamera(std::string default_config_path,
                           std::string runtime_config_path,
                           std::string sensor_name)
    : default_config_path_(std::move(default_config_path)),
      runtime_config_path_(std::move(runtime_config_path)),
      sensor_name_(std::move(sensor_name))
{
}

NativeCamera::~NativeCamera()
{
    stop();
}

bool NativeCamera::start(std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) {
        return true;
    }

    int result = ehal_camera_create(&camera_);
    if (result != EHAL_OK) {
        last_error_ = camera_error(result);
        if (error_message != nullptr) {
            *error_message = last_error_;
        }
        camera_ = nullptr;
        return false;
    }

    ehal_camera_config_t config{};
    config.default_config_path = default_config_path_.c_str();
    config.runtime_config_path = runtime_config_path_.c_str();
    config.user_config_path = nullptr;
    config.pipeline_name = "video_record";
    config.sensor_name = sensor_name_.c_str();
    config.channel_count = 1U;
    config.channels[0].channel = kCameraChannel;
    /* 当前板端 SDK 要求启动阶段使用零掩码，视频参数由 JSON 配置生效。 */
    config.channels[0].config_mask = 0U;
    config.channels[0].width = kCameraWidth;
    config.channels[0].height = kCameraHeight;
    config.channels[0].fps = kCameraFps;
    config.channels[0].vpss_width = kCameraWidth;
    config.channels[0].vpss_height = kCameraHeight;
    config.channels[0].vi_fps = kCameraFps;
    config.video_callback = nullptr;
    config.user_data = nullptr;

    result = ehal_camera_configure(camera_, &config);
    if (result == EHAL_OK) {
        result = ehal_camera_start(camera_);
    }
    if (result == EHAL_OK) {
        result = ehal_camera_set_resolution(camera_, kCameraChannel,
                                            kCameraWidth, kCameraHeight);
    }
    if (result == EHAL_OK) {
        result = ehal_camera_set_fps(camera_, kCameraChannel, kCameraFps);
    }
    if (result != EHAL_OK) {
        last_error_ = camera_error(result);
        if (camera_ != nullptr) {
            (void)ehal_camera_stop(camera_);
            ehal_camera_destroy(camera_);
            camera_ = nullptr;
        }
        if (error_message != nullptr) {
            *error_message = last_error_;
        }
        return false;
    }

    last_error_.clear();
    snapshot_count_ = 0U;
    running_ = true;
    return true;
}

void NativeCamera::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (camera_ != nullptr) {
        if (running_) {
            (void)ehal_camera_stop(camera_);
        }
        ehal_camera_destroy(camera_);
        camera_ = nullptr;
    }
    running_ = false;
}

bool NativeCamera::snapshot(std::string *jpeg_data, std::string *error_message)
{
    if (jpeg_data == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || camera_ == nullptr) {
        if (error_message != nullptr) {
            *error_message = "camera is not running";
        }
        return false;
    }

    const std::string path = snapshot_path();
    const int result = ehal_camera_snapshot(camera_, kCameraChannel, path.c_str());
    if (result != EHAL_OK) {
        last_error_ = camera_error(result);
        if (error_message != nullptr) {
            *error_message = last_error_;
        }
        (void)unlink(path.c_str());
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        last_error_ = "camera snapshot file cannot be opened";
        if (error_message != nullptr) {
            *error_message = last_error_;
        }
        (void)unlink(path.c_str());
        return false;
    }
    std::ostringstream content;
    content << input.rdbuf();
    input.close();
    (void)unlink(path.c_str());
    *jpeg_data = content.str();
    if (jpeg_data->empty()) {
        last_error_ = "camera snapshot is empty";
        if (error_message != nullptr) {
            *error_message = last_error_;
        }
        return false;
    }
    ++snapshot_count_;
    return true;
}

std::string NativeCamera::status_json() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream output;
    output << "{\"running\":" << (running_ ? "true" : "false")
           << ",\"channel\":" << kCameraChannel
           << ",\"width\":" << kCameraWidth
           << ",\"height\":" << kCameraHeight
           << ",\"fps\":" << kCameraFps
           << ",\"sensor\":\"" << sensor_name_ << "\""
           << ",\"snapshot_count\":" << snapshot_count_
           << ",\"last_error\":\"" << last_error_ << "\"}";
    return output.str();
}

} // namespace hardware_test
