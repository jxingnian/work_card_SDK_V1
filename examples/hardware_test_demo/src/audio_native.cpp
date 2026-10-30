#include "audio_native.h"

#include "ehal_audio.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace hardware_test {
namespace {

/* SDK 包内的 libopus 没有随包提供公共头文件，这里声明其稳定的 C 接口。 */
struct OpusDecoderOpaque;
extern "C" OpusDecoderOpaque *opus_decoder_create(int sample_rate, int channels, int *error);
extern "C" void opus_decoder_destroy(OpusDecoderOpaque *decoder);
extern "C" int opus_decode(OpusDecoderOpaque *decoder, const unsigned char *data,
                            int length, int16_t *pcm, int frame_size, int decode_fec);

constexpr uint32_t kSampleRate = 16000;
constexpr uint8_t kChannels = 1;
constexpr uint8_t kBitWidth = 16;
constexpr uint32_t kSamplesPerFrame = 320;
constexpr uint64_t kMaxRecordingBytes = 16U * 1024U * 1024U;
constexpr int kAudioStartMaxAttempts = 3;
constexpr int kAudioStartRetryDelayMs = 500;

/* 只有运行时资源暂时不可用才重试；配置错误必须直接返回，避免重复执行必然失败的初始化。 */
bool is_retryable_start_error(int code)
{
    return code == EHAL_ERR_BUSY || code == EHAL_ERR_RUNTIME ||
        code == EHAL_ERR_TIMEOUT;
}

/* 将整数按 Ogg/Opus 规范写入小端字节序。 */
void append_u16(std::vector<uint8_t> *output, uint16_t value)
{
    output->push_back(static_cast<uint8_t>(value & 0xffU));
    output->push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<uint8_t> *output, uint32_t value)
{
    for (int index = 0; index < 4; ++index) {
        output->push_back(static_cast<uint8_t>(value & 0xffU));
        value >>= 8U;
    }
}

void append_u64(std::vector<uint8_t> *output, uint64_t value)
{
    for (int index = 0; index < 8; ++index) {
        output->push_back(static_cast<uint8_t>(value & 0xffU));
        value >>= 8U;
    }
}

/* Ogg 使用固定多项式计算页校验和，保证下载文件可被标准播放器识别。 */
uint32_t ogg_crc(const std::vector<uint8_t> &page)
{
    uint32_t crc = 0U;
    for (uint8_t value : page) {
        crc ^= static_cast<uint32_t>(value) << 24U;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U) != 0U ?
                (crc << 1U) ^ 0x04c11db7U : (crc << 1U);
        }
    }
    return crc;
}

std::vector<uint8_t> make_ogg_page(const std::vector<uint8_t> &packet,
                                   uint32_t serial, uint32_t sequence,
                                   uint64_t granule, uint8_t header_type)
{
    std::vector<uint8_t> page;
    std::vector<uint8_t> lacing;
    size_t remaining = packet.size();
    do {
        const uint8_t segment = static_cast<uint8_t>(remaining > 255U ? 255U : remaining);
        lacing.push_back(segment);
        remaining -= segment;
    } while (remaining > 0U || packet.empty());
    page.insert(page.end(), {'O', 'g', 'g', 'S', 0, header_type});
    append_u64(&page, granule);
    append_u32(&page, serial);
    append_u32(&page, sequence);
    append_u32(&page, 0U);
    page.push_back(static_cast<uint8_t>(lacing.size()));
    page.insert(page.end(), lacing.begin(), lacing.end());
    page.insert(page.end(), packet.begin(), packet.end());
    const uint32_t crc = ogg_crc(page);
    page[22] = static_cast<uint8_t>(crc & 0xffU);
    page[23] = static_cast<uint8_t>((crc >> 8U) & 0xffU);
    page[24] = static_cast<uint8_t>((crc >> 16U) & 0xffU);
    page[25] = static_cast<uint8_t>((crc >> 24U) & 0xffU);
    return page;
}

/* 将带两字节长度前缀的 OPUS 帧封装为标准 Ogg Opus 文件。 */
bool make_ogg_opus(const std::vector<uint8_t> &framed,
                   std::string *output, std::string *error_message)
{
    std::vector<uint8_t> result;
    const uint32_t serial = 0x48544431U;
    uint32_t sequence = 0U;
    std::vector<uint8_t> head({'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, kChannels, 0, 0});
    append_u32(&head, kSampleRate);
    append_u16(&head, 0U);
    head.push_back(0U);
    auto page = make_ogg_page(head, serial, sequence++, 0U, 0x02U);
    result.insert(result.end(), page.begin(), page.end());
    std::vector<uint8_t> tags({'O', 'p', 'u', 's', 'T', 'a', 'g', 's'});
    append_u32(&tags, 9U);
    tags.insert(tags.end(), {'e', 'h', 'a', 'l', '-', 'd', 'e', 'm', 'o'});
    append_u32(&tags, 0U);
    page = make_ogg_page(tags, serial, sequence++, 0U, 0U);
    result.insert(result.end(), page.begin(), page.end());
    size_t offset = 0U;
    uint64_t granule = 0U;
    while (offset + 2U <= framed.size()) {
        const uint16_t packet_size = static_cast<uint16_t>(framed[offset]) |
            (static_cast<uint16_t>(framed[offset + 1U]) << 8U);
        offset += 2U;
        if (packet_size == 0U || offset + packet_size > framed.size()) {
            if (error_message != nullptr) *error_message = "invalid OPUS frame length";
            return false;
        }
        std::vector<uint8_t> packet(framed.begin() + offset,
                                    framed.begin() + offset + packet_size);
        offset += packet_size;
        granule += kSamplesPerFrame;
        page = make_ogg_page(packet, serial, sequence++, granule, 0U);
        result.insert(result.end(), page.begin(), page.end());
    }
    if (offset != framed.size() || result.size() <= 27U) {
        if (error_message != nullptr) *error_message = "recording contains no complete OPUS frame";
        return false;
    }
    result[result.size() - 26U] |= 0x04U;
    output->assign(reinterpret_cast<const char *>(result.data()), result.size());
    return true;
}

/* 写入标准 PCM WAV 头，结束录音时再回填实际数据长度。 */
void write_wav_header(std::ostream &file, uint32_t data_size)
{
    const uint32_t byte_rate = kSampleRate * kChannels * (kBitWidth / 8U);
    const uint16_t wav_channels = static_cast<uint16_t>(kChannels);
    const uint16_t wav_bit_width = static_cast<uint16_t>(kBitWidth);
    const uint16_t block_align = static_cast<uint16_t>(kChannels * (kBitWidth / 8U));
    const uint32_t riff_size = 36U + data_size;
    file.seekp(0, std::ios::beg);
    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char *>(&riff_size), sizeof(riff_size));
    file.write("WAVEfmt ", 8);
    const uint32_t fmt_size = 16;
    const uint16_t pcm_format = 1;
    file.write(reinterpret_cast<const char *>(&fmt_size), sizeof(fmt_size));
    file.write(reinterpret_cast<const char *>(&pcm_format), sizeof(pcm_format));
    /* WAV 规范要求 channels 和 bit_width 都是 16 位字段，不能直接写入 SDK 的 8 位配置变量。 */
    file.write(reinterpret_cast<const char *>(&wav_channels), sizeof(wav_channels));
    file.write(reinterpret_cast<const char *>(&kSampleRate), sizeof(kSampleRate));
    file.write(reinterpret_cast<const char *>(&byte_rate), sizeof(byte_rate));
    file.write(reinterpret_cast<const char *>(&block_align), sizeof(block_align));
    file.write(reinterpret_cast<const char *>(&wav_bit_width), sizeof(wav_bit_width));
    file.write("data", 4);
    file.write(reinterpret_cast<const char *>(&data_size), sizeof(data_size));
}

} // namespace

NativeAudio::NativeAudio(std::string default_config_path,
                         std::string runtime_config_path)
    : default_config_path_(std::move(default_config_path)),
      runtime_config_path_(std::move(runtime_config_path))
{
}

NativeAudio::~NativeAudio()
{
    stop();
}

std::string NativeAudio::make_error(int code)
{
    std::ostringstream output;
    output << "audio error " << code << ": " << ehal_audio_error_string(code);
    return output.str();
}

bool NativeAudio::start(std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
        return true;
    }
    for (int attempt = 1; attempt <= kAudioStartMaxAttempts; ++attempt) {
    int ret = ehal_audio_create(&audio_);
    if (ret != EHAL_OK) {
        if (is_retryable_start_error(ret) && attempt < kAudioStartMaxAttempts) {
            std::fprintf(stderr, "audio create retry %d/%d after error: %s\n",
                         attempt, kAudioStartMaxAttempts, make_error(ret).c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(kAudioStartRetryDelayMs));
            continue;
        }
        if (error_message != nullptr) *error_message = make_error(ret);
        return false;
    }
    ehal_audio_config_t config{};
    config.default_config_path = default_config_path_.c_str();
    config.runtime_config_path = runtime_config_path_.c_str();
    config.enable_record = 1;
    config.enable_playback = 1;
    config.ai_dev = 0;
    config.ao_dev = 0;
    config.aenc_channel = 0;
    config.input_volume = static_cast<int>(input_volume_);
    config.output_volume = static_cast<int>(output_volume_);
    config.sample_rate = kSampleRate;
    config.channels = kChannels;
    config.bit_width = kBitWidth;
    config.samples_per_frame = kSamplesPerFrame;
    /* 录音链路使用 OPUS；回调收到的是压缩帧，不能按 PCM 直接写 WAV。 */
    config.codec = EHAL_AUDIO_CODEC_OPUS;
    config.capture_callback = capture_callback;
    config.user_data = this;
    ret = ehal_audio_configure(audio_, &config);
    if (ret == EHAL_OK) {
        ret = ehal_audio_set_input_channel(audio_, EHAL_AUDIO_INPUT_LEFT);
    }
    if (ret == EHAL_OK) {
        ret = ehal_audio_start(audio_);
    }
    if (ret == EHAL_OK) {
        ret = ehal_audio_set_input_volume(audio_, static_cast<int>(input_volume_));
    }
    if (ret == EHAL_OK) {
        ret = ehal_audio_set_output_volume(audio_, static_cast<int>(output_volume_));
    }
    if (ret != EHAL_OK) {
        ehal_audio_destroy(audio_);
        audio_ = nullptr;
        if (is_retryable_start_error(ret) && attempt < kAudioStartMaxAttempts) {
            std::fprintf(stderr, "audio start retry %d/%d after error: %s\n",
                         attempt, kAudioStartMaxAttempts, make_error(ret).c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(kAudioStartRetryDelayMs));
            continue;
        }
        if (error_message != nullptr) *error_message = make_error(ret);
        last_error_ = make_error(ret);
        return false;
    }
    started_ = true;
    recording_channel_ = "left";
    last_error_.clear();
    return true;
    }
    return false;
}

void NativeAudio::stop()
{
    stop_playback();
    std::lock_guard<std::mutex> lock(mutex_);
    finish_recording_locked();
    if (audio_ != nullptr) {
        if (started_) (void)ehal_audio_stop(audio_);
        ehal_audio_destroy(audio_);
        audio_ = nullptr;
    }
    started_ = false;
}

bool NativeAudio::configure_and_start(const std::string &channel,
                                     std::string *error_message)
{
    if (audio_ == nullptr) {
        if (error_message != nullptr) *error_message = "audio is not created";
        return false;
    }
    const ehal_audio_input_t input = channel == "right" ?
        EHAL_AUDIO_INPUT_RIGHT : EHAL_AUDIO_INPUT_LEFT;
    const int ret = ehal_audio_set_input_channel(audio_, input);
    if (ret != EHAL_OK) {
        if (error_message != nullptr) *error_message = make_error(ret);
        return false;
    }
    const int start_ret = ehal_audio_start(audio_);
    if (start_ret != EHAL_OK) {
        if (error_message != nullptr) *error_message = make_error(start_ret);
        return false;
    }
    (void)ehal_audio_set_input_volume(audio_, static_cast<int>(input_volume_));
    (void)ehal_audio_set_output_volume(audio_, static_cast<int>(output_volume_));
    started_ = true;
    recording_channel_ = channel == "right" ? "right" : "left";
    return true;
}

bool NativeAudio::start_record(const std::string &channel, std::string *error_message)
{
    if (channel != "left" && channel != "right") {
        if (error_message != nullptr) *error_message = "channel must be left or right";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (recording_) {
        if (error_message != nullptr) *error_message = "recording is already running";
        return false;
    }
    if (!started_ || audio_ == nullptr) {
        if (error_message != nullptr) *error_message = "audio is not running";
        return false;
    }
    if (recording_channel_ != channel) {
        (void)ehal_audio_stop(audio_);
        started_ = false;
        if (!configure_and_start(channel, error_message)) return false;
    }
    std::ofstream file(recording_path_, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        if (error_message != nullptr) *error_message = "cannot create recording file";
        return false;
    }
    file.close();
    recording_bytes_ = 0;
    recording_ = true;
    last_error_.clear();
    return true;
}

bool NativeAudio::stop_record(std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recording_) {
        if (error_message != nullptr) *error_message = "recording is not running";
        return false;
    }
    finish_recording_locked();
    return true;
}

void NativeAudio::finish_recording_locked()
{
    if (!recording_) return;
    recording_ = false;
    /* OPUS 录音文件已经按帧写入，停止时只需关闭录音状态，不再补 PCM/WAV 头。 */
    /* 录音文件由每次回调打开的追加流负责落盘。 */
    /* 保留该函数作为录音生命周期的统一收口。 */
    /* NOLINTNEXTLINE(readability-function-size) */
    /* 下面的旧 WAV 回填逻辑已被 OPUS 帧格式替代。 */
    /* no-op */
    #if 0
        /*
         * SDK 的 WAV 播放接口按 320 个采样、16 bit、单声道组织数据，
         * 因此每一帧必须是 320 * 2 = 640 字节。录音回调在停止时可能
         * 正好落在半帧，直接播放会返回 invalid configuration；这里把
         * 最后一帧补零，保证录音文件可以直接交给同一个播放接口。
         */
        const uint64_t frame_bytes =
            static_cast<uint64_t>(kSamplesPerFrame) * (kBitWidth / 8U) * kChannels;
        const uint64_t remainder = recording_bytes_ % frame_bytes;
        if (remainder != 0U && recording_bytes_ + (frame_bytes - remainder) <= kMaxRecordingBytes) {
            const uint64_t padding = frame_bytes - remainder;
            std::vector<char> zero_padding(static_cast<size_t>(padding), 0);
            file.seekp(0, std::ios::end);
            file.write(zero_padding.data(), static_cast<std::streamsize>(zero_padding.size()));
            if (file.good()) {
                recording_bytes_ += padding;
            }
        }
        write_wav_header(file, static_cast<uint32_t>(recording_bytes_));
    #endif
}

void NativeAudio::capture_callback(const ehal_audio_frame_t *frame, void *user_data)
{
    if (frame != nullptr && user_data != nullptr) {
        static_cast<NativeAudio *>(user_data)->on_capture(frame);
    }
}

void NativeAudio::on_capture(const ehal_audio_frame_t *frame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++capture_frames_;
    if (!recording_ || frame->data == nullptr || frame->size == 0U ||
        recording_bytes_ + frame->size > kMaxRecordingBytes) {
        return;
    }
    std::ofstream file(recording_path_, std::ios::binary | std::ios::app);
    if (!file.is_open()) {
        last_error_ = "cannot append recording file";
        recording_ = false;
        return;
    }
    /* 每个 OPUS 包加长度前缀，便于停止录音后逐包解码和封装 Ogg。 */
    if (frame->codec != EHAL_AUDIO_CODEC_OPUS || frame->size > 0xffffU) {
        last_error_ = "capture callback did not provide a valid OPUS frame";
        return;
    }
    const uint16_t packet_size = static_cast<uint16_t>(frame->size);
    file.put(static_cast<char>(packet_size & 0xffU));
    file.put(static_cast<char>((packet_size >> 8U) & 0xffU));
    file.write(reinterpret_cast<const char *>(frame->data), frame->size);
    if (file.good()) recording_bytes_ += frame->size;
}

bool NativeAudio::upload(const std::string &wav_data, std::string *error_message)
{
    if (wav_data.size() < 44U || wav_data.compare(0U, 4U, "RIFF") != 0 ||
        wav_data.compare(8U, 4U, "WAVE") != 0) {
        if (error_message != nullptr) *error_message = "only PCM WAV files are supported";
        return false;
    }
    std::ofstream file(upload_path_, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        if (error_message != nullptr) *error_message = "cannot create upload file";
        return false;
    }
    file.write(wav_data.data(), static_cast<std::streamsize>(wav_data.size()));
    if (!file.good()) {
        if (error_message != nullptr) *error_message = "cannot save upload file";
        return false;
    }
    return true;
}

bool NativeAudio::play(const std::string &source, uint32_t frequency_hz,
                       uint32_t duration_ms, uint32_t volume,
                       uint32_t loop_count, bool infinite_loop,
                       std::string *error_message)
{
    if (source != "voice" && source != "recording" && source != "tone") {
        if (error_message != nullptr) *error_message = "source must be voice, recording or tone";
        return false;
    }
    if (source == "tone" && (frequency_hz == 0U || duration_ms == 0U)) {
        if (error_message != nullptr) *error_message = "tone frequency and duration are required";
        return false;
    }
    if (volume > 100U || (!infinite_loop && loop_count == 0U)) {
        if (error_message != nullptr) *error_message = "invalid volume or loop count";
        return false;
    }
    std::thread old_thread;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || audio_ == nullptr) {
            if (error_message != nullptr) *error_message = "audio is not running";
            return false;
        }
        if (playback_) {
            if (error_message != nullptr) *error_message = "playback is already running";
            return false;
        }
        /* 播放线程完成后仍保持 joinable，复用前必须先在线程外完成回收。 */
        if (playback_thread_.joinable()) {
            old_thread = std::move(playback_thread_);
        }
    }
    if (old_thread.joinable()) {
        old_thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || audio_ == nullptr) {
            if (error_message != nullptr) *error_message = "audio is not running";
            return false;
        }
        if (playback_) {
            if (error_message != nullptr) *error_message = "playback is already running";
            return false;
        }
        const std::string file_path = source == "recording" ? recording_path_ : upload_path_;
        if (source != "tone") {
            std::ifstream file(file_path, std::ios::binary);
            if (!file.is_open()) {
                if (error_message != nullptr) *error_message = source == "recording" ?
                    "no recording is available" : "upload a WAV voice file first";
                return false;
            }
        }
        playback_stop_ = false;
        playback_ = true;
        playback_thread_ = std::thread(&NativeAudio::playback_worker, this, source,
                                       frequency_hz, duration_ms, volume,
                                       loop_count, infinite_loop);
    }
    return true;
}

void NativeAudio::playback_worker(std::string source, uint32_t frequency_hz,
                                  uint32_t duration_ms, uint32_t volume,
                                  uint32_t loop_count, bool infinite_loop)
{
    uint32_t completed = 0;
    while (infinite_loop || completed < loop_count) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (playback_stop_) break;
        }
        int ret = EHAL_OK;
        if (source == "recording") {
            /* OPUS 录音必须先解码成 PCM，不能交给 WAV 播放器直接解释。 */
            std::vector<int16_t> pcm;
            std::string decode_error;
            if (!decode_recording(&pcm, &decode_error)) {
                std::lock_guard<std::mutex> lock(mutex_);
                last_error_ = decode_error;
                break;
            }
            const size_t chunk_samples = kSamplesPerFrame * kChannels;
            for (size_t offset = 0U; offset < pcm.size(); offset += chunk_samples) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (playback_stop_) break;
                }
                const size_t samples = std::min(chunk_samples, pcm.size() - offset);
                ret = ehal_audio_play_pcm(audio_, pcm.data() + offset,
                                          static_cast<uint32_t>(samples * sizeof(int16_t)), 1000);
                if (ret != EHAL_OK) break;
            }
        } else if (source == "voice") {
            ret = ehal_audio_play_wav_file(audio_, upload_path_.c_str(), 1000);
        } else {
            ret = ehal_audio_play_test_tone(audio_, frequency_hz, duration_ms,
                                            static_cast<int>(volume));
        }
        if (ret != EHAL_OK) {
            std::lock_guard<std::mutex> lock(mutex_);
            last_error_ = make_error(ret);
            break;
        }
        ++completed;
        std::lock_guard<std::mutex> lock(mutex_);
        ++playback_count_;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    playback_ = false;
}

void NativeAudio::stop_playback()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        playback_stop_ = true;
        if (audio_ != nullptr) (void)ehal_audio_stop_playback(audio_);
    }
    if (playback_thread_.joinable()) playback_thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    playback_ = false;
}

bool NativeAudio::set_volume(uint32_t output_volume, std::string *error_message)
{
    if (output_volume > 100U) {
        if (error_message != nullptr) *error_message = "volume must be 0..100";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (audio_ == nullptr) {
        if (error_message != nullptr) *error_message = "audio is not created";
        return false;
    }
    const int ret = ehal_audio_set_output_volume(audio_, static_cast<int>(output_volume));
    if (ret != EHAL_OK) {
        if (error_message != nullptr) *error_message = make_error(ret);
        return false;
    }
    output_volume_ = output_volume;
    return true;
}

bool NativeAudio::set_gain(uint32_t input_volume, std::string *error_message)
{
    if (input_volume > 100U) {
        if (error_message != nullptr) *error_message = "gain must be 0..100";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (audio_ == nullptr) {
        if (error_message != nullptr) *error_message = "audio is not created";
        return false;
    }
    const int ret = ehal_audio_set_input_volume(audio_, static_cast<int>(input_volume));
    if (ret != EHAL_OK) {
        if (error_message != nullptr) *error_message = make_error(ret);
        return false;
    }
    input_volume_ = input_volume;
    return true;
}

std::string NativeAudio::status_json() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream output;
    output << "{\"ok\":true,\"running\":" << (started_ ? "true" : "false")
           << ",\"recording\":" << (recording_ ? "true" : "false")
           << ",\"recording_channel\":\"" << recording_channel_ << "\""
           << ",\"playback\":" << (playback_ ? "true" : "false")
           << ",\"sample_rate\":" << kSampleRate
           << ",\"channels\":1,\"bit_width\":16"
           << ",\"input_gain\":" << input_volume_
           << ",\"output_volume\":" << output_volume_
           << ",\"capture_frames\":" << capture_frames_
           << ",\"recording_bytes\":" << recording_bytes_
           << ",\"playback_count\":" << playback_count_
           << ",\"last_error\":\"" << last_error_ << "\"}";
    return output.str();
}

bool NativeAudio::decode_recording(std::vector<int16_t> *pcm,
                                   std::string *error_message) const
{
    std::ifstream file(recording_path_, std::ios::binary);
    if (!file.is_open()) {
        if (error_message != nullptr) *error_message = "recording file does not exist";
        return false;
    }
    int decoder_error = 0;
    OpusDecoderOpaque *decoder = opus_decoder_create(
        static_cast<int>(kSampleRate), static_cast<int>(kChannels), &decoder_error);
    if (decoder == nullptr) {
        if (error_message != nullptr) *error_message = "cannot create OPUS decoder";
        return false;
    }
    std::vector<int16_t> output(5760U * kChannels);
    while (true) {
        uint8_t length_bytes[2] = {};
        file.read(reinterpret_cast<char *>(length_bytes), sizeof(length_bytes));
        if (file.gcount() == 0) break;
        if (file.gcount() != static_cast<std::streamsize>(sizeof(length_bytes))) {
            opus_decoder_destroy(decoder);
            if (error_message != nullptr) *error_message = "truncated OPUS frame length";
            return false;
        }
        const uint16_t packet_size = static_cast<uint16_t>(length_bytes[0]) |
            (static_cast<uint16_t>(length_bytes[1]) << 8U);
        if (packet_size == 0U) {
            opus_decoder_destroy(decoder);
            if (error_message != nullptr) *error_message = "invalid OPUS frame length";
            return false;
        }
        std::vector<uint8_t> packet(packet_size);
        file.read(reinterpret_cast<char *>(packet.data()), packet_size);
        if (file.gcount() != static_cast<std::streamsize>(packet_size)) {
            opus_decoder_destroy(decoder);
            if (error_message != nullptr) *error_message = "truncated OPUS frame";
            return false;
        }
        const int decoded_samples = opus_decode(
            decoder, packet.data(), static_cast<int>(packet.size()),
            output.data(), 5760, 0);
        if (decoded_samples < 0 ||
            static_cast<size_t>(decoded_samples) * kChannels > output.size()) {
            opus_decoder_destroy(decoder);
            if (error_message != nullptr) {
                std::ostringstream detail;
                detail << "OPUS decode failed ret=" << decoded_samples
                       << " decoder_error=" << decoder_error
                       << " packet_size=" << packet_size;
                *error_message = detail.str();
            }
            return false;
        }
        pcm->insert(pcm->end(), output.begin(), output.begin() +
                    static_cast<size_t>(decoded_samples) * kChannels);
    }
    opus_decoder_destroy(decoder);
    if (pcm->empty()) {
        if (error_message != nullptr) *error_message = "recording contains no OPUS audio";
        return false;
    }
    return true;
}

bool NativeAudio::read_recording(std::string *data, std::string *error_message) const
{
    std::ifstream file(recording_path_, std::ios::binary);
    if (!file.is_open()) {
        if (error_message != nullptr) *error_message = "recording file does not exist";
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string framed = buffer.str();
    const std::vector<uint8_t> framed_bytes(framed.begin(), framed.end());
    return make_ogg_opus(framed_bytes, data, error_message);
}

} // namespace hardware_test
