#ifndef HARDWARE_TEST_AUDIO_NATIVE_H
#define HARDWARE_TEST_AUDIO_NATIVE_H

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ehal_audio.h"

namespace hardware_test {

/*
 * 音频原生模块负责当前独立 Demo 的 AI、AO 和 ACODEC 生命周期。
 * 录音使用 SDK 回调写入 WAV 文件，播放使用 SDK 的 PCM/WAV 接口。
 */
class NativeAudio {
public:
    NativeAudio(std::string default_config_path, std::string runtime_config_path);
    ~NativeAudio();

    bool start(std::string *error_message);
    void stop();

    bool start_record(const std::string &channel, std::string *error_message);
    bool stop_record(std::string *error_message);
    bool upload(const std::string &wav_data, std::string *error_message);
    bool play(const std::string &source, uint32_t frequency_hz,
              uint32_t duration_ms, uint32_t volume, uint32_t loop_count,
              bool infinite_loop, std::string *error_message);
    void stop_playback();
    bool set_volume(uint32_t output_volume, std::string *error_message);
    bool set_gain(uint32_t input_volume, std::string *error_message);
    std::string status_json() const;
    bool read_recording(std::string *data, std::string *error_message) const;

private:
    static void capture_callback(const ehal_audio_frame_t *frame, void *user_data);
    void on_capture(const ehal_audio_frame_t *frame);
    bool configure_and_start(const std::string &channel, std::string *error_message);
    void finish_recording_locked();
    bool decode_recording(std::vector<int16_t> *pcm, std::string *error_message) const;
    void playback_worker(std::string source, uint32_t frequency_hz,
                         uint32_t duration_ms, uint32_t volume,
                         uint32_t loop_count, bool infinite_loop);
    static std::string make_error(int code);

    std::string default_config_path_;
    std::string runtime_config_path_;
    ehal_audio_t *audio_ = nullptr;
    mutable std::mutex mutex_;
    std::thread playback_thread_;
    bool started_ = false;
    bool recording_ = false;
    bool playback_ = false;
    bool playback_stop_ = false;
    std::string recording_channel_ = "left";
    std::string recording_path_ = "/tmp/hardware_test_demo_recording.opus.frames";
    std::string upload_path_ = "/tmp/hardware_test_demo_upload.wav";
    std::string last_error_;
    uint32_t input_volume_ = 30;
    uint32_t output_volume_ = 80;
    uint64_t recording_bytes_ = 0;
    uint64_t capture_frames_ = 0;
    uint64_t playback_count_ = 0;
};

} // namespace hardware_test

#endif
