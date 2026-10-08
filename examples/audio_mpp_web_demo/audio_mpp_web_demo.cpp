#include "ss_mpi_audio.h"
#include "ot_acodec.h"
#include "ot_common.h"
#include "ot_common_aio.h"
#include "ot_common_aenc.h"
#include "ot_common_adec.h"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

/*
 * 独立的厂商 MPP 音频网页 demo。
 *
 * 设计目标：
 *   1. 不调用 ehal_audio/libhal 封装接口；本文件里的音频操作
 *      只通过厂商 ss_mpi_* 接口或 /dev/acodec ioctl 完成。
 *   2. 通过 HTTP 暴露厂商音频接口，方便网页配置、执行和读回
 *      AI/AO/AENC/ADEC/VQE/ACODEC 参数。
 *   3. 普通录放 demo 固定使用 16kHz / 16bit / 每帧 320 点。
 *      高级配置页仍允许提交厂商其它枚举值，用于接口验证。
 *
 *
 * 程序使用一个很小的内置 HTTP 服务，不额外引入 Web 框架依赖，
 * 这样交付包更简单。请求解析器只处理当前网页需要的 GET/POST 表单。
 *
 */

/* demo 固定参数：16kHz 下 320 个采样点正好是 20ms 音频帧。 */
constexpr uint32_t kSampleRate = 16000;
constexpr uint32_t kFrameSamples = 320;
constexpr uint16_t kBits = 16;
constexpr int kAiDev = 0;
constexpr int kAiChn = 0;
constexpr int kAoDev = 0;
constexpr int kAoChn = 0;
constexpr const char *kLeftPcm = "/tmp/audio_mpp_left.pcm";
constexpr const char *kRightPcm = "/tmp/audio_mpp_right.pcm";

std::atomic<bool> g_running{true};

void on_signal(int) { g_running = false; }

/* 内置 HTTP 服务使用的最小请求模型。 */
struct Request {
    std::string method;
    std::string path;
    std::string query;
    std::string body;
};

/*
 * 共享运行状态。
 *
 * MPP 音频设备在板端是进程外的硬件状态。这里保存本进程成功执行
 * 过的操作镜像，供 /api/state 展示。这个镜像不替代厂商 get_* 接口；
 * 网页上的所有 get_* 按钮都会直接调用真实 MPP 读回接口。
 *
 */
struct AudioState {
    std::mutex mu;
    bool audio_initialized = false;
    bool ai_enabled = false;
    bool ai_chn_enabled = false;
    bool ao_enabled = false;
    bool ao_chn_enabled = false;
    bool recording = false;
    bool playing = false;
    std::string record_input;
    std::thread record_thread;
    std::thread play_thread;
    std::atomic<bool> record_stop{false};
    std::atomic<bool> play_stop{false};
    uint64_t record_frames = 0;
    uint64_t record_bytes = 0;
    uint64_t play_frames = 0;
    uint64_t play_bytes = 0;
    bool left_saved = false;
    bool right_saved = false;
    ot_aio_attr ai_attr{};
    ot_aio_attr ao_attr{};
    ot_ai_chn_attr ai_chn_attr{};
    ot_ai_chn_param ai_chn_param{};
    ot_audio_track_mode ai_track = OT_AUDIO_TRACK_NORMAL;
    ot_audio_track_mode ao_track = OT_AUDIO_TRACK_NORMAL;
    int ao_volume_db = 0;
    bool ao_mute = false;
    int acodec_boost_l = 1;
    int acodec_boost_r = 1;
    int acodec_gain_micl = 31;
    int acodec_gain_micr = 31;
    int acodec_input_volume_db = 80;
    int acodec_actual_gain_micl = -1;
    int acodec_actual_gain_micr = -1;
    int acodec_actual_input_volume_db = -1;
    int last_record_error = 0;
    std::string last_record_error_text;
};

AudioState g_state;
std::mutex g_diag_mu;
std::string g_last_detail_error;

/* 保存最近一次底层调用失败详情，供 HTTP 返回和 /api/state 展示。 */
void set_detail_error(const std::string &text)
{
    std::lock_guard<std::mutex> lock(g_diag_mu);
    g_last_detail_error = text;
}

/* 取出最近一次底层调用失败详情；没有详情时使用调用方传入的兜底文本。 */
std::string get_detail_error(const std::string &fallback)
{
    std::lock_guard<std::mutex> lock(g_diag_mu);
    return g_last_detail_error.empty() ? fallback : g_last_detail_error;
}

/* 清空底层调用失败详情，避免旧错误影响下一次接口返回。 */
void clear_detail_error()
{
    std::lock_guard<std::mutex> lock(g_diag_mu);
    g_last_detail_error.clear();
}

/* 读取整数参数；参数缺失时返回确定的默认值。 */
int to_int(const std::map<std::string, std::string> &q, const std::string &key, int def)
{
    auto it = q.find(key);
    if (it == q.end() || it->second.empty()) return def;
    return std::strtol(it->second.c_str(), nullptr, 0);
}

/* 从 query/body 表单参数中读取字符串。 */
std::string get_str(const std::map<std::string, std::string> &q, const std::string &key, const std::string &def)
{
    auto it = q.find(key);
    return it == q.end() ? def : it->second;
}

/* 解码 GET query 和 POST 表单里的 URL 转义字符。 */
std::string url_decode(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char hex[3] = {s[i + 1], s[i + 2], 0};
            out.push_back(static_cast<char>(std::strtol(hex, nullptr, 16)));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

/* 把 application/x-www-form-urlencoded 数据解析成键值表。 */
std::map<std::string, std::string> parse_params(const std::string &s)
{
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t amp = s.find('&', pos);
        std::string item = s.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (!item.empty()) {
            size_t eq = item.find('=');
            std::string key = url_decode(item.substr(0, eq));
            std::string val = eq == std::string::npos ? "" : url_decode(item.substr(eq + 1));
            out[key] = val;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return out;
}

/* 合并 URL query 参数和 POST 表单 body 参数。 */
std::map<std::string, std::string> request_params(const Request &r)
{
    auto q = parse_params(r.query);
    auto b = parse_params(r.body);
    q.insert(b.begin(), b.end());
    return q;
}

/* 字符串写入 JSON 响应前先做转义。 */
std::string json_escape(const std::string &s)
{
    std::ostringstream o;
    for (char c : s) {
        if (c == '"' || c == '\\') o << '\\' << c;
        else if (c == '\n') o << "\\n";
        else if (c == '\r') o << "\\r";
        else o << c;
    }
    return o.str();
}

/* 标准成功响应工具；extra 为不含首逗号的 JSON 字段列表。 */
std::string ok_json(const std::string &extra = "")
{
    return std::string("{\"ok\":true") + (extra.empty() ? "" : "," + extra) + "}\n";
}

/* 标准错误响应工具，会附带厂商返回码和 errno 上下文。 */
std::string err_json(const std::string &msg, int ret = 0)
{
    std::ostringstream o;
    o << "{\"ok\":false,\"error\":\"" << json_escape(msg) << "\"";
    if (ret != 0) o << ",\"ret\":" << ret;
    if (errno != 0) o << ",\"errno\":" << errno << ",\"strerror\":\"" << json_escape(std::strerror(errno)) << "\"";
    o << "}\n";
    return o.str();
}

/* send(2) 包装函数：持续发送直到写完或连接关闭。 */
void send_all(int fd, const void *data, size_t size)
{
    const char *p = static_cast<const char *>(data);
    while (size > 0) {
        ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
        if (n <= 0) return;
        p += n;
        size -= static_cast<size_t>(n);
    }
}

/* 发送文本响应；所有 API JSON 响应都走这个路径。 */
void reply(int fd, int code, const std::string &type, const std::string &body)
{
    std::ostringstream h;
    h << "HTTP/1.1 " << code << (code == 200 ? " OK" : " Error")
      << "\r\nContent-Type: " << type
      << "\r\nContent-Length: " << body.size()
      << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    const auto header = h.str();
    send_all(fd, header.data(), header.size());
    send_all(fd, body.data(), body.size());
}

/* 发送二进制响应；WAV 下载走这个路径。 */
void reply_bin(int fd, int code, const std::string &type, const std::vector<uint8_t> &body)
{
    std::ostringstream h;
    h << "HTTP/1.1 " << code << (code == 200 ? " OK" : " Error")
      << "\r\nContent-Type: " << type
      << "\r\nContent-Length: " << body.size()
      << "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    const auto header = h.str();
    send_all(fd, header.data(), header.size());
    if (!body.empty()) send_all(fd, body.data(), body.size());
}

/*
 * 从 socket 中解析一个 HTTP 请求。
 *
 * 当前 demo 页面只提交较小的表单数据，固定缓冲区足够覆盖控件参数。
 * 如果 body 分包到达，会按 Content-Length 继续读取。
 *
 */
bool parse_request(int fd, Request *req)
{
    char buf[32768]{};
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return false;
    std::string raw(buf, static_cast<size_t>(n));
    size_t header_end = raw.find("\r\n\r\n");
    std::string head = header_end == std::string::npos ? raw : raw.substr(0, header_end);
    req->body = header_end == std::string::npos ? "" : raw.substr(header_end + 4);
    std::istringstream in(head);
    std::string target;
    in >> req->method >> target;
    size_t qm = target.find('?');
    req->path = target.substr(0, qm);
    req->query = qm == std::string::npos ? "" : target.substr(qm + 1);
    size_t content_len = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string key = "Content-Length:";
        if (line.rfind(key, 0) == 0) content_len = static_cast<size_t>(std::strtoul(line.c_str() + key.size(), nullptr, 10));
    }
    while (req->body.size() < content_len) {
        n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        req->body.append(buf, static_cast<size_t>(n));
    }
    return !req->method.empty() && !req->path.empty();
}

/* 把网页输入的采样率数字转换成厂商枚举。 */
ot_audio_sample_rate sample_rate_from_int(int v)
{
    switch (v) {
    case 8000: return OT_AUDIO_SAMPLE_RATE_8000;
    case 11025: return OT_AUDIO_SAMPLE_RATE_11025;
    case 12000: return OT_AUDIO_SAMPLE_RATE_12000;
    case 16000: return OT_AUDIO_SAMPLE_RATE_16000;
    case 22050: return OT_AUDIO_SAMPLE_RATE_22050;
    case 24000: return OT_AUDIO_SAMPLE_RATE_24000;
    case 32000: return OT_AUDIO_SAMPLE_RATE_32000;
    case 44100: return OT_AUDIO_SAMPLE_RATE_44100;
    case 48000: return OT_AUDIO_SAMPLE_RATE_48000;
    case 64000: return OT_AUDIO_SAMPLE_RATE_64000;
    case 96000: return OT_AUDIO_SAMPLE_RATE_96000;
    default: return OT_AUDIO_SAMPLE_RATE_16000;
    }
}

/* 把网页输入的位宽数字转换成厂商枚举。 */
ot_audio_bit_width bit_width_from_int(int v)
{
    if (v == 8) return OT_AUDIO_BIT_WIDTH_8;
    if (v == 24) return OT_AUDIO_BIT_WIDTH_24;
    return OT_AUDIO_BIT_WIDTH_16;
}

/* 把网页单双声道选择转换成厂商枚举。 */
ot_audio_snd_mode snd_mode_from_int(int v)
{
    return v == 1 ? OT_AUDIO_SOUND_MODE_STEREO : OT_AUDIO_SOUND_MODE_MONO;
}

/* 转换 AIO 工作模式前先校验枚举范围。 */
ot_aio_mode aio_mode_from_int(int v)
{
    if (v < OT_AIO_MODE_I2S_MASTER || v >= OT_AIO_MODE_BUTT) return OT_AIO_MODE_I2S_MASTER;
    return static_cast<ot_aio_mode>(v);
}

/* 转换 I2S 类型前先校验枚举范围。 */
ot_aio_i2s_type i2s_type_from_int(int v)
{
    if (v < OT_AIO_I2STYPE_INNERCODEC || v > OT_AIO_I2STYPE_DMIC) return OT_AIO_I2STYPE_INNERCODEC;
    return static_cast<ot_aio_i2s_type>(v);
}

/* 调用 ss_mpi_*_set_track_mode 前先校验 track mode 范围。 */
ot_audio_track_mode track_from_int(int v)
{
    if (v < OT_AUDIO_TRACK_NORMAL || v >= OT_AUDIO_TRACK_BUTT) return OT_AUDIO_TRACK_NORMAL;
    return static_cast<ot_audio_track_mode>(v);
}

/* AENC/ADEC 负载类型限制为页面上暴露的音频类型。 */
ot_payload_type payload_from_int(int v)
{
    switch (v) {
    case OT_PT_G711A:
    case OT_PT_G711U:
    case OT_PT_G726:
    case OT_PT_LPCM:
    case OT_PT_ADPCMA:
        return static_cast<ot_payload_type>(v);
    default:
        return OT_PT_LPCM;
    }
}

/*
 * 根据表单字段构造 ot_aio_attr。
 *
 * AI 和 AO 的公共属性都使用这个结构。表单字段名尽量与厂商
 * 结构体字段保持一致，方便从网页请求直接映射到 MPP 参数。
 *
 */
ot_aio_attr make_aio_attr(const std::map<std::string, std::string> &q)
{
    ot_aio_attr a{};
    a.sample_rate = sample_rate_from_int(to_int(q, "sample_rate", static_cast<int>(kSampleRate)));
    a.bit_width = bit_width_from_int(to_int(q, "bit_width", kBits));
    a.work_mode = aio_mode_from_int(to_int(q, "work_mode", OT_AIO_MODE_I2S_MASTER));
    a.snd_mode = snd_mode_from_int(to_int(q, "snd_mode", OT_AUDIO_SOUND_MODE_MONO));
    a.expand_flag = static_cast<td_u32>(to_int(q, "expand_flag", 0));
    a.frame_num = static_cast<td_u32>(to_int(q, "frame_num", 10));
    a.point_num_per_frame = static_cast<td_u32>(to_int(q, "point_num_per_frame", kFrameSamples));
    a.chn_cnt = static_cast<td_u32>(to_int(q, "chn_cnt", 1));
    a.clk_share = static_cast<td_u32>(to_int(q, "clk_share", 0));
    a.i2s_type = i2s_type_from_int(to_int(q, "i2s_type", OT_AIO_I2STYPE_INNERCODEC));
    return a;
}

/* 把 ot_aio_attr 读回值渲染成 JSON 字段。 */
std::string aio_attr_json(const ot_aio_attr &a)
{
    std::ostringstream o;
    o << "\"sample_rate\":" << static_cast<int>(a.sample_rate)
      << ",\"bit_width\":" << (a.bit_width == OT_AUDIO_BIT_WIDTH_8 ? 8 : (a.bit_width == OT_AUDIO_BIT_WIDTH_24 ? 24 : 16))
      << ",\"work_mode\":" << static_cast<int>(a.work_mode)
      << ",\"snd_mode\":" << static_cast<int>(a.snd_mode)
      << ",\"expand_flag\":" << a.expand_flag
      << ",\"frame_num\":" << a.frame_num
      << ",\"point_num_per_frame\":" << a.point_num_per_frame
      << ",\"chn_cnt\":" << a.chn_cnt
      << ",\"clk_share\":" << a.clk_share
      << ",\"i2s_type\":" << static_cast<int>(a.i2s_type);
    return o.str();
}

/* 整体读取文件用于 WAV 播放/下载；空结果表示文件不存在或长度为 0。 */
std::vector<uint8_t> read_file_bytes(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/* WAV 使用小端格式；这些工具函数负责追加小端整数。 */
void put_u16(std::vector<uint8_t> *v, uint16_t x)
{
    v->push_back(static_cast<uint8_t>(x & 0xff));
    v->push_back(static_cast<uint8_t>((x >> 8) & 0xff));
}

/* 向 WAV 缓冲区追加一个小端 32 位整数。 */
void put_u32(std::vector<uint8_t> *v, uint32_t x)
{
    put_u16(v, static_cast<uint16_t>(x & 0xffff));
    put_u16(v, static_cast<uint16_t>((x >> 16) & 0xffff));
}

/*
 * 给裸 PCM 添加标准 PCM WAV 头。
 *
 * demo 从 AI 采集到的是裸 16bit PCM，浏览器播放/下载前必须添加
 * 带有正确采样率和声道数的 RIFF/WAVE 头。
 */
std::vector<uint8_t> make_wav(const std::vector<uint8_t> &pcm, uint32_t rate, uint16_t channels)
{
    std::vector<uint8_t> wav;
    wav.insert(wav.end(), {'R','I','F','F'});
    put_u32(&wav, 36U + static_cast<uint32_t>(pcm.size()));
    wav.insert(wav.end(), {'W','A','V','E','f','m','t',' '});
    put_u32(&wav, 16);
    put_u16(&wav, 1);
    put_u16(&wav, channels);
    put_u32(&wav, rate);
    put_u32(&wav, rate * channels * kBits / 8U);
    put_u16(&wav, channels * kBits / 8U);
    put_u16(&wav, kBits);
    wav.insert(wav.end(), {'d','a','t','a'});
    put_u32(&wav, static_cast<uint32_t>(pcm.size()));
    wav.insert(wav.end(), pcm.begin(), pcm.end());
    return wav;
}

/*
 * 根据左右两次独立单声道录音生成双声道 WAV。
 *
 * 厂商 track-mode 接口把一个物理输入映射成单声道 AI 流。
 * 为了输出双声道文件，这里把最近一次左麦和右麦 PCM 按 L/R 交织。
 *
 */
std::vector<uint8_t> make_stereo_wav(const std::vector<uint8_t> &left, const std::vector<uint8_t> &right)
{
    size_t samples = std::max(left.size(), right.size()) / 2U;
    std::vector<uint8_t> stereo(samples * 4U, 0);
    for (size_t i = 0; i < samples; ++i) {
        if (i * 2U + 1U < left.size()) {
            stereo[i * 4U] = left[i * 2U];
            stereo[i * 4U + 1U] = left[i * 2U + 1U];
        }
        if (i * 2U + 1U < right.size()) {
            stereo[i * 4U + 2U] = right[i * 2U];
            stereo[i * 4U + 3U] = right[i * 2U + 1U];
        }
    }
    return make_wav(stereo, kSampleRate, 2);
}

/*
 * 停止并回收录音线程。
 *
 * 录音线程持有阻塞式 ss_mpi_ai_get_frame 循环。主线程通过 record_stop
 * 发出停止信号，并在状态锁外 join，避免线程退出时更新计数造成死锁。
 *
 */
void stop_record_thread()
{
    std::thread t;
    {
        std::lock_guard<std::mutex> lock(g_state.mu);
        g_state.record_stop.store(true);
        if (g_state.record_thread.joinable()) t = std::move(g_state.record_thread);
    }
    if (t.joinable()) t.join();
}

/* 停止并回收播放线程，同样遵守“不在锁内 join”的规则。 */
void stop_play_thread()
{
    std::thread t;
    {
        std::lock_guard<std::mutex> lock(g_state.mu);
        g_state.play_stop.store(true);
        if (g_state.play_thread.joinable()) t = std::move(g_state.play_thread);
    }
    if (t.joinable()) t.join();
}

/*
 * 为当前进程初始化一次厂商音频模块。
 *
 * 如果没有先调用 audio_init，部分 MPP 接口会返回 NOT_READY。
 * 网页仍暴露原始 init/exit 按钮；普通录放动作会调用此函数补齐启动序列。
 *
 */
int ensure_audio_init()
{
    std::lock_guard<std::mutex> lock(g_state.mu);
    if (g_state.audio_initialized) return TD_SUCCESS;
    int ret = ss_mpi_audio_init();
    if (ret == TD_SUCCESS) {
        g_state.audio_initialized = true;
    } else {
        set_detail_error("ss_mpi_audio_init 失败，返回码=" + std::to_string(ret));
    }
    return ret;
}

/*
 * 打开 ACODEC 控制句柄。
 *
 * 板端驱动在音频模块初始化后可能已经由 MPP 持有 /dev/acodec，再次 open
 * 会返回 EPERM。这里先扫描本进程已打开的文件描述符，找到同一个字符设备
 * 后 dup 一份出来使用；找不到时才直接 open。
 */
int open_acodec_control()
{
    struct stat codec_stat{};
    if (stat("/dev/acodec", &codec_stat) == 0) {
        DIR *fds = opendir("/proc/self/fd");
        if (fds != nullptr) {
            while (struct dirent *entry = readdir(fds)) {
                char *end = nullptr;
                long candidate = std::strtol(entry->d_name, &end, 10);
                if (end == entry->d_name || *end != '\0' || candidate < 0) continue;
                int fd = static_cast<int>(candidate);
                struct stat fd_stat{};
                if (fstat(fd, &fd_stat) == 0 && S_ISCHR(fd_stat.st_mode) &&
                    fd_stat.st_rdev == codec_stat.st_rdev &&
                    (fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR) {
                    int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
                    if (duplicate >= 0) {
                        closedir(fds);
                        return duplicate;
                    }
                }
            }
            closedir(fds);
        }
    }
    return open("/dev/acodec", O_RDWR | O_CLOEXEC);
}

/*
 * 配置板载 ACODEC 的输入模拟链路。
 *
 * 录音失败时板端表现为 AI 能启动但没有帧输出。现有可工作的音频封装层
 * 在 AI 启动前会配置 boost、MIC PGA 和输入音量；这个底层 demo 不使用
 * 封装库，因此也必须自己完成相同的硬件初始化。
 */
int configure_acodec_input()
{
    int fd = open_acodec_control();
    if (fd < 0) {
        int saved = errno;
        set_detail_error("打开 /dev/acodec 失败，errno=" + std::to_string(saved) + "，原因=" + std::strerror(saved));
        return -saved;
    }

    int boost_l_cfg = 1;
    int boost_r_cfg = 1;
    int gain_l_cfg = 31;
    int gain_r_cfg = 31;
    int input_volume_cfg = 80;
    {
        /*
         * 录音启动路径使用网页里保存的 ACODEC 输入参数。
         * 这里先复制到局部变量，避免 ioctl 慢操作期间长时间持有状态锁。
         */
        std::lock_guard<std::mutex> lock(g_state.mu);
        boost_l_cfg = std::max(0, std::min(g_state.acodec_boost_l, 1));
        boost_r_cfg = std::max(0, std::min(g_state.acodec_boost_r, 1));
        gain_l_cfg = std::max(0, std::min(g_state.acodec_gain_micl, 31));
        gain_r_cfg = std::max(0, std::min(g_state.acodec_gain_micr, 31));
        input_volume_cfg = std::max(0, std::min(g_state.acodec_input_volume_db, 80));
    }
    td_u32 boost_l = static_cast<td_u32>(boost_l_cfg);
    td_u32 boost_r = static_cast<td_u32>(boost_r_cfg);
    td_u32 input_volume = static_cast<td_u32>(input_volume_cfg);
    td_u32 unmute = 0;

    /*
     * 每一步失败都立即返回，并记录具体 ioctl 名称。
     * 这样网页上可以直接看到是 open、采样率、boost、增益、音量还是静音失败。
     * 16kHz 采样率由 ss_mpi_ai_set_pub_attr 设置；板端实测启动流程里直接
     * 调用 OT_ACODEC_SET_I2S1_FS 会被驱动返回 EPERM，因此录音启动路径不写这个 ioctl。
     */
    auto call = [&](unsigned long req, void *arg, const char *name) -> int {
        if (ioctl(fd, req, arg) == 0) return 0;
        int saved = errno;
        set_detail_error(std::string("ACODEC 输入配置失败：") + name +
                         "，errno=" + std::to_string(saved) +
                         "，原因=" + std::strerror(saved));
        return -saved;
    };

    int ret = 0;
    if ((ret = call(OT_ACODEC_ENABLE_BOOSTL, &boost_l, "OT_ACODEC_ENABLE_BOOSTL")) != 0) { close(fd); return ret; }
    if ((ret = call(OT_ACODEC_ENABLE_BOOSTR, &boost_r, "OT_ACODEC_ENABLE_BOOSTR")) != 0) { close(fd); return ret; }

    /*
     * 麦克风 PGA 增益沿用已验证 demo 的兼容写法：从最高请求值向下尝试。
     * 板端驱动会拒绝部分增益档位，固定写 0x1f 会返回 EPERM；降档后以
     * 驱动接受的实际值为准。
     */
    auto set_gain_compat = [&](unsigned long req, td_u32 requested, int *actual, const char *name) -> int {
        int saved = 0;
        for (td_u32 current = requested;; --current) {
            if (ioctl(fd, req, &current) == 0) {
                *actual = static_cast<int>(current);
                return 0;
            }
            saved = errno;
            if (current == 0U) break;
        }
        set_detail_error(std::string("ACODEC 输入配置失败：") + name +
                         "，已从请求值 " + std::to_string(requested) + " 降档到 0 均被驱动拒绝，errno=" +
                         std::to_string(saved) + "，原因=" + std::strerror(saved));
        return -saved;
    };
    int actual_gain_l = -1;
    int actual_gain_r = -1;
    if ((ret = set_gain_compat(OT_ACODEC_SET_GAIN_MICL, static_cast<td_u32>(gain_l_cfg), &actual_gain_l, "OT_ACODEC_SET_GAIN_MICL")) != 0) { close(fd); return ret; }
    if ((ret = set_gain_compat(OT_ACODEC_SET_GAIN_MICR, static_cast<td_u32>(gain_r_cfg), &actual_gain_r, "OT_ACODEC_SET_GAIN_MICR")) != 0) { close(fd); return ret; }
    if ((ret = call(OT_ACODEC_SET_INPUT_VOLUME, &input_volume, "OT_ACODEC_SET_INPUT_VOLUME")) != 0) { close(fd); return ret; }
    if ((ret = call(OT_ACODEC_SET_MICL_MUTE, &unmute, "OT_ACODEC_SET_MICL_MUTE")) != 0) { close(fd); return ret; }
    if ((ret = call(OT_ACODEC_SET_MICR_MUTE, &unmute, "OT_ACODEC_SET_MICR_MUTE")) != 0) { close(fd); return ret; }

    td_u32 readback = 0;
    if (ioctl(fd, OT_ACODEC_GET_GAIN_MICL, &readback) == 0) actual_gain_l = static_cast<int>(readback);
    if (ioctl(fd, OT_ACODEC_GET_GAIN_MICR, &readback) == 0) actual_gain_r = static_cast<int>(readback);
    if (ioctl(fd, OT_ACODEC_GET_INPUT_VOLUME, &readback) == 0) input_volume_cfg = static_cast<int>(readback);
    {
        /* 保存硬件读回值，页面状态区可以显示真实生效结果。 */
        std::lock_guard<std::mutex> lock(g_state.mu);
        g_state.acodec_actual_gain_micl = actual_gain_l;
        g_state.acodec_actual_gain_micr = actual_gain_r;
        g_state.acodec_actual_input_volume_db = input_volume_cfg;
    }

    close(fd);
    return 0;
}

/*
 * 配置板载 ACODEC 的输出模拟链路。
 *
 * MPP 的 AO 音量只在 AO 设备启用后才能稳定设置；ACODEC 的输出通路
 * 则负责 DAC 侧不静音、输出音量处于可听状态。
 */
int configure_acodec_output()
{
    int fd = open_acodec_control();
    if (fd < 0) {
        int saved = errno;
        set_detail_error("打开 /dev/acodec 失败，errno=" + std::to_string(saved) + "，原因=" + std::strerror(saved));
        return -saved;
    }

    td_s32 output_db = 0;
    td_u32 unmute = 0;

    /*
     * 输出链路也按单步返回，便于定位扬声器硬件配置失败项。
     * 16kHz 采样率由 ss_mpi_ao_set_pub_attr 设置；这里不写 I2S1_FS，
     * 保持和已验证可工作的 ACODEC 输出配置方式一致。
     */
    auto call = [&](unsigned long req, void *arg, const char *name) -> int {
        if (ioctl(fd, req, arg) == 0) return 0;
        int saved = errno;
        set_detail_error(std::string("ACODEC 输出配置失败：") + name +
                         "，errno=" + std::to_string(saved) +
                         "，原因=" + std::strerror(saved));
        return -saved;
    };

    int ret = 0;
    if ((ret = call(OT_ACODEC_SET_OUTPUT_VOLUME, &output_db, "OT_ACODEC_SET_OUTPUT_VOLUME")) != 0) { close(fd); return ret; }
    if ((ret = call(OT_ACODEC_SET_DACL_MUTE, &unmute, "OT_ACODEC_SET_DACL_MUTE")) != 0) { close(fd); return ret; }
    if ((ret = call(OT_ACODEC_SET_DACR_MUTE, &unmute, "OT_ACODEC_SET_DACR_MUTE")) != 0) { close(fd); return ret; }

    close(fd);
    return 0;
}

/*
 * 按 16kHz demo 路径配置并启动 AI。
 *
 * 启动顺序：
 *   1. ss_mpi_audio_init
 *   2. ss_mpi_ai_set_pub_attr 设置 16k/16bit/mono/320 点
 *   3. ss_mpi_ai_set_chn_param 设置用户帧深度
 *   4. 启用 AI 设备和通道
 *   5. ss_mpi_ai_set_track_mode 选择左/右物理输入
 *   6. ss_mpi_ai_get_track_mode 读回核对
 *
 * 读回核对可以给出明确的麦克风选择成功/失败结果，避免只写不验。
 *
 */
int ensure_ai_started(ot_audio_track_mode track)
{
    clear_detail_error();
    int ret = ensure_audio_init();
    if (ret != TD_SUCCESS) return ret;
    ot_aio_attr attr{};
    attr.sample_rate = OT_AUDIO_SAMPLE_RATE_16000;
    attr.bit_width = OT_AUDIO_BIT_WIDTH_16;
    attr.work_mode = OT_AIO_MODE_I2S_MASTER;
    attr.snd_mode = OT_AUDIO_SOUND_MODE_MONO;
    attr.frame_num = 10;
    attr.point_num_per_frame = kFrameSamples;
    attr.chn_cnt = 1;
    attr.clk_share = 1;
    attr.i2s_type = OT_AIO_I2STYPE_INNERCODEC;
    ret = ss_mpi_ai_set_pub_attr(kAiDev, &attr);
    if (ret != TD_SUCCESS) {
        set_detail_error("ss_mpi_ai_set_pub_attr 失败，返回码=" + std::to_string(ret));
        return ret;
    }
    ot_ai_chn_param p{};
    p.usr_frame_depth = 8;
    (void)ss_mpi_ai_set_chn_param(kAiDev, kAiChn, &p);
    ret = ss_mpi_ai_enable(kAiDev);
    if (ret != TD_SUCCESS) {
        set_detail_error("ss_mpi_ai_enable 失败，返回码=" + std::to_string(ret));
        return ret;
    }
    ret = ss_mpi_ai_enable_chn(kAiDev, kAiChn);
    if (ret != TD_SUCCESS) {
        (void)ss_mpi_ai_disable(kAiDev);
        set_detail_error("ss_mpi_ai_enable_chn 失败，返回码=" + std::to_string(ret));
        return ret;
    }
    ret = ss_mpi_ai_set_track_mode(kAiDev, track);
    if (ret != TD_SUCCESS) {
        (void)ss_mpi_ai_disable_chn(kAiDev, kAiChn);
        (void)ss_mpi_ai_disable(kAiDev);
        set_detail_error("ss_mpi_ai_set_track_mode 失败，返回码=" + std::to_string(ret));
        return ret;
    }
    ot_audio_track_mode readback = OT_AUDIO_TRACK_NORMAL;
    ret = ss_mpi_ai_get_track_mode(kAiDev, &readback);
    if (ret != TD_SUCCESS || readback != track) {
        (void)ss_mpi_ai_disable_chn(kAiDev, kAiChn);
        (void)ss_mpi_ai_disable(kAiDev);
        set_detail_error(ret != TD_SUCCESS ?
            "ss_mpi_ai_get_track_mode 失败，返回码=" + std::to_string(ret) :
            "ss_mpi_ai_get_track_mode 读回值与请求值不一致，请求值=" + std::to_string(static_cast<int>(track)) +
                "，读回值=" + std::to_string(static_cast<int>(readback)));
        return ret != TD_SUCCESS ? ret : TD_FAILURE;
    }
    ret = configure_acodec_input();
    if (ret != 0) {
        (void)ss_mpi_ai_disable_chn(kAiDev, kAiChn);
        (void)ss_mpi_ai_disable(kAiDev);
        return ret;
    }
    {
        std::lock_guard<std::mutex> lock(g_state.mu);
        g_state.ai_enabled = true;
        g_state.ai_chn_enabled = true;
        g_state.ai_attr = attr;
        g_state.ai_track = track;
    }
    return TD_SUCCESS;
}

/* 先停止 AI 通道，再停止 AI 设备，最后更新本进程状态镜像。 */
void stop_ai()
{
    (void)ss_mpi_ai_disable_chn(kAiDev, kAiChn);
    (void)ss_mpi_ai_disable(kAiDev);
    std::lock_guard<std::mutex> lock(g_state.mu);
    g_state.ai_chn_enabled = false;
    g_state.ai_enabled = false;
}

/*
 * 按 16kHz demo 路径配置并启动 AO。
 *
 * AO 与 AI 使用同一个 AIO 公共属性结构。扬声器 demo 直接通过
 * ss_mpi_ao_send_frame 发送 PCM 帧；WAV 先被解析为裸 PCM，
 * 因此普通 WAV 播放不需要 ADEC 链路。
 */
int ensure_ao_started()
{
    clear_detail_error();
    int ret = ensure_audio_init();
    if (ret != TD_SUCCESS) return ret;
    ret = configure_acodec_output();
    if (ret != 0) return ret;
    ot_aio_attr attr{};
    attr.sample_rate = OT_AUDIO_SAMPLE_RATE_16000;
    attr.bit_width = OT_AUDIO_BIT_WIDTH_16;
    attr.work_mode = OT_AIO_MODE_I2S_MASTER;
    attr.snd_mode = OT_AUDIO_SOUND_MODE_MONO;
    attr.frame_num = 10;
    attr.point_num_per_frame = kFrameSamples;
    attr.chn_cnt = 1;
    attr.clk_share = 1;
    attr.i2s_type = OT_AIO_I2STYPE_INNERCODEC;
    ret = ss_mpi_ao_set_pub_attr(kAoDev, &attr);
    if (ret != TD_SUCCESS) {
        set_detail_error("ss_mpi_ao_set_pub_attr 失败，返回码=" + std::to_string(ret));
        return ret;
    }
    ret = ss_mpi_ao_enable(kAoDev);
    if (ret != TD_SUCCESS) {
        set_detail_error("ss_mpi_ao_enable 失败，返回码=" + std::to_string(ret));
        return ret;
    }
    ret = ss_mpi_ao_enable_chn(kAoDev, kAoChn);
    if (ret != TD_SUCCESS) {
        (void)ss_mpi_ao_disable(kAoDev);
        set_detail_error("ss_mpi_ao_enable_chn 失败，返回码=" + std::to_string(ret));
        return ret;
    }
    int volume_db = 0;
    bool mute = false;
    {
        std::lock_guard<std::mutex> lock(g_state.mu);
        g_state.ao_enabled = true;
        g_state.ao_chn_enabled = true;
        g_state.ao_attr = attr;
        volume_db = g_state.ao_volume_db;
        mute = g_state.ao_mute;
    }
    (void)ss_mpi_ao_set_volume(kAoDev, volume_db);
    ot_audio_fade fade{};
    (void)ss_mpi_ao_set_mute(kAoDev, mute ? TD_TRUE : TD_FALSE, &fade);
    return TD_SUCCESS;
}

/* 按清缓存、停通道、停设备的顺序停止 AO 播放。 */
void stop_ao()
{
    (void)ss_mpi_ao_clr_chn_buf(kAoDev, kAoChn);
    (void)ss_mpi_ao_disable_chn(kAoDev, kAoChn);
    (void)ss_mpi_ao_disable(kAoDev);
    std::lock_guard<std::mutex> lock(g_state.mu);
    g_state.ao_chn_enabled = false;
    g_state.ao_enabled = false;
}

/*
 * 向 AO 发送一帧单声道 PCM。
 *
 * 单声道 PCM 使用厂商 frame 的 virt_addr[0] 和 len 字段。
 * 调用期间缓冲区由当前栈或 vector 持有，直到 ss_mpi_ao_send_frame 返回。
 *
 */
int send_pcm_frame(const int16_t *samples, uint32_t count)
{
    ot_audio_frame frame{};
    frame.bit_width = OT_AUDIO_BIT_WIDTH_16;
    frame.snd_mode = OT_AUDIO_SOUND_MODE_MONO;
    frame.virt_addr[0] = reinterpret_cast<td_u8 *>(const_cast<int16_t *>(samples));
    frame.len = count * sizeof(int16_t);
    return ss_mpi_ao_send_frame(kAoDev, kAoChn, &frame, 1000);
}

/*
 * 左/右麦录音线程。
 *
 * AI 返回的是厂商持有的帧缓冲。每一次成功 get_frame 后都必须
 * 调用 release_frame，即使数据已经复制到 PCM 文件。
 * PCM 文件保存在 /tmp，下载接口再按 mono 或 stereo 封装成 WAV。
 *
 */
void record_worker(std::string input, std::string path)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    while (!g_state.record_stop.load()) {
        ot_audio_frame frame{};
        ot_aec_frame aec{};
        int ret = ss_mpi_ai_get_frame(kAiDev, kAiChn, &frame, &aec, 1000);
        if (ret == TD_SUCCESS) {
            if (frame.virt_addr[0] != nullptr && frame.len > 0) {
                out.write(reinterpret_cast<const char *>(frame.virt_addr[0]), frame.len);
                std::lock_guard<std::mutex> lock(g_state.mu);
                ++g_state.record_frames;
                g_state.record_bytes += frame.len;
            }
            (void)ss_mpi_ai_release_frame(kAiDev, kAiChn, &frame, &aec);
        }
    }
    out.close();
    stop_ai();
    std::lock_guard<std::mutex> lock(g_state.mu);
    g_state.recording = false;
    if (input == "right") g_state.right_saved = true;
    else g_state.left_saved = true;
}

/*
 * 扬声器测试音线程。
 *
 * 生成 16kHz、16bit、单声道正弦波帧，并逐帧发送到 AO。
 * volume 参数用于缩放 PCM 幅值；AO 自身的 volume_db 仍可在 AO 页独立配置。
 *
 */
void tone_worker(uint32_t freq, uint32_t duration_ms, int volume)
{
    int ret = ensure_ao_started();
    if (ret == TD_SUCCESS) {
        const uint32_t total = kSampleRate * duration_ms / 1000U;
        uint32_t done = 0;
        while (done < total && !g_state.play_stop.load()) {
            uint32_t n = std::min<uint32_t>(kFrameSamples, total - done);
            std::vector<int16_t> buf(n);
            for (uint32_t i = 0; i < n; ++i) {
                double t = static_cast<double>(done + i) / static_cast<double>(kSampleRate);
                double s = std::sin(2.0 * M_PI * static_cast<double>(freq) * t);
                buf[i] = static_cast<int16_t>(s * 32767.0 * std::max(0, std::min(volume, 100)) / 100.0);
            }
            ret = send_pcm_frame(buf.data(), n);
            if (ret != TD_SUCCESS) break;
            {
                std::lock_guard<std::mutex> lock(g_state.mu);
                ++g_state.play_frames;
                g_state.play_bytes += n * sizeof(int16_t);
            }
            done += n;
        }
    }
    stop_ao();
    std::lock_guard<std::mutex> lock(g_state.mu);
    g_state.playing = false;
}

/*
 * 只解析本 demo 使用的 WAV 子集：PCM、16kHz、16bit、单声道。
 *
 * 这里故意严格校验格式。AO 被配置为固定格式，如果接受其它 WAV，
 * 就会形成隐含重采样/转换路径，影响扬声器测试结论。
 *
 */
bool parse_wav_pcm16_mono(const std::vector<uint8_t> &wav, std::vector<uint8_t> *pcm)
{
    if (wav.size() < 44 || std::memcmp(wav.data(), "RIFF", 4) != 0 || std::memcmp(wav.data() + 8, "WAVE", 4) != 0) return false;
    uint16_t fmt = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    size_t pos = 12;
    size_t data_pos = 0;
    uint32_t data_size = 0;
    while (pos + 8 <= wav.size()) {
        uint32_t size = wav[pos + 4] | (wav[pos + 5] << 8) | (wav[pos + 6] << 16) | (wav[pos + 7] << 24);
        if (std::memcmp(wav.data() + pos, "fmt ", 4) == 0 && size >= 16 && pos + 8 + size <= wav.size()) {
            const uint8_t *p = wav.data() + pos + 8;
            fmt = p[0] | (p[1] << 8);
            channels = p[2] | (p[3] << 8);
            rate = p[4] | (p[5] << 8) | (p[6] << 16) | (p[7] << 24);
            bits = p[14] | (p[15] << 8);
        } else if (std::memcmp(wav.data() + pos, "data", 4) == 0) {
            data_pos = pos + 8;
            data_size = size;
        }
        pos += 8 + size + (size & 1U);
    }
    if (fmt != 1 || channels != 1 || rate != kSampleRate || bits != 16 || data_pos == 0 || data_pos + data_size > wav.size()) return false;
    pcm->assign(wav.begin() + static_cast<long>(data_pos), wav.begin() + static_cast<long>(data_pos + data_size));
    return true;
}

/* 把已经解析好的单声道 PCM 按 320 点一帧送入 AO 播放。 */
void pcm_worker(std::vector<uint8_t> pcm)
{
    int ret = ensure_ao_started();
    if (ret == TD_SUCCESS) {
        size_t pos = 0;
        while (pos < pcm.size() && !g_state.play_stop.load()) {
            size_t bytes = std::min<size_t>(kFrameSamples * sizeof(int16_t), pcm.size() - pos);
            ret = send_pcm_frame(reinterpret_cast<const int16_t *>(pcm.data() + pos), static_cast<uint32_t>(bytes / 2U));
            if (ret != TD_SUCCESS) break;
            {
                std::lock_guard<std::mutex> lock(g_state.mu);
                ++g_state.play_frames;
                g_state.play_bytes += bytes;
            }
            pos += bytes;
        }
    }
    stop_ao();
    std::lock_guard<std::mutex> lock(g_state.mu);
    g_state.playing = false;
}

/*
 * 执行一个参数类型为 td_u32 的 /dev/acodec ioctl。
 *
 * 每次请求单独打开文件描述符，让 ACODEC 控制与 AI/AO 生命周期解耦。
 * close 前保存 errno，确保错误响应能报告真实失败原因。
 */
int acodec_ioctl_u32(unsigned long req, td_u32 *value)
{
    int fd = open("/dev/acodec", O_RDWR);
    if (fd < 0) return -1;
    int ret = ioctl(fd, req, value);
    int saved = errno;
    close(fd);
    errno = saved;
    return ret;
}

/* 执行参数类型为 ot_acodec_volume_ctrl 的 ACODEC 音量 ioctl。 */
int acodec_ioctl_volume(unsigned long req, ot_acodec_volume_ctrl *value)
{
    int fd = open("/dev/acodec", O_RDWR);
    if (fd < 0) return -1;
    int ret = ioctl(fd, req, value);
    int saved = errno;
    close(fd);
    errno = saved;
    return ret;
}

/*
 * API 总路由。
 *
 * 每个分支把一个网页接口映射到一个厂商 API 族。普通接口返回 JSON；
 * WAV 下载接口会填充 bin/content_type。所有 set/get 类接口都直接
 * 调用厂商 API；状态字段只是本进程成功动作的 UI 镜像。
 *
 */
std::string handle_api(const Request &req, int *code, std::string *content_type, std::vector<uint8_t> *bin)
{
    *code = 200;
    *content_type = "application/json; charset=utf-8";
    auto q = request_params(req);
    errno = 0;

    if (req.path == "/api/state") {
        std::lock_guard<std::mutex> lock(g_state.mu);
        std::ostringstream o;
        o << "{\"ok\":true,\"audio_initialized\":" << (g_state.audio_initialized ? "true" : "false")
          << ",\"ai_enabled\":" << (g_state.ai_enabled ? "true" : "false")
          << ",\"ai_chn_enabled\":" << (g_state.ai_chn_enabled ? "true" : "false")
          << ",\"ao_enabled\":" << (g_state.ao_enabled ? "true" : "false")
          << ",\"ao_chn_enabled\":" << (g_state.ao_chn_enabled ? "true" : "false")
          << ",\"recording\":" << (g_state.recording ? "true" : "false")
          << ",\"playing\":" << (g_state.playing ? "true" : "false")
          << ",\"record_input\":\"" << json_escape(g_state.record_input) << "\""
          << ",\"left_saved\":" << (g_state.left_saved ? "true" : "false")
          << ",\"right_saved\":" << (g_state.right_saved ? "true" : "false")
          << ",\"record_frames\":" << g_state.record_frames
          << ",\"record_bytes\":" << g_state.record_bytes
          << ",\"play_frames\":" << g_state.play_frames
          << ",\"play_bytes\":" << g_state.play_bytes
          << ",\"acodec_boost_l\":" << g_state.acodec_boost_l
          << ",\"acodec_boost_r\":" << g_state.acodec_boost_r
          << ",\"acodec_gain_micl\":" << g_state.acodec_gain_micl
          << ",\"acodec_gain_micr\":" << g_state.acodec_gain_micr
          << ",\"acodec_input_volume_db\":" << g_state.acodec_input_volume_db
          << ",\"acodec_actual_gain_micl\":" << g_state.acodec_actual_gain_micl
          << ",\"acodec_actual_gain_micr\":" << g_state.acodec_actual_gain_micr
          << ",\"acodec_actual_input_volume_db\":" << g_state.acodec_actual_input_volume_db
          << ",\"last_record_error\":" << g_state.last_record_error
          << ",\"last_record_error_text\":\"" << json_escape(g_state.last_record_error_text) << "\""
          << ",\"sample_rate\":16000,\"bits\":16,\"frame_samples\":320}\n";
        return o.str();
    }
    if (req.path == "/api/audio/init") {
        int ret = ensure_audio_init();
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_audio_init failed", ret);
    }
    if (req.path == "/api/audio/exit") {
        stop_record_thread();
        stop_play_thread();
        stop_ai();
        stop_ao();
        int ret = ss_mpi_audio_exit();
        std::lock_guard<std::mutex> lock(g_state.mu);
        g_state.audio_initialized = false;
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_audio_exit failed", ret);
    }
    if (req.path == "/api/audio/set_mod_param") {
        ot_audio_mod_param p{};
        p.clk_select = to_int(q, "clk_select", 0) == 1 ? OT_AUDIO_CLK_SELECT_SPARE : OT_AUDIO_CLK_SELECT_BASE;
        int ret = ss_mpi_audio_set_mod_param(&p);
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_audio_set_mod_param failed", ret);
    }
    if (req.path == "/api/audio/get_mod_param") {
        ot_audio_mod_param p{};
        int ret = ss_mpi_audio_get_mod_param(&p);
        if (ret != TD_SUCCESS) return err_json("ss_mpi_audio_get_mod_param failed", ret);
        return ok_json("\"clk_select\":" + std::to_string(static_cast<int>(p.clk_select)));
    }

    if (req.path == "/api/ai/set_pub_attr") {
        auto attr = make_aio_attr(q);
        int ret = ss_mpi_ai_set_pub_attr(to_int(q, "ai_dev", kAiDev), &attr);
        if (ret == TD_SUCCESS) { std::lock_guard<std::mutex> lock(g_state.mu); g_state.ai_attr = attr; }
        return ret == TD_SUCCESS ? ok_json(aio_attr_json(attr)) : err_json("ss_mpi_ai_set_pub_attr failed", ret);
    }
    if (req.path == "/api/ai/get_pub_attr") {
        ot_aio_attr attr{};
        int ret = ss_mpi_ai_get_pub_attr(to_int(q, "ai_dev", kAiDev), &attr);
        return ret == TD_SUCCESS ? ok_json(aio_attr_json(attr)) : err_json("ss_mpi_ai_get_pub_attr failed", ret);
    }
    if (req.path == "/api/ai/clr_pub_attr") {
        int ret = ss_mpi_ai_clr_pub_attr(to_int(q, "ai_dev", kAiDev));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ai_clr_pub_attr failed", ret);
    }
    if (req.path == "/api/ai/enable") {
        int ret = ss_mpi_ai_enable(to_int(q, "ai_dev", kAiDev));
        if (ret == TD_SUCCESS) { std::lock_guard<std::mutex> lock(g_state.mu); g_state.ai_enabled = true; }
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ai_enable failed", ret);
    }
    if (req.path == "/api/ai/disable") {
        int ret = ss_mpi_ai_disable(to_int(q, "ai_dev", kAiDev));
        if (ret == TD_SUCCESS) { std::lock_guard<std::mutex> lock(g_state.mu); g_state.ai_enabled = false; }
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ai_disable failed", ret);
    }
    if (req.path == "/api/ai/set_chn_attr") {
        ot_ai_chn_attr attr{};
        attr.mode = to_int(q, "mode", 0) == 1 ? OT_AI_CHN_MODE_FAST : OT_AI_CHN_MODE_NORMAL;
        attr.raw_frame_enable = to_int(q, "raw_frame_enable", 0) ? TD_TRUE : TD_FALSE;
        int ret = ss_mpi_ai_set_chn_attr(to_int(q, "ai_dev", kAiDev), to_int(q, "ai_chn", kAiChn), &attr);
        if (ret == TD_SUCCESS) { std::lock_guard<std::mutex> lock(g_state.mu); g_state.ai_chn_attr = attr; }
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ai_set_chn_attr failed", ret);
    }
    if (req.path == "/api/ai/get_chn_attr") {
        ot_ai_chn_attr attr{};
        int ret = ss_mpi_ai_get_chn_attr(to_int(q, "ai_dev", kAiDev), to_int(q, "ai_chn", kAiChn), &attr);
        if (ret != TD_SUCCESS) return err_json("ss_mpi_ai_get_chn_attr failed", ret);
        return ok_json("\"mode\":" + std::to_string(static_cast<int>(attr.mode)) + ",\"raw_frame_enable\":" + (attr.raw_frame_enable ? "true" : "false"));
    }
    if (req.path == "/api/ai/set_chn_param") {
        ot_ai_chn_param p{};
        p.usr_frame_depth = static_cast<td_u32>(to_int(q, "usr_frame_depth", 4));
        int ret = ss_mpi_ai_set_chn_param(to_int(q, "ai_dev", kAiDev), to_int(q, "ai_chn", kAiChn), &p);
        if (ret == TD_SUCCESS) { std::lock_guard<std::mutex> lock(g_state.mu); g_state.ai_chn_param = p; }
        return ret == TD_SUCCESS ? ok_json("\"usr_frame_depth\":" + std::to_string(p.usr_frame_depth)) : err_json("ss_mpi_ai_set_chn_param failed", ret);
    }
    if (req.path == "/api/ai/get_chn_param") {
        ot_ai_chn_param p{};
        int ret = ss_mpi_ai_get_chn_param(to_int(q, "ai_dev", kAiDev), to_int(q, "ai_chn", kAiChn), &p);
        return ret == TD_SUCCESS ? ok_json("\"usr_frame_depth\":" + std::to_string(p.usr_frame_depth)) : err_json("ss_mpi_ai_get_chn_param failed", ret);
    }
    if (req.path == "/api/ai/enable_chn") {
        int ret = ss_mpi_ai_enable_chn(to_int(q, "ai_dev", kAiDev), to_int(q, "ai_chn", kAiChn));
        if (ret == TD_SUCCESS) { std::lock_guard<std::mutex> lock(g_state.mu); g_state.ai_chn_enabled = true; }
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ai_enable_chn failed", ret);
    }
    if (req.path == "/api/ai/disable_chn") {
        int ret = ss_mpi_ai_disable_chn(to_int(q, "ai_dev", kAiDev), to_int(q, "ai_chn", kAiChn));
        if (ret == TD_SUCCESS) { std::lock_guard<std::mutex> lock(g_state.mu); g_state.ai_chn_enabled = false; }
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ai_disable_chn failed", ret);
    }
    if (req.path == "/api/ai/set_track_mode") {
        auto track = track_from_int(to_int(q, "track_mode", OT_AUDIO_TRACK_NORMAL));
        int ret = ss_mpi_ai_set_track_mode(to_int(q, "ai_dev", kAiDev), track);
        if (ret == TD_SUCCESS) { std::lock_guard<std::mutex> lock(g_state.mu); g_state.ai_track = track; }
        return ret == TD_SUCCESS ? ok_json("\"track_mode\":" + std::to_string(static_cast<int>(track))) : err_json("ss_mpi_ai_set_track_mode failed", ret);
    }
    if (req.path == "/api/ai/get_track_mode") {
        ot_audio_track_mode track{};
        int ret = ss_mpi_ai_get_track_mode(to_int(q, "ai_dev", kAiDev), &track);
        return ret == TD_SUCCESS ? ok_json("\"track_mode\":" + std::to_string(static_cast<int>(track))) : err_json("ss_mpi_ai_get_track_mode failed", ret);
    }
    if (req.path == "/api/ai/set_dmic_gain") {
        int ret = ss_mpi_ai_set_dmic_gain(to_int(q, "ai_dev", kAiDev), to_int(q, "gain", 0));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ai_set_dmic_gain failed", ret);
    }
    if (req.path == "/api/ai/get_dmic_gain") {
        td_s32 gain = 0;
        int ret = ss_mpi_ai_get_dmic_gain(to_int(q, "ai_dev", kAiDev), &gain);
        return ret == TD_SUCCESS ? ok_json("\"gain\":" + std::to_string(gain)) : err_json("ss_mpi_ai_get_dmic_gain failed", ret);
    }
    if (req.path == "/api/ai/enable_resample") {
        int ret = ss_mpi_ai_enable_resample(to_int(q, "ai_dev", kAiDev), to_int(q, "ai_chn", kAiChn), sample_rate_from_int(to_int(q, "out_sample_rate", kSampleRate)));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ai_enable_resample failed", ret);
    }
    if (req.path == "/api/ai/disable_resample") {
        int ret = ss_mpi_ai_disable_resample(to_int(q, "ai_dev", kAiDev), to_int(q, "ai_chn", kAiChn));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ai_disable_resample failed", ret);
    }
    if (req.path == "/api/ai/fd") {
        int ret = ss_mpi_ai_get_fd(to_int(q, "ai_dev", kAiDev), to_int(q, "ai_chn", kAiChn));
        return ret >= 0 ? ok_json("\"fd\":" + std::to_string(ret)) : err_json("ss_mpi_ai_get_fd failed", ret);
    }

    if (req.path == "/api/ao/set_pub_attr") {
        auto attr = make_aio_attr(q);
        int ret = ss_mpi_ao_set_pub_attr(to_int(q, "ao_dev", kAoDev), &attr);
        if (ret == TD_SUCCESS) { std::lock_guard<std::mutex> lock(g_state.mu); g_state.ao_attr = attr; }
        return ret == TD_SUCCESS ? ok_json(aio_attr_json(attr)) : err_json("ss_mpi_ao_set_pub_attr failed", ret);
    }
    if (req.path == "/api/ao/get_pub_attr") {
        ot_aio_attr attr{};
        int ret = ss_mpi_ao_get_pub_attr(to_int(q, "ao_dev", kAoDev), &attr);
        return ret == TD_SUCCESS ? ok_json(aio_attr_json(attr)) : err_json("ss_mpi_ao_get_pub_attr failed", ret);
    }
    if (req.path == "/api/ao/clr_pub_attr") {
        int ret = ss_mpi_ao_clr_pub_attr(to_int(q, "ao_dev", kAoDev));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ao_clr_pub_attr failed", ret);
    }
    if (req.path == "/api/ao/enable") {
        int ret = ss_mpi_ao_enable(to_int(q, "ao_dev", kAoDev));
        if (ret == TD_SUCCESS) { std::lock_guard<std::mutex> lock(g_state.mu); g_state.ao_enabled = true; }
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ao_enable failed", ret);
    }
    if (req.path == "/api/ao/disable") {
        int ret = ss_mpi_ao_disable(to_int(q, "ao_dev", kAoDev));
        if (ret == TD_SUCCESS) { std::lock_guard<std::mutex> lock(g_state.mu); g_state.ao_enabled = false; }
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ao_disable failed", ret);
    }
    if (req.path == "/api/ao/enable_chn") {
        int ret = ss_mpi_ao_enable_chn(to_int(q, "ao_dev", kAoDev), to_int(q, "ao_chn", kAoChn));
        if (ret == TD_SUCCESS) { std::lock_guard<std::mutex> lock(g_state.mu); g_state.ao_chn_enabled = true; }
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ao_enable_chn failed", ret);
    }
    if (req.path == "/api/ao/disable_chn") {
        int ret = ss_mpi_ao_disable_chn(to_int(q, "ao_dev", kAoDev), to_int(q, "ao_chn", kAoChn));
        if (ret == TD_SUCCESS) { std::lock_guard<std::mutex> lock(g_state.mu); g_state.ao_chn_enabled = false; }
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ao_disable_chn failed", ret);
    }
    if (req.path == "/api/ao/pause_chn") {
        int ret = ss_mpi_ao_pause_chn(to_int(q, "ao_dev", kAoDev), to_int(q, "ao_chn", kAoChn));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ao_pause_chn failed", ret);
    }
    if (req.path == "/api/ao/resume_chn") {
        int ret = ss_mpi_ao_resume_chn(to_int(q, "ao_dev", kAoDev), to_int(q, "ao_chn", kAoChn));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ao_resume_chn failed", ret);
    }
    if (req.path == "/api/ao/clear_buf") {
        int ret = ss_mpi_ao_clr_chn_buf(to_int(q, "ao_dev", kAoDev), to_int(q, "ao_chn", kAoChn));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ao_clr_chn_buf failed", ret);
    }
    if (req.path == "/api/ao/query_chn_status") {
        ot_ao_chn_state st{};
        int ret = ss_mpi_ao_query_chn_status(to_int(q, "ao_dev", kAoDev), to_int(q, "ao_chn", kAoChn), &st);
        if (ret != TD_SUCCESS) return err_json("ss_mpi_ao_query_chn_status failed", ret);
        std::ostringstream o;
        o << "\"chn_total_num\":" << st.chn_total_num << ",\"chn_free_num\":" << st.chn_free_num << ",\"chn_busy_num\":" << st.chn_busy_num;
        return ok_json(o.str());
    }
    if (req.path == "/api/ao/get_chn_delay") {
        td_u32 ms = 0;
        int ret = ss_mpi_ao_get_chn_delay(to_int(q, "ao_dev", kAoDev), to_int(q, "ao_chn", kAoChn), &ms);
        return ret == TD_SUCCESS ? ok_json("\"delay_ms\":" + std::to_string(ms)) : err_json("ss_mpi_ao_get_chn_delay failed", ret);
    }
    if (req.path == "/api/ao/set_volume") {
        int volume_db = to_int(q, "volume_db", 0);
        bool enabled = false;
        {
            std::lock_guard<std::mutex> lock(g_state.mu);
            g_state.ao_volume_db = volume_db;
            enabled = g_state.ao_enabled;
        }
        int ret = enabled ? ss_mpi_ao_set_volume(to_int(q, "ao_dev", kAoDev), volume_db) : TD_SUCCESS;
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ao_set_volume failed", ret);
    }
    if (req.path == "/api/ao/get_volume") {
        {
            std::lock_guard<std::mutex> lock(g_state.mu);
            if (!g_state.ao_enabled) return ok_json("\"volume_db\":" + std::to_string(g_state.ao_volume_db) + ",\"source\":\"cached\"");
        }
        td_s32 vol = 0;
        int ret = ss_mpi_ao_get_volume(to_int(q, "ao_dev", kAoDev), &vol);
        return ret == TD_SUCCESS ? ok_json("\"volume_db\":" + std::to_string(vol)) : err_json("ss_mpi_ao_get_volume failed", ret);
    }
    if (req.path == "/api/ao/set_mute") {
        ot_audio_fade fade{};
        fade.fade = to_int(q, "fade", 0) ? TD_TRUE : TD_FALSE;
        fade.fade_in_rate = static_cast<ot_audio_fade_rate>(to_int(q, "fade_in_rate", 0));
        fade.fade_out_rate = static_cast<ot_audio_fade_rate>(to_int(q, "fade_out_rate", 0));
        bool mute = to_int(q, "enable", 0) != 0;
        bool enabled = false;
        {
            std::lock_guard<std::mutex> lock(g_state.mu);
            g_state.ao_mute = mute;
            enabled = g_state.ao_enabled;
        }
        int ret = enabled ? ss_mpi_ao_set_mute(to_int(q, "ao_dev", kAoDev), mute ? TD_TRUE : TD_FALSE, &fade) : TD_SUCCESS;
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ao_set_mute failed", ret);
    }
    if (req.path == "/api/ao/get_mute") {
        {
            std::lock_guard<std::mutex> lock(g_state.mu);
            if (!g_state.ao_enabled) return ok_json(std::string("\"enable\":") + (g_state.ao_mute ? "true" : "false") + ",\"source\":\"cached\"");
        }
        td_bool enable = TD_FALSE;
        ot_audio_fade fade{};
        int ret = ss_mpi_ao_get_mute(to_int(q, "ao_dev", kAoDev), &enable, &fade);
        return ret == TD_SUCCESS ? ok_json(std::string("\"enable\":") + (enable ? "true" : "false")) : err_json("ss_mpi_ao_get_mute failed", ret);
    }
    if (req.path == "/api/ao/set_track_mode") {
        auto track = track_from_int(to_int(q, "track_mode", OT_AUDIO_TRACK_NORMAL));
        int ret = ss_mpi_ao_set_track_mode(to_int(q, "ao_dev", kAoDev), track);
        if (ret == TD_SUCCESS) { std::lock_guard<std::mutex> lock(g_state.mu); g_state.ao_track = track; }
        return ret == TD_SUCCESS ? ok_json("\"track_mode\":" + std::to_string(static_cast<int>(track))) : err_json("ss_mpi_ao_set_track_mode failed", ret);
    }
    if (req.path == "/api/ao/get_track_mode") {
        ot_audio_track_mode track{};
        int ret = ss_mpi_ao_get_track_mode(to_int(q, "ao_dev", kAoDev), &track);
        return ret == TD_SUCCESS ? ok_json("\"track_mode\":" + std::to_string(static_cast<int>(track))) : err_json("ss_mpi_ao_get_track_mode failed", ret);
    }
    if (req.path == "/api/ao/enable_resample") {
        int ret = ss_mpi_ao_enable_resample(to_int(q, "ao_dev", kAoDev), to_int(q, "ao_chn", kAoChn), sample_rate_from_int(to_int(q, "in_sample_rate", kSampleRate)));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ao_enable_resample failed", ret);
    }
    if (req.path == "/api/ao/disable_resample") {
        int ret = ss_mpi_ao_disable_resample(to_int(q, "ao_dev", kAoDev), to_int(q, "ao_chn", kAoChn));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ao_disable_resample failed", ret);
    }
    if (req.path == "/api/ao/fd") {
        int ret = ss_mpi_ao_get_fd(to_int(q, "ao_dev", kAoDev), to_int(q, "ao_chn", kAoChn));
        return ret >= 0 ? ok_json("\"fd\":" + std::to_string(ret)) : err_json("ss_mpi_ao_get_fd failed", ret);
    }

    if (req.path == "/api/aenc/create") {
        static ot_aenc_attr_g711 g711{};
        static ot_aenc_attr_g726 g726{};
        static ot_aenc_attr_adpcm adpcm{};
        static ot_aenc_attr_lpcm lpcm{};
        ot_payload_type type = payload_from_int(to_int(q, "type", OT_PT_LPCM));
        ot_aenc_chn_attr a{};
        a.type = type;
        a.point_num_per_frame = static_cast<td_u32>(to_int(q, "point_num_per_frame", kFrameSamples));
        a.buf_size = static_cast<td_u32>(to_int(q, "buf_size", 10));
        if (type == OT_PT_G726) { g726.g726bps = static_cast<ot_g726_bps>(to_int(q, "g726bps", OT_G726_32K)); a.value = &g726; }
        else if (type == OT_PT_ADPCMA) { adpcm.adpcm_type = static_cast<ot_adpcm_type>(to_int(q, "adpcm_type", OT_ADPCM_TYPE_DVI4)); a.value = &adpcm; }
        else if (type == OT_PT_LPCM) { a.value = &lpcm; }
        else { a.value = &g711; }
        int ret = ss_mpi_aenc_create_chn(to_int(q, "aenc_chn", 0), &a);
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_aenc_create_chn failed", ret);
    }
    if (req.path == "/api/aenc/destroy") {
        int ret = ss_mpi_aenc_destroy_chn(to_int(q, "aenc_chn", 0));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_aenc_destroy_chn failed", ret);
    }
    if (req.path == "/api/aenc/set_mute") {
        int ret = ss_mpi_aenc_set_mute(to_int(q, "aenc_chn", 0), to_int(q, "enable", 0) ? TD_TRUE : TD_FALSE);
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_aenc_set_mute failed", ret);
    }
    if (req.path == "/api/aenc/get_mute") {
        td_bool enable = TD_FALSE;
        int ret = ss_mpi_aenc_get_mute(to_int(q, "aenc_chn", 0), &enable);
        return ret == TD_SUCCESS ? ok_json(std::string("\"enable\":") + (enable ? "true" : "false")) : err_json("ss_mpi_aenc_get_mute failed", ret);
    }
    if (req.path == "/api/aenc/clear_buf") {
        int ret = ss_mpi_aenc_clr_chn_buf(to_int(q, "aenc_chn", 0));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_aenc_clr_chn_buf failed", ret);
    }
    if (req.path == "/api/aenc/fd") {
        int ret = ss_mpi_aenc_get_fd(to_int(q, "aenc_chn", 0));
        return ret >= 0 ? ok_json("\"fd\":" + std::to_string(ret)) : err_json("ss_mpi_aenc_get_fd failed", ret);
    }
    if (req.path == "/api/aenc/buf_info") {
        td_phys_addr_t phys = 0;
        td_u32 size = 0;
        int ret = ss_mpi_aenc_get_stream_buf_info(to_int(q, "aenc_chn", 0), &phys, &size);
        if (ret != TD_SUCCESS) return err_json("ss_mpi_aenc_get_stream_buf_info failed", ret);
        std::ostringstream o;
        o << "\"phys_addr\":" << static_cast<unsigned long long>(phys) << ",\"size\":" << size;
        return ok_json(o.str());
    }

    if (req.path == "/api/adec/create") {
        static ot_adec_attr_g711 g711{};
        static ot_adec_attr_g726 g726{};
        static ot_adec_attr_adpcm adpcm{};
        static ot_adec_attr_lpcm lpcm{};
        ot_payload_type type = payload_from_int(to_int(q, "type", OT_PT_LPCM));
        ot_adec_chn_attr a{};
        a.type = type;
        a.buf_size = static_cast<td_u32>(to_int(q, "buf_size", 10));
        a.mode = to_int(q, "mode", 0) == 1 ? OT_ADEC_MODE_STREAM : OT_ADEC_MODE_PACK;
        if (type == OT_PT_G726) { g726.g726bps = static_cast<ot_g726_bps>(to_int(q, "g726bps", OT_G726_32K)); a.value = &g726; }
        else if (type == OT_PT_ADPCMA) { adpcm.adpcm_type = static_cast<ot_adpcm_type>(to_int(q, "adpcm_type", OT_ADPCM_TYPE_DVI4)); a.value = &adpcm; }
        else if (type == OT_PT_LPCM) { a.value = &lpcm; }
        else { a.value = &g711; }
        int ret = ss_mpi_adec_create_chn(to_int(q, "adec_chn", 0), &a);
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_adec_create_chn failed", ret);
    }
    if (req.path == "/api/adec/destroy") {
        int ret = ss_mpi_adec_destroy_chn(to_int(q, "adec_chn", 0));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_adec_destroy_chn failed", ret);
    }
    if (req.path == "/api/adec/clear_buf") {
        int ret = ss_mpi_adec_clr_chn_buf(to_int(q, "adec_chn", 0));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_adec_clr_chn_buf failed", ret);
    }
    if (req.path == "/api/adec/eos") {
        int ret = ss_mpi_adec_send_end_of_stream(to_int(q, "adec_chn", 0), to_int(q, "instant", 0) ? TD_TRUE : TD_FALSE);
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_adec_send_end_of_stream failed", ret);
    }
    if (req.path == "/api/adec/status") {
        ot_adec_chn_state st{};
        int ret = ss_mpi_adec_query_chn_status(to_int(q, "adec_chn", 0), &st);
        if (ret != TD_SUCCESS) return err_json("ss_mpi_adec_query_chn_status failed", ret);
        std::ostringstream o;
        o << "\"end_of_stream\":" << (st.end_of_stream ? "true" : "false")
          << ",\"buf_total_num\":" << st.buf_total_num
          << ",\"buf_free_num\":" << st.buf_free_num
          << ",\"buf_busy_num\":" << st.buf_busy_num;
        return ok_json(o.str());
    }

    if (req.path == "/api/vqe/ai_record/set") {
        ot_ai_record_vqe_cfg cfg{};
        cfg.open_mask = static_cast<td_u32>(to_int(q, "open_mask", 0));
        cfg.work_sample_rate = to_int(q, "work_sample_rate", kSampleRate);
        cfg.frame_sample = to_int(q, "frame_sample", kFrameSamples);
        cfg.work_state = static_cast<ot_vqe_work_state>(to_int(q, "work_state", OT_VQE_WORK_STATE_COMMON));
        cfg.in_chn_num = to_int(q, "in_chn_num", 1);
        cfg.out_chn_num = to_int(q, "out_chn_num", 1);
        cfg.record_type = OT_VQE_RECORD_NORMAL;
        cfg.hpf_cfg.usr_mode = TD_TRUE;
        cfg.hpf_cfg.hpf_freq = static_cast<ot_audio_hpf_freq>(to_int(q, "hpf_freq", OT_AUDIO_HPF_FREQ_80));
        int ret = ss_mpi_ai_set_record_vqe_attr(to_int(q, "ai_dev", kAiDev), to_int(q, "ai_chn", kAiChn), &cfg);
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ai_set_record_vqe_attr failed", ret);
    }
    if (req.path == "/api/vqe/ai_record/get") {
        ot_ai_record_vqe_cfg cfg{};
        int ret = ss_mpi_ai_get_record_vqe_attr(to_int(q, "ai_dev", kAiDev), to_int(q, "ai_chn", kAiChn), &cfg);
        if (ret != TD_SUCCESS) return err_json("ss_mpi_ai_get_record_vqe_attr failed", ret);
        std::ostringstream o;
        o << "\"open_mask\":" << cfg.open_mask << ",\"work_sample_rate\":" << cfg.work_sample_rate << ",\"frame_sample\":" << cfg.frame_sample;
        return ok_json(o.str());
    }
    if (req.path == "/api/vqe/ai_talk/set") {
        ot_ai_talk_vqe_cfg cfg{};
        cfg.open_mask = static_cast<td_u32>(to_int(q, "open_mask", 0));
        cfg.work_sample_rate = to_int(q, "work_sample_rate", kSampleRate);
        cfg.frame_sample = to_int(q, "frame_sample", kFrameSamples);
        cfg.work_state = static_cast<ot_vqe_work_state>(to_int(q, "work_state", OT_VQE_WORK_STATE_COMMON));
        int ret = ss_mpi_ai_set_talk_vqe_attr(to_int(q, "ai_dev", kAiDev), to_int(q, "ai_chn", kAiChn), to_int(q, "ao_dev", kAoDev), to_int(q, "ao_chn", kAoChn), &cfg);
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ai_set_talk_vqe_attr failed", ret);
    }
    if (req.path == "/api/vqe/ai_talk/get") {
        ot_ai_talk_vqe_cfg cfg{};
        int ret = ss_mpi_ai_get_talk_vqe_attr(to_int(q, "ai_dev", kAiDev), to_int(q, "ai_chn", kAiChn), &cfg);
        if (ret != TD_SUCCESS) return err_json("ss_mpi_ai_get_talk_vqe_attr failed", ret);
        return ok_json("\"open_mask\":" + std::to_string(cfg.open_mask) + ",\"work_sample_rate\":" + std::to_string(cfg.work_sample_rate));
    }
    if (req.path == "/api/vqe/ai_talk_v2/set") {
        ot_ai_talk_vqe_v2_cfg cfg{};
        cfg.open_mask = static_cast<td_u32>(to_int(q, "open_mask", 0));
        cfg.work_sample_rate = to_int(q, "work_sample_rate", kSampleRate);
        cfg.frame_sample = to_int(q, "frame_sample", kFrameSamples);
        cfg.work_state = static_cast<ot_vqe_work_state>(to_int(q, "work_state", OT_VQE_WORK_STATE_COMMON));
        cfg.in_chn_num = to_int(q, "in_chn_num", 1);
        cfg.out_chn_num = to_int(q, "out_chn_num", 1);
        int ret = ss_mpi_ai_set_talk_vqe_v2_attr(to_int(q, "ai_dev", kAiDev), to_int(q, "ai_chn", kAiChn), to_int(q, "ao_dev", kAoDev), to_int(q, "ao_chn", kAoChn), &cfg);
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ai_set_talk_vqe_v2_attr failed", ret);
    }
    if (req.path == "/api/vqe/ai_talk_v2/get") {
        ot_ai_talk_vqe_v2_cfg cfg{};
        int ret = ss_mpi_ai_get_talk_vqe_v2_attr(to_int(q, "ai_dev", kAiDev), to_int(q, "ai_chn", kAiChn), &cfg);
        if (ret != TD_SUCCESS) return err_json("ss_mpi_ai_get_talk_vqe_v2_attr failed", ret);
        return ok_json("\"open_mask\":" + std::to_string(cfg.open_mask) + ",\"work_sample_rate\":" + std::to_string(cfg.work_sample_rate));
    }
    if (req.path == "/api/vqe/ai/enable") {
        int ret = ss_mpi_ai_enable_vqe(to_int(q, "ai_dev", kAiDev), to_int(q, "ai_chn", kAiChn));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ai_enable_vqe failed", ret);
    }
    if (req.path == "/api/vqe/ai/disable") {
        int ret = ss_mpi_ai_disable_vqe(to_int(q, "ai_dev", kAiDev), to_int(q, "ai_chn", kAiChn));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ai_disable_vqe failed", ret);
    }
    if (req.path == "/api/vqe/ao/set") {
        ot_ao_vqe_cfg cfg{};
        cfg.open_mask = static_cast<td_u32>(to_int(q, "open_mask", 0));
        cfg.work_sample_rate = to_int(q, "work_sample_rate", kSampleRate);
        cfg.frame_sample = to_int(q, "frame_sample", kFrameSamples);
        cfg.work_state = static_cast<ot_vqe_work_state>(to_int(q, "work_state", OT_VQE_WORK_STATE_COMMON));
        int ret = ss_mpi_ao_set_vqe_attr(to_int(q, "ao_dev", kAoDev), to_int(q, "ao_chn", kAoChn), &cfg);
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ao_set_vqe_attr failed", ret);
    }
    if (req.path == "/api/vqe/ao/get") {
        ot_ao_vqe_cfg cfg{};
        int ret = ss_mpi_ao_get_vqe_attr(to_int(q, "ao_dev", kAoDev), to_int(q, "ao_chn", kAoChn), &cfg);
        if (ret != TD_SUCCESS) return err_json("ss_mpi_ao_get_vqe_attr failed", ret);
        return ok_json("\"open_mask\":" + std::to_string(cfg.open_mask) + ",\"work_sample_rate\":" + std::to_string(cfg.work_sample_rate));
    }
    if (req.path == "/api/vqe/ao/enable") {
        int ret = ss_mpi_ao_enable_vqe(to_int(q, "ao_dev", kAoDev), to_int(q, "ao_chn", kAoChn));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ao_enable_vqe failed", ret);
    }
    if (req.path == "/api/vqe/ao/disable") {
        int ret = ss_mpi_ao_disable_vqe(to_int(q, "ao_dev", kAoDev), to_int(q, "ao_chn", kAoChn));
        return ret == TD_SUCCESS ? ok_json() : err_json("ss_mpi_ao_disable_vqe failed", ret);
    }

    if (req.path == "/api/acodec/ioctl") {
        std::string name = get_str(q, "name", "");
        td_u32 value = static_cast<td_u32>(to_int(q, "value", 0));
        int ret = -1;
        if (name == "soft_reset") { int fd = open("/dev/acodec", O_RDWR); if (fd >= 0) { ret = ioctl(fd, OT_ACODEC_SOFT_RESET_CTRL); close(fd); } }
        else if (name == "set_i2s1_fs") ret = acodec_ioctl_u32(OT_ACODEC_SET_I2S1_FS, &value);
        else if (name == "set_mixer_mic") ret = acodec_ioctl_u32(OT_ACODEC_SET_MIXER_MIC, &value);
        else if (name == "set_input_volume") ret = acodec_ioctl_u32(OT_ACODEC_SET_INPUT_VOLUME, &value);
        else if (name == "set_output_volume") ret = acodec_ioctl_u32(OT_ACODEC_SET_OUTPUT_VOLUME, &value);
        else if (name == "get_input_volume") ret = acodec_ioctl_u32(OT_ACODEC_GET_INPUT_VOLUME, &value);
        else if (name == "get_output_volume") ret = acodec_ioctl_u32(OT_ACODEC_GET_OUTPUT_VOLUME, &value);
        else if (name == "enable_boostl") ret = acodec_ioctl_u32(OT_ACODEC_ENABLE_BOOSTL, &value);
        else if (name == "enable_boostr") ret = acodec_ioctl_u32(OT_ACODEC_ENABLE_BOOSTR, &value);
        else if (name == "set_gain_micl") ret = acodec_ioctl_u32(OT_ACODEC_SET_GAIN_MICL, &value);
        else if (name == "set_gain_micr") ret = acodec_ioctl_u32(OT_ACODEC_SET_GAIN_MICR, &value);
        else if (name == "get_gain_micl") ret = acodec_ioctl_u32(OT_ACODEC_GET_GAIN_MICL, &value);
        else if (name == "get_gain_micr") ret = acodec_ioctl_u32(OT_ACODEC_GET_GAIN_MICR, &value);
        else if (name == "set_micl_mute") ret = acodec_ioctl_u32(OT_ACODEC_SET_MICL_MUTE, &value);
        else if (name == "set_micr_mute") ret = acodec_ioctl_u32(OT_ACODEC_SET_MICR_MUTE, &value);
        else if (name == "set_dacl_mute") ret = acodec_ioctl_u32(OT_ACODEC_SET_DACL_MUTE, &value);
        else if (name == "set_dacr_mute") ret = acodec_ioctl_u32(OT_ACODEC_SET_DACR_MUTE, &value);
        else if (name == "set_pd_dacl") ret = acodec_ioctl_u32(OT_ACODEC_SET_PD_DACL, &value);
        else if (name == "set_pd_dacr") ret = acodec_ioctl_u32(OT_ACODEC_SET_PD_DACR, &value);
        else if (name == "set_pd_adcl") ret = acodec_ioctl_u32(OT_ACODEC_SET_PD_ADCL, &value);
        else if (name == "set_pd_adcr") ret = acodec_ioctl_u32(OT_ACODEC_SET_PD_ADCR, &value);
        else if (name == "set_pd_lineinl") ret = acodec_ioctl_u32(OT_ACODEC_SET_PD_LINEINL, &value);
        else if (name == "set_pd_lineinr") ret = acodec_ioctl_u32(OT_ACODEC_SET_PD_LINEINR, &value);
        else if (name == "select_dac_clk") ret = acodec_ioctl_u32(OT_ACODEC_SELECT_DAC_CLK, &value);
        else if (name == "select_adc_clk") ret = acodec_ioctl_u32(OT_ACODEC_SELECT_ADC_CLK, &value);
        else if (name == "select_ana_mclk") ret = acodec_ioctl_u32(OT_ACODEC_SELECT_ANA_MCLK, &value);
        else if (name == "dacl_select_track") ret = acodec_ioctl_u32(OT_ACODEC_DACL_SELECT_TRACK, &value);
        else if (name == "dacr_select_track") ret = acodec_ioctl_u32(OT_ACODEC_DACR_SELECT_TRACK, &value);
        else if (name == "adcl_select_track") ret = acodec_ioctl_u32(OT_ACODEC_ADCL_SELECT_TRACK, &value);
        else if (name == "adcr_select_track") ret = acodec_ioctl_u32(OT_ACODEC_ADCR_SELECT_TRACK, &value);
        else if (name == "set_dac_de_emphasis") ret = acodec_ioctl_u32(OT_ACODEC_SET_DAC_DE_EMPHASIS, &value);
        else if (name == "set_adc_hp_filter") ret = acodec_ioctl_u32(OT_ACODEC_SET_ADC_HP_FILTER, &value);
        else if (name == "set_i2s1_data_width") ret = acodec_ioctl_u32(OT_ACODEC_SET_I2S1_DATA_WIDTH, &value);
        else if (name == "dac_soft_mute") ret = acodec_ioctl_u32(OT_ACODEC_DAC_SOFT_MUTE, &value);
        else if (name == "dac_soft_unmute") ret = acodec_ioctl_u32(OT_ACODEC_DAC_SOFT_UNMUTE, &value);
        else if (name == "dac_soft_mute_rate") ret = acodec_ioctl_u32(OT_ACODEC_DAC_SOFT_MUTE_RATE, &value);
        else if (name == "query_status") ret = acodec_ioctl_u32(OT_ACODEC_QUERY_STATUS, &value);
        else if (name == "set_dacl_volume" || name == "set_dacr_volume" || name == "set_adcl_volume" || name == "set_adcr_volume" ||
                 name == "get_dacl_volume" || name == "get_dacr_volume" || name == "get_adcl_volume" || name == "get_adcr_volume") {
            ot_acodec_volume_ctrl vol{};
            vol.volume_ctrl = value;
            vol.volume_ctrl_mute = static_cast<td_u32>(to_int(q, "mute", 0));
            unsigned long ioctl_req = OT_ACODEC_SET_DACL_VOLUME;
            if (name == "set_dacr_volume") ioctl_req = OT_ACODEC_SET_DACR_VOLUME;
            else if (name == "set_adcl_volume") ioctl_req = OT_ACODEC_SET_ADCL_VOLUME;
            else if (name == "set_adcr_volume") ioctl_req = OT_ACODEC_SET_ADCR_VOLUME;
            else if (name == "get_dacl_volume") ioctl_req = OT_ACODEC_GET_DACL_VOLUME;
            else if (name == "get_dacr_volume") ioctl_req = OT_ACODEC_GET_DACR_VOLUME;
            else if (name == "get_adcl_volume") ioctl_req = OT_ACODEC_GET_ADCL_VOLUME;
            else if (name == "get_adcr_volume") ioctl_req = OT_ACODEC_GET_ADCR_VOLUME;
            ret = acodec_ioctl_volume(ioctl_req, &vol);
            value = vol.volume_ctrl;
        } else {
            return err_json("unknown acodec ioctl name");
        }
        return ret == 0 ? ok_json("\"name\":\"" + json_escape(name) + "\",\"value\":" + std::to_string(value)) : err_json("acodec ioctl failed: " + name, ret);
    }

    if (req.path == "/api/demo/mic/input_settings/set") {
        /*
         * 保存录音启动时使用的 ACODEC 输入硬件参数。
         * 取值范围来自厂商头文件和 sample：
         *   boost: 0 关闭 / 1 开启；
         *   MIC 模拟增益: 0~31，驱动不接受高档位时启动录音会自动向下兼容；
         *   总输入音量: 0~80 dB。
         */
        std::lock_guard<std::mutex> lock(g_state.mu);
        g_state.acodec_boost_l = std::max(0, std::min(to_int(q, "boost_l", g_state.acodec_boost_l), 1));
        g_state.acodec_boost_r = std::max(0, std::min(to_int(q, "boost_r", g_state.acodec_boost_r), 1));
        g_state.acodec_gain_micl = std::max(0, std::min(to_int(q, "gain_micl", g_state.acodec_gain_micl), 31));
        g_state.acodec_gain_micr = std::max(0, std::min(to_int(q, "gain_micr", g_state.acodec_gain_micr), 31));
        g_state.acodec_input_volume_db = std::max(0, std::min(to_int(q, "input_volume_db", g_state.acodec_input_volume_db), 80));
        std::ostringstream o;
        o << "\"boost_l\":" << g_state.acodec_boost_l
          << ",\"boost_r\":" << g_state.acodec_boost_r
          << ",\"gain_micl\":" << g_state.acodec_gain_micl
          << ",\"gain_micr\":" << g_state.acodec_gain_micr
          << ",\"input_volume_db\":" << g_state.acodec_input_volume_db;
        return ok_json(o.str());
    }
    if (req.path == "/api/demo/mic/input_settings/get") {
        std::lock_guard<std::mutex> lock(g_state.mu);
        std::ostringstream o;
        o << "\"boost_l\":" << g_state.acodec_boost_l
          << ",\"boost_r\":" << g_state.acodec_boost_r
          << ",\"gain_micl\":" << g_state.acodec_gain_micl
          << ",\"gain_micr\":" << g_state.acodec_gain_micr
          << ",\"input_volume_db\":" << g_state.acodec_input_volume_db
          << ",\"actual_gain_micl\":" << g_state.acodec_actual_gain_micl
          << ",\"actual_gain_micr\":" << g_state.acodec_actual_gain_micr
          << ",\"actual_input_volume_db\":" << g_state.acodec_actual_input_volume_db;
        return ok_json(o.str());
    }

    if (req.path == "/api/demo/mic/start") {
        stop_record_thread();
        std::string input = get_str(q, "input", "left");
        std::string path = input == "right" ? kRightPcm : kLeftPcm;
        const ot_audio_track_mode track = input == "right" ? OT_AUDIO_TRACK_BOTH_RIGHT : OT_AUDIO_TRACK_BOTH_LEFT;
        {
            std::lock_guard<std::mutex> lock(g_state.mu);
            if (g_state.playing) { *code = 409; return err_json("speaker is playing"); }
        }
        int ret = ensure_ai_started(track);
        if (ret != TD_SUCCESS) {
            std::string detail = get_detail_error("ensure_ai_started failed");
            std::lock_guard<std::mutex> lock(g_state.mu);
            g_state.last_record_error = ret;
            g_state.last_record_error_text = detail;
            *code = 500;
            return err_json(detail, ret);
        }
        {
            std::lock_guard<std::mutex> lock(g_state.mu);
            g_state.record_stop.store(false);
            g_state.recording = true;
            g_state.record_input = input;
            g_state.record_frames = 0;
            g_state.record_bytes = 0;
            g_state.last_record_error = 0;
            g_state.last_record_error_text.clear();
            g_state.record_thread = std::thread(record_worker, input, path);
        }
        return ok_json("\"input\":\"" + json_escape(input) + "\"");
    }
    if (req.path == "/api/demo/mic/stop") {
        stop_record_thread();
        return ok_json();
    }
    if (req.path == "/api/demo/mic/download") {
        std::string input = get_str(q, "input", "left");
        if (input == "stereo") {
            auto l = read_file_bytes(kLeftPcm);
            auto r = read_file_bytes(kRightPcm);
            if (l.empty() && r.empty()) { *code = 404; return err_json("no pcm saved"); }
            *content_type = "audio/wav";
            *bin = make_stereo_wav(l, r);
            return "";
        }
        std::string path = input == "right" ? kRightPcm : kLeftPcm;
        auto pcm = read_file_bytes(path);
        if (pcm.empty()) { *code = 404; return err_json("no pcm saved"); }
        *content_type = "audio/wav";
        *bin = make_wav(pcm, kSampleRate, 1);
        return "";
    }
    if (req.path == "/api/demo/speaker/tone") {
        stop_play_thread();
        std::lock_guard<std::mutex> lock(g_state.mu);
        if (g_state.recording) { *code = 409; return err_json("microphone is recording"); }
        g_state.play_stop.store(false);
        g_state.playing = true;
        g_state.play_frames = 0;
        g_state.play_bytes = 0;
        g_state.play_thread = std::thread(tone_worker,
            static_cast<uint32_t>(to_int(q, "freq", 1000)),
            static_cast<uint32_t>(to_int(q, "duration", 3000)),
            to_int(q, "volume", 80));
        return ok_json();
    }
    if (req.path == "/api/demo/speaker/file") {
        stop_play_thread();
        auto wav = read_file_bytes(get_str(q, "path", "audio/speaker_voice_16k.wav"));
        std::vector<uint8_t> pcm;
        if (!parse_wav_pcm16_mono(wav, &pcm)) { *code = 400; return err_json("wav must be PCM 16kHz 16bit mono"); }
        std::lock_guard<std::mutex> lock(g_state.mu);
        if (g_state.recording) { *code = 409; return err_json("microphone is recording"); }
        g_state.play_stop.store(false);
        g_state.playing = true;
        g_state.play_frames = 0;
        g_state.play_bytes = 0;
        g_state.play_thread = std::thread(pcm_worker, pcm);
        return ok_json();
    }
    if (req.path == "/api/demo/speaker/stop") {
        stop_play_thread();
        return ok_json();
    }

    *code = 404;
    return err_json("not found");
}

void handle_client(int fd, const std::string &web_root)
{
    /*
     * 一个短生命周期线程处理一个 HTTP 连接。
     *
     * 静态资源只提供 index.html；其它浏览器动作全部走 /api/* 接口。
     * 这样部署简单，也方便审计所有会触碰硬件的入口。
     *
     */
    Request req;
    if (!parse_request(fd, &req)) {
        close(fd);
        return;
    }
    if (req.method == "GET" && (req.path == "/" || req.path == "/index.html")) {
        std::ifstream f(web_root + "/index.html", std::ios::binary);
        std::stringstream body;
        body << f.rdbuf();
        reply(fd, f ? 200 : 404, "text/html; charset=utf-8", body.str());
        close(fd);
        return;
    }
    if (req.path.rfind("/api/", 0) == 0) {
        int code = 200;
        std::string type;
        std::vector<uint8_t> bin;
        std::string body = handle_api(req, &code, &type, &bin);
        if (!bin.empty()) reply_bin(fd, code, type, bin);
        else reply(fd, code, type.empty() ? "application/json; charset=utf-8" : type, body);
        close(fd);
        return;
    }
    reply(fd, 404, "text/plain; charset=utf-8", "Not Found\n");
    close(fd);
}

} // 匿名命名空间结束

int main(int argc, char **argv)
{
    /*
     * 命令行参数：
     *   --port N       HTTP 监听端口，默认 8092。
     *   --web-root DIR index.html 所在目录。
     *
     * 运行脚本会传入完整包内的本地路径，因此整个 demo 目录可以整体移动。
     *
     */
    int port = 8092;
    std::string web_root = "web";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--web-root") == 0 && i + 1 < argc) web_root = argv[++i];
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || listen(s, 16) < 0) {
        std::cerr << "audio mpp web demo bind failed: " << std::strerror(errno) << "\n";
        return 2;
    }
    std::cout << "Audio MPP web demo: http://<device-ip>:" << port << "/\n";
    while (g_running.load()) {
        sockaddr_in ca{};
        socklen_t cl = sizeof(ca);
        int c = accept(s, reinterpret_cast<sockaddr *>(&ca), &cl);
        if (c >= 0) std::thread(handle_client, c, web_root).detach();
    }
    /* 进程退出时始终请求工作线程停止，并释放 AI/AO。 */
    stop_record_thread();
    stop_play_thread();
    stop_ai();
    stop_ao();
    close(s);
    return 0;
}

