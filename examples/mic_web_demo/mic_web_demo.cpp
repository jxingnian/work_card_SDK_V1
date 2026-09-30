#include "ehal_audio.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;

struct Config {
    int port = 8091;
    std::string web_root = "web";
    std::string default_config = "configs/hal_default.json";
    std::string pcm_runtime_config;
    std::string opus_runtime_config;
    std::string playback_audio = "audio/speaker_voice.wav";
};

struct AudioBuffer {
    std::mutex mutex;
    std::vector<uint8_t> encoded;
    uint64_t frames = 0;
    uint64_t bytes = 0;
    bool stream_to_file = false;
    std::ofstream stream_file;
};

void on_signal(int)
{
    g_stop = 1;
}

bool send_all(int fd, const void *data, size_t size)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    while (size > 0) {
        ssize_t n = send(fd, p, size, 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

void send_response(int fd,
                   int status,
                   const std::string &content_type,
                   const std::vector<uint8_t> &body)
{
    const char *text = status == 200 ? "OK" : (status == 404 ? "Not Found" : "Error");
    std::ostringstream header;
    header << "HTTP/1.1 " << status << ' ' << text << "\r\n"
           << "Content-Type: " << content_type << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Connection: close\r\n\r\n";
    std::string h = header.str();
    (void)send_all(fd, h.data(), h.size());
    if (!body.empty()) {
        (void)send_all(fd, body.data(), body.size());
    }
}

void send_text(int fd, int status, const std::string &content_type, const std::string &body)
{
    send_response(fd,
                  status,
                  content_type,
                  std::vector<uint8_t>(body.begin(), body.end()));
}

std::string content_type(const std::string &path)
{
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".html") {
        return "text/html; charset=utf-8";
    }
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".js") {
        return "application/javascript; charset=utf-8";
    }
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".css") {
        return "text/css; charset=utf-8";
    }
    return "application/octet-stream";
}

bool read_file(const std::string &path, std::vector<uint8_t> *data)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size < 0) {
        return false;
    }
    data->resize(static_cast<size_t>(size));
    if (!data->empty()) {
        file.read(reinterpret_cast<char *>(data->data()), size);
    }
    return file.good() || file.eof();
}

void put_u16(std::vector<uint8_t> *out, uint16_t value)
{
    out->push_back(static_cast<uint8_t>(value & 0xffU));
    out->push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
}

void put_u32(std::vector<uint8_t> *out, uint32_t value)
{
    out->push_back(static_cast<uint8_t>(value & 0xffU));
    out->push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
    out->push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
    out->push_back(static_cast<uint8_t>((value >> 24U) & 0xffU));
}

std::vector<uint8_t> make_wav_from_pcm(const std::vector<uint8_t> &pcm,
                                       uint32_t sample_rate,
                                       uint16_t channels,
                                       uint16_t bits)
{
    uint32_t pcm_bytes = static_cast<uint32_t>(pcm.size());
    std::vector<uint8_t> wav;
    wav.reserve(44U + pcm_bytes);
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    put_u32(&wav, 36U + pcm_bytes);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put_u32(&wav, 16U);
    put_u16(&wav, 1U);
    put_u16(&wav, channels);
    put_u32(&wav, sample_rate);
    put_u32(&wav, sample_rate * channels * bits / 8U);
    put_u16(&wav, channels * bits / 8U);
    put_u16(&wav, bits);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    put_u32(&wav, pcm_bytes);
    wav.insert(wav.end(), pcm.begin(), pcm.end());
    return wav;
}

uint32_t ogg_crc(const std::vector<uint8_t> &data)
{
    uint32_t crc = 0;
    for (uint8_t value : data) {
        crc ^= static_cast<uint32_t>(value) << 24U;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U) != 0U ?
                (crc << 1U) ^ 0x04c11db7U : (crc << 1U);
        }
    }
    return crc;
}

void append_u64_le(std::vector<uint8_t> *out, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        out->push_back(static_cast<uint8_t>(value & 0xffU));
        value >>= 8U;
    }
}

std::vector<uint8_t> make_ogg_page(const std::vector<uint8_t> &packet,
                                   uint32_t serial,
                                   uint32_t sequence,
                                   uint64_t granule,
                                   uint8_t header_type)
{
    std::vector<uint8_t> page;
    size_t remaining = packet.size();
    size_t offset = 0;
    std::vector<uint8_t> lacing;
    do {
        uint8_t segment = static_cast<uint8_t>(remaining > 255U ? 255U : remaining);
        lacing.push_back(segment);
        remaining -= segment;
        offset += segment;
    } while (remaining > 0U || packet.empty());

    page.insert(page.end(), {'O', 'g', 'g', 'S'});
    page.push_back(0);
    page.push_back(header_type);
    append_u64_le(&page, granule);
    put_u32(&page, serial);
    put_u32(&page, sequence);
    put_u32(&page, 0);
    page.push_back(static_cast<uint8_t>(lacing.size()));
    page.insert(page.end(), lacing.begin(), lacing.end());
    page.insert(page.end(), packet.begin(), packet.end());
    uint32_t crc = ogg_crc(page);
    page[22] = static_cast<uint8_t>(crc & 0xffU);
    page[23] = static_cast<uint8_t>((crc >> 8U) & 0xffU);
    page[24] = static_cast<uint8_t>((crc >> 16U) & 0xffU);
    page[25] = static_cast<uint8_t>((crc >> 24U) & 0xffU);
    return page;
}

std::vector<uint8_t> make_ogg_opus(const std::vector<uint8_t> &frames,
                                   uint32_t sample_rate,
                                   uint8_t channels)
{
    std::vector<uint8_t> out;
    const uint32_t serial = 0x4d494331U;
    uint32_t sequence = 0;
    std::vector<uint8_t> head({'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
                                1, channels, 0, 0});
    put_u32(&head, sample_rate);
    put_u16(&head, 0);
    head.push_back(0);
    auto head_page = make_ogg_page(head, serial, sequence++, 0, 0x02);
    out.insert(out.end(), head_page.begin(), head_page.end());
    std::vector<uint8_t> tags({'O', 'p', 'u', 's', 'T', 'a', 'g', 's'});
    put_u32(&tags, 7);
    tags.insert(tags.end(), {'e', 'h', 'a', 'l', '-', 's', 'd', 'k'});
    put_u32(&tags, 0);
    auto tag_page = make_ogg_page(tags, serial, sequence++, 0, 0);
    out.insert(out.end(), tag_page.begin(), tag_page.end());

    // The SDK callback delivers frame boundaries, so each callback payload is one Opus packet.
    // The caller stores packets with a 2-byte little-endian length prefix.
    size_t pos = 0;
    uint64_t granule = 0;
    while (pos + 2U <= frames.size()) {
        uint16_t size = static_cast<uint16_t>(frames[pos] | (frames[pos + 1U] << 8U));
        pos += 2U;
        if (size == 0U || pos + size > frames.size()) {
            break;
        }
        std::vector<uint8_t> packet(frames.begin() + pos, frames.begin() + pos + size);
        pos += size;
        granule += sample_rate / 50U;
        auto page = make_ogg_page(packet, serial, sequence++, granule, 0);
        out.insert(out.end(), page.begin(), page.end());
    }
    if (out.size() >= 27U) {
        out[out.size() - 26U] |= 0x04U;
    }
    return out;
}

void audio_callback(const ehal_audio_frame_t *frame, void *user_data)
{
    auto *buffer = static_cast<AudioBuffer *>(user_data);
    if (buffer == nullptr || frame == nullptr || frame->data == nullptr || frame->size == 0U) {
        return;
    }
    std::lock_guard<std::mutex> lock(buffer->mutex);
    if (buffer->stream_to_file && buffer->stream_file.is_open()) {
        if (frame->codec == EHAL_AUDIO_CODEC_OPUS) {
            uint16_t size = static_cast<uint16_t>(frame->size > 0xffffU ? 0xffffU : frame->size);
            buffer->stream_file.put(static_cast<char>(size & 0xffU));
            buffer->stream_file.put(static_cast<char>((size >> 8U) & 0xffU));
        }
        buffer->stream_file.write(reinterpret_cast<const char *>(frame->data), frame->size);
        buffer->frames++;
        buffer->bytes += frame->size;
        return;
    }
    if (frame->codec == EHAL_AUDIO_CODEC_OPUS) {
        uint16_t size = static_cast<uint16_t>(
            frame->size > 0xffffU ? 0xffffU : frame->size);
        buffer->encoded.push_back(static_cast<uint8_t>(size & 0xffU));
        buffer->encoded.push_back(static_cast<uint8_t>((size >> 8U) & 0xffU));
    }
    buffer->encoded.insert(buffer->encoded.end(), frame->data, frame->data + frame->size);
    buffer->frames++;
    buffer->bytes += frame->size;
}

int parse_seconds(const std::string &path)
{
    size_t pos = path.find("seconds=");
    if (pos == std::string::npos) {
        return 5;
    }
    int seconds = std::atoi(path.c_str() + pos + 8U);
    return std::max(1, std::min(30, seconds));
}

std::string parse_format(const std::string &path)
{
    return path.find("format=opus") != std::string::npos ? "opus" : "pcm";
}

class MicWebDemo {
public:
    explicit MicWebDemo(Config config) : config_(std::move(config)) {}

    ~MicWebDemo()
    {
        if (audio_ != nullptr) {
            (void)ehal_audio_stop_playback(audio_);
        }
        if (playback_thread_.joinable()) {
            playback_thread_.join();
        }
        stop_audio();
    }

    int configure_audio(ehal_audio_codec_t codec,
                        int enable_record,
                        int enable_playback)
    {
        if (audio_ != nullptr) {
            ehal_audio_destroy(audio_);
            audio_ = nullptr;
        }
        record_enabled_ = enable_record != 0;
        playback_enabled_ = enable_playback != 0;
        int ret = ehal_audio_create(&audio_);
        if (ret != EHAL_OK) {
            return ret;
        }
        ehal_audio_config_t audio_config{};
        audio_config.default_config_path = config_.default_config.c_str();
        audio_config.runtime_config_path =
            codec == EHAL_AUDIO_CODEC_OPUS ?
                config_.opus_runtime_config.c_str() :
                config_.pcm_runtime_config.c_str();
        audio_config.enable_record = enable_record;
        audio_config.enable_playback = enable_playback;
        audio_config.ai_dev = 0;
        audio_config.aenc_channel = 0;
        audio_config.input_volume = input_volume_;
        audio_config.output_volume = output_volume_;
        audio_config.playback_pipeline_name =
            enable_playback && codec == EHAL_AUDIO_CODEC_PCM ?
                "audio_playback_pcm" : "audio_playback";
        audio_config.sample_rate = 16000;
        audio_config.channels = 1;
        audio_config.bit_width = 16;
        audio_config.samples_per_frame = 320;
        audio_config.codec = codec;
        audio_config.capture_callback = audio_callback;
        audio_config.user_data = &audio_buffer_;
        return ehal_audio_configure(audio_, &audio_config);
    }

    int run()
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return 1;
        }
        int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<uint16_t>(config_.port));
        if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
            listen(fd, 8) != 0) {
            close(fd);
            return 1;
        }
        std::printf("mic web demo listening on port %d\n", config_.port);
        std::fflush(stdout);
        while (!g_stop) {
            int client = accept(fd, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            std::thread(&MicWebDemo::handle_client, this, client).detach();
        }
        close(fd);
        return 0;
    }

private:
    void handle_client(int fd)
    {
        char buffer[2048];
        ssize_t n = recv(fd, buffer, sizeof(buffer) - 1U, 0);
        if (n <= 0) {
            close(fd);
            return;
        }
        buffer[n] = '\0';
        std::istringstream request(buffer);
        std::string method;
        std::string path;
        request >> method >> path;
        if (method != "GET") {
            send_text(fd, 500, "text/plain; charset=utf-8", "Only GET is supported\n");
            close(fd);
            return;
        }
        if (path == "/" || path == "/index.html") {
            send_file(fd, "index.html");
        } else if (path.rfind("/api/status", 0) == 0) {
            send_status(fd);
        } else if (path.rfind("/api/record", 0) == 0) {
            handle_record(fd, parse_seconds(path), parse_format(path));
        } else if (path.rfind("/api/long_record/start", 0) == 0) {
            handle_long_record_start(fd, parse_format(path));
        } else if (path.rfind("/api/long_record/stop", 0) == 0) {
            handle_long_record_stop(fd);
        } else if (path.rfind("/api/long_record/download", 0) == 0) {
            handle_long_record_download(fd);
        } else if (path.rfind("/api/play", 0) == 0) {
            handle_play(fd);
        } else if (path.rfind("/api/sweep", 0) == 0) {
            handle_sweep(fd);
        } else if (path.rfind("/api/stop", 0) == 0) {
            handle_stop(fd);
        } else if (path.rfind("/api/volume", 0) == 0) {
            handle_volume(fd, parse_volume(path));
        } else {
            send_file(fd, path.size() > 1 ? path.substr(1) : "index.html");
        }
        close(fd);
    }

    void send_file(int fd, const std::string &relative)
    {
        if (relative.find("..") != std::string::npos) {
            send_text(fd, 404, "text/plain; charset=utf-8", "Not found\n");
            return;
        }
        std::vector<uint8_t> data;
        std::string path = config_.web_root + "/" + relative;
        if (!read_file(path, &data)) {
            send_text(fd, 404, "text/plain; charset=utf-8", "Not found\n");
            return;
        }
        send_response(fd, 200, content_type(path), data);
    }

    void send_status(int fd)
    {
        ehal_audio_stats_t stats{};
        if (audio_ != nullptr) {
            (void)ehal_audio_get_stats(audio_, &stats);
        }
        std::ostringstream out;
        out << "{\"ok\":true,\"running\":" << (running_ ? "true" : "false")
            << ",\"capture_frames\":" << stats.capture_frames
            << ",\"capture_bytes\":" << stats.capture_bytes
            << ",\"sample_rate\":16000,\"channels\":1,\"codec\":\""
            << (active_codec_ == EHAL_AUDIO_CODEC_OPUS ? "OPUS" : "PCM") << "\"}";
        send_text(fd, 200, "application/json; charset=utf-8", out.str());
    }

    void handle_record(int fd, int seconds, const std::string &format)
    {
        std::unique_lock<std::mutex> record_lock(record_mutex_, std::try_to_lock);
        if (!record_lock.owns_lock()) {
            send_text(fd, 409, "text/plain; charset=utf-8",
                      "audio operation is already running\n");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(playback_mutex_);
            if (playback_running_) {
                send_text(fd, 409, "text/plain; charset=utf-8",
                          "speaker playback is already running\n");
                return;
            }
        }
        {
            std::lock_guard<std::mutex> lock(audio_buffer_.mutex);
            audio_buffer_.encoded.clear();
            audio_buffer_.frames = 0;
            audio_buffer_.bytes = 0;
            audio_buffer_.stream_to_file = false;
        }

        ehal_audio_codec_t codec = format == "opus" ?
            EHAL_AUDIO_CODEC_OPUS : EHAL_AUDIO_CODEC_PCM;
        int ret = configure_audio(codec, 1, 0);
        active_codec_ = codec;
        if (ret == EHAL_OK) {
            ret = start_audio();
        }
        if (ret != EHAL_OK) {
            send_text(fd, 500, "text/plain; charset=utf-8",
                      std::string("audio start failed: ") + ehal_audio_error_string(ret) + "\n");
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        stop_audio();

        std::vector<uint8_t> encoded;
        {
            std::lock_guard<std::mutex> lock(audio_buffer_.mutex);
            encoded = audio_buffer_.encoded;
        }
        if (encoded.empty()) {
            send_text(fd, 500, "text/plain; charset=utf-8", "no microphone audio captured\n");
            return;
        }
        if (codec == EHAL_AUDIO_CODEC_OPUS) {
            std::vector<uint8_t> ogg = make_ogg_opus(encoded, 16000, 1);
            send_response(fd, 200, "audio/ogg", ogg);
        } else {
            std::vector<uint8_t> wav = make_wav_from_pcm(encoded, 16000, 1, 16);
            send_response(fd, 200, "audio/wav", wav);
        }
    }

    void handle_long_record_start(int fd, const std::string &format)
    {
        std::unique_lock<std::mutex> record_lock(record_mutex_, std::try_to_lock);
        if (!record_lock.owns_lock()) {
            send_text(fd, 409, "text/plain; charset=utf-8", "audio operation is already running\n");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(playback_mutex_);
            if (playback_running_) {
                send_text(fd, 409, "text/plain; charset=utf-8", "speaker playback is already running\n");
                return;
            }
        }
        ehal_audio_codec_t codec = format == "opus" ? EHAL_AUDIO_CODEC_OPUS : EHAL_AUDIO_CODEC_PCM;
        {
            std::lock_guard<std::mutex> lock(audio_buffer_.mutex);
            audio_buffer_.encoded.clear();
            audio_buffer_.frames = 0;
            audio_buffer_.bytes = 0;
            audio_buffer_.stream_file.close();
            long_record_path_ = std::string("/tmp/mic_long_recording.") + (format == "opus" ? "opus" : "pcm");
            audio_buffer_.stream_file.open(long_record_path_, std::ios::binary | std::ios::trunc);
            if (!audio_buffer_.stream_file.is_open()) {
                send_text(fd, 500, "text/plain; charset=utf-8", "cannot open long recording file\n");
                return;
            }
            audio_buffer_.stream_to_file = true;
        }
        int ret = configure_audio(codec, 1, 0);
        active_codec_ = codec;
        if (ret == EHAL_OK) ret = start_audio();
        if (ret != EHAL_OK) {
            send_text(fd, 500, "text/plain; charset=utf-8",
                      std::string("audio start failed: ") + ehal_audio_error_string(ret) + "\n");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(long_record_mutex_);
            long_record_started_ = std::chrono::steady_clock::now();
            long_record_format_ = format;
            long_record_lock_ = std::make_unique<std::unique_lock<std::mutex>>(std::move(record_lock));
        }
        send_text(fd, 200, "application/json; charset=utf-8",
                  std::string("{\"ok\":true,\"recording\":true,\"format\":\"") + format + "\"}\n");
    }

    void handle_long_record_stop(int fd)
    {
        std::unique_ptr<std::unique_lock<std::mutex>> held_lock;
        std::string format;
        uint64_t frames = 0;
        uint64_t bytes = 0;
        {
            std::lock_guard<std::mutex> lock(long_record_mutex_);
            if (!long_record_lock_) {
                send_text(fd, 409, "text/plain; charset=utf-8", "long recording is not running\n");
                return;
            }
            held_lock = std::move(long_record_lock_);
            format = long_record_format_;
        }
        stop_audio();
        {
            std::lock_guard<std::mutex> lock(audio_buffer_.mutex);
            frames = audio_buffer_.frames;
            bytes = audio_buffer_.bytes;
            audio_buffer_.stream_file.flush();
            audio_buffer_.stream_file.close();
            audio_buffer_.stream_to_file = false;
        }
        held_lock.reset();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - long_record_started_).count();
        send_text(fd, 200, "application/json; charset=utf-8",
                  std::string("{\"ok\":true,\"recording\":false,\"format\":\"") + format +
                      "\",\"duration_ms\":" + std::to_string(elapsed) +
                      ",\"frames\":" + std::to_string(frames) +
                      ",\"bytes\":" + std::to_string(bytes) +
                      ",\"download\":\"/api/long_record/download\"}\n");
    }

    void handle_long_record_download(int fd)
    {
        std::vector<uint8_t> data;
        std::string format;
        {
            std::lock_guard<std::mutex> lock(long_record_mutex_);
            if (long_record_lock_) {
                send_text(fd, 409, "text/plain; charset=utf-8", "stop the long recording before downloading\n");
                return;
            }
            format = long_record_format_;
        }
        std::ifstream file(long_record_path_, std::ios::binary);
        if (!file.is_open()) {
            send_text(fd, 404, "text/plain; charset=utf-8", "no saved long recording\n");
            return;
        }
        file.seekg(0, std::ios::end); const uint64_t raw_size = static_cast<uint64_t>(file.tellg()); file.seekg(0);
        const uint64_t output_size = format == "pcm" ? raw_size + 44U : raw_size;
        const std::string type = format == "opus" ? "application/octet-stream" : "audio/wav";
        const std::string name = format == "opus" ? "long-recording.opus" : "long-recording.wav";
        std::ostringstream header;
        header << "HTTP/1.1 200 OK\r\nContent-Type: " << type << "\r\nContent-Length: " << output_size
               << "\r\nContent-Disposition: attachment; filename=\"" << name << "\"\r\nConnection: close\r\n\r\n";
        std::string h = header.str();
        (void)send_all(fd, h.data(), h.size());
        if (format == "pcm") {
            std::vector<uint8_t> wav_header = make_wav_from_pcm({}, 16000, 1, 16);
            uint32_t size32 = static_cast<uint32_t>(raw_size); std::memcpy(&wav_header[4], &size32, 4); uint32_t riff = size32 + 36U; std::memcpy(&wav_header[40], &size32, 4); std::memcpy(&wav_header[4], &riff, 4);
            (void)send_all(fd, wav_header.data(), wav_header.size());
        }
        std::vector<uint8_t> chunk(64 * 1024);
        while (file) { file.read(reinterpret_cast<char *>(chunk.data()), chunk.size()); std::streamsize n = file.gcount(); if (n > 0 && !send_all(fd, chunk.data(), static_cast<size_t>(n))) break; }
    }

    static int parse_volume(const std::string &path)
    {
        size_t pos = path.find("value=");
        if (pos == std::string::npos) {
            return 100;
        }
        int volume = std::atoi(path.c_str() + pos + 6U);
        return std::max(0, std::min(100, volume));
    }

    void handle_play(int fd)
    {
        if (playback_running_.load(std::memory_order_acquire)) {
            send_text(fd, 409, "text/plain; charset=utf-8",
                      "speaker playback is already running\n");
            return;
        }
        if (playback_thread_.joinable()) {
            playback_thread_.join();
        }
        std::lock_guard<std::mutex> playback_lock(playback_mutex_);
        if (playback_running_.load(std::memory_order_acquire)) {
            send_text(fd, 409, "text/plain; charset=utf-8",
                      "speaker playback is already running\n");
            return;
        }
        if (!record_mutex_.try_lock()) {
            send_text(fd, 409, "text/plain; charset=utf-8",
                      "microphone recording is already running\n");
            return;
        }
        record_mutex_.unlock();

        int ret = configure_audio(EHAL_AUDIO_CODEC_PCM, 0, 1);
        if (ret == EHAL_OK) {
            ret = start_audio();
        }
        if (ret != EHAL_OK) {
            send_text(fd, 500, "text/plain; charset=utf-8",
                      std::string("speaker start failed: ") +
                          ehal_audio_error_string(ret) + "\n");
            return;
        }

        playback_running_.store(true, std::memory_order_release);
        ret = ehal_audio_play_wav_file(audio_, config_.playback_audio.c_str(), 1000);
        if (ret != EHAL_OK) {
            std::fprintf(stderr, "speaker playback failed: %s\n",
                         ehal_audio_error_string(ret));
        }
        stop_audio();
        playback_running_.store(false, std::memory_order_release);
        if (ret != EHAL_OK) {
            send_text(fd, 500, "text/plain; charset=utf-8",
                      std::string("speaker playback failed: ") +
                          ehal_audio_error_string(ret) + "\n");
            return;
        }
        ehal_audio_stats_t stats{};
        (void)ehal_audio_get_stats(audio_, &stats);
        send_text(fd, 200, "application/json; charset=utf-8",
                  std::string("{\"ok\":true,\"playing\":false,\"frames\":") +
                      std::to_string(stats.playback_frames) +
                      ",\"bytes\":" + std::to_string(stats.playback_bytes) + "}\n");
    }

    // Board self-test: record the microphone while the board speaker emits
    // the same calibrated digital tones.  Each tone is 3 s with a 0.5 s
    // silence gap; the returned WAV contains the complete capture.
    void handle_sweep(int fd)
    {
        std::unique_lock<std::mutex> record_lock(record_mutex_, std::try_to_lock);
        if (!record_lock.owns_lock()) {
            send_text(fd, 409, "text/plain; charset=utf-8", "audio operation is already running\n");
            return;
        }
        std::lock_guard<std::mutex> playback_lock(playback_mutex_);
        if (playback_running_) {
            send_text(fd, 409, "text/plain; charset=utf-8", "speaker playback is already running\n");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(audio_buffer_.mutex);
            audio_buffer_.encoded.clear(); audio_buffer_.frames = 0; audio_buffer_.bytes = 0;
        }
        int ret = configure_audio(EHAL_AUDIO_CODEC_PCM, 1, 1);
        if (ret == EHAL_OK) ret = start_audio();
        if (ret != EHAL_OK) {
            send_text(fd, 500, "text/plain; charset=utf-8", std::string("audio start failed: ") + ehal_audio_error_string(ret) + "\n");
            return;
        }
        playback_running_.store(true, std::memory_order_release);
        const uint32_t tones[] = {100, 500, 1000, 3000, 5000, 6000};
        for (uint32_t f : tones) {
            ret = ehal_audio_play_test_tone(audio_, f, 3000, 80);
            if (ret != EHAL_OK) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        (void)ehal_audio_stop_playback(audio_);
        stop_audio();
        playback_running_.store(false, std::memory_order_release);
        if (ret != EHAL_OK) {
            send_text(fd, 500, "text/plain; charset=utf-8", std::string("sweep failed: ") + ehal_audio_error_string(ret) + "\n");
            return;
        }
        std::vector<uint8_t> pcm;
        { std::lock_guard<std::mutex> lock(audio_buffer_.mutex); pcm = audio_buffer_.encoded; }
        if (pcm.empty()) { send_text(fd, 500, "text/plain; charset=utf-8", "no microphone audio captured\n"); return; }
        send_response(fd, 200, "audio/wav", make_wav_from_pcm(pcm, 16000, 1, 16));
    }

    void handle_stop(int fd)
    {
        int ret = ehal_audio_stop_playback(audio_);
        send_text(fd, ret == EHAL_OK ? 200 : 500,
                  "application/json; charset=utf-8",
                  ret == EHAL_OK ? "{\"ok\":true,\"playing\":false}\n" :
                                   "{\"ok\":false}\n");
    }

    void handle_volume(int fd, int volume)
    {
        input_volume_ = volume;
        output_volume_ = volume;
        int ret = EHAL_OK;
        if (running_) {
            if (record_enabled_) {
                ret = ehal_audio_set_input_volume(audio_, volume);
            }
            if (ret == EHAL_OK && playback_enabled_) {
                ret = ehal_audio_set_output_volume(audio_, volume);
            }
        }
        send_text(fd, ret == EHAL_OK ? 200 : 500,
                  "application/json; charset=utf-8",
                  ret == EHAL_OK ?
                      std::string("{\"ok\":true,\"volume\":") +
                          std::to_string(volume) + "}\n" :
                      "{\"ok\":false}\n");
    }

    int start_audio()
    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        if (running_) {
            return EHAL_OK;
        }
        int ret = ehal_audio_start(audio_);
        if (ret == EHAL_OK) {
            if (record_enabled_) {
                (void)ehal_audio_set_input_volume(audio_, input_volume_);
            }
            if (playback_enabled_) {
                (void)ehal_audio_set_output_volume(audio_, output_volume_);
            }
            running_ = true;
        }
        return ret;
    }

    void stop_audio()
    {
        std::lock_guard<std::mutex> lock(audio_mutex_);
        if (audio_ != nullptr && running_) {
            (void)ehal_audio_stop(audio_);
            running_ = false;
        }
    }

    Config config_;
    ehal_audio_t *audio_ = nullptr;
    AudioBuffer audio_buffer_;
    std::mutex audio_mutex_;
    std::mutex record_mutex_;
    std::mutex playback_mutex_;
    std::mutex long_record_mutex_;
    std::unique_ptr<std::unique_lock<std::mutex>> long_record_lock_;
    std::chrono::steady_clock::time_point long_record_started_{};
    std::string long_record_format_ = "pcm";
    std::string long_record_path_;
    std::thread playback_thread_;
    bool running_ = false;
    bool record_enabled_ = false;
    bool playback_enabled_ = false;
    std::atomic<bool> playback_running_{false};
    int input_volume_ = 50;
    int output_volume_ = 100;
    ehal_audio_codec_t active_codec_ = EHAL_AUDIO_CODEC_PCM;
};

Config parse_args(int argc, char **argv)
{
    Config config;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            config.port = std::atoi(argv[++i]);
        } else if (arg == "--web-root" && i + 1 < argc) {
            config.web_root = argv[++i];
        } else if (arg == "--default-config" && i + 1 < argc) {
            config.default_config = argv[++i];
        } else if (arg == "--pcm-runtime-config" && i + 1 < argc) {
            config.pcm_runtime_config = argv[++i];
        } else if (arg == "--opus-runtime-config" && i + 1 < argc) {
            config.opus_runtime_config = argv[++i];
        } else if (arg == "--playback-audio" && i + 1 < argc) {
            config.playback_audio = argv[++i];
        }
    }
    return config;
}

} // namespace

int main(int argc, char **argv)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    // A client may close the WAV download immediately after receiving it.
    // Never let that socket condition terminate the whole demo process.
    signal(SIGPIPE, SIG_IGN);
    Config config = parse_args(argc, argv);
    MicWebDemo demo(config);
    int ret = demo.configure_audio(EHAL_AUDIO_CODEC_PCM, 1, 0);
    if (ret != EHAL_OK) {
        std::fprintf(stderr, "init audio failed: %s\n", ehal_audio_error_string(ret));
        return ret;
    }
    return demo.run();
}
