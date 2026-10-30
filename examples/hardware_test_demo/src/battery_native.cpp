#include "battery_native.h"

#include "chip/hi3516cv610/ot_adc.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <sstream>

namespace hardware_test {
namespace {

constexpr const char *kAdcDevice = "/dev/ot_lsadc";
constexpr uintptr_t kPinmuxAddress = 0x11130000U;
constexpr size_t kPinmuxMappingSize = 4096U;
constexpr int kAdcMaxCode = 1023;
constexpr int kAdcReferenceMv = 3300;
constexpr int kDividerNumerator = 3;
constexpr size_t kMaxCurveSamples = 3600U;
constexpr uint32_t kSampleIntervalMs = 1000U;

/* 用于后台线程句柄，避免把线程实现细节暴露到头文件。 */
class ThreadHolder {
public:
    std::thread thread;
};

} // namespace

/* 头文件中的不透明 Worker 实际保存后台线程对象。 */
class NativeBattery::Worker : public ThreadHolder {};

NativeBattery::NativeBattery() = default;

NativeBattery::~NativeBattery()
{
    stop();
}

uint64_t NativeBattery::monotonic_ms()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count());
}

int NativeBattery::voltage_to_percent(int battery_mv)
{
    /* 按单节锂电池 3.30V 空电、4.20V 满电做线性工程显示，原始电压同时保留。 */
    constexpr int kEmptyMv = 3300;
    constexpr int kFullMv = 4200;
    if (battery_mv <= kEmptyMv) return 0;
    if (battery_mv >= kFullMv) return 100;
    return (battery_mv - kEmptyMv) * 100 / (kFullMv - kEmptyMv);
}

bool NativeBattery::configure_adc(std::string *error_message)
{
    fd_ = ::open(kAdcDevice, O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        if (error_message != nullptr) *error_message = std::string("open ") +
            kAdcDevice + " failed: " + std::strerror(errno);
        return false;
    }

    const int mem_fd = ::open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (mem_fd < 0) {
        if (error_message != nullptr) *error_message = std::string("open /dev/mem failed: ") + std::strerror(errno);
        close_adc();
        return false;
    }
    pinmux_mapping_ = ::mmap(nullptr, kPinmuxMappingSize, PROT_READ | PROT_WRITE,
                              MAP_SHARED, mem_fd, kPinmuxAddress);
    ::close(mem_fd);
    if (pinmux_mapping_ == MAP_FAILED) {
        pinmux_mapping_ = nullptr;
        if (error_message != nullptr) *error_message = std::string("map pinmux failed: ") + std::strerror(errno);
        close_adc();
        return false;
    }
    volatile uint32_t *pinmux = static_cast<volatile uint32_t *>(pinmux_mapping_);
    saved_pinmux_ = *pinmux;
    *pinmux = (saved_pinmux_ & ~0x20fU) | 4U;
    pinmux_saved_ = true;
    if (*pinmux != ((saved_pinmux_ & ~0x20fU) | 4U)) {
        if (error_message != nullptr) *error_message = "configure GPIO1_5 pinmux failed";
        close_adc();
        return false;
    }

    int mode = 1;
    if (::ioctl(fd_, LSADC_IOC_MODEL_SEL, &mode) < 0 ||
        ::ioctl(fd_, LSADC_IOC_CHN_ENABLE, &channel_) < 0 ||
        ::ioctl(fd_, LSADC_IOC_START) < 0) {
        if (error_message != nullptr) *error_message = std::string("configure LSADC failed: ") + std::strerror(errno);
        close_adc();
        return false;
    }
    adc_started_ = true;
    return true;
}

void NativeBattery::close_adc()
{
    if (fd_ >= 0) {
        if (adc_started_) (void)::ioctl(fd_, LSADC_IOC_STOP);
        (void)::ioctl(fd_, LSADC_IOC_CHN_DISABLE, &channel_);
        ::close(fd_);
        fd_ = -1;
    }
    adc_started_ = false;
    if (pinmux_mapping_ != nullptr) {
        if (pinmux_saved_) {
            *static_cast<volatile uint32_t *>(pinmux_mapping_) = saved_pinmux_;
        }
        ::munmap(pinmux_mapping_, kPinmuxMappingSize);
        pinmux_mapping_ = nullptr;
    }
    pinmux_saved_ = false;
}

bool NativeBattery::start(std::string *error_message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_) return true;
    if (!configure_adc(error_message)) {
        last_error_ = error_message == nullptr ? "battery ADC configure failed" : *error_message;
        return false;
    }
    running_ = true;
    worker_holder_ = new Worker();
    worker_holder_->thread = std::thread(&NativeBattery::worker, this);
    return true;
}

void NativeBattery::stop()
{
    Worker *holder = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && worker_holder_ == nullptr) {
            close_adc();
            return;
        }
        running_ = false;
        holder = worker_holder_;
        worker_holder_ = nullptr;
    }
    if (holder != nullptr) {
        if (holder->thread.joinable()) holder->thread.join();
        delete holder;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    close_adc();
}

void NativeBattery::worker()
{
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) break;
        }
        const int raw = ::ioctl(fd_, LSADC_IOC_GET_CHNVAL, &channel_);
        if (raw >= 0 && raw <= kAdcMaxCode) {
            Sample sample;
            sample.timestamp_ms = monotonic_ms();
            sample.raw = raw;
            sample.battery_mv = raw * kAdcReferenceMv * kDividerNumerator / kAdcMaxCode;
            sample.percent = voltage_to_percent(sample.battery_mv);
            std::lock_guard<std::mutex> lock(mutex_);
            latest_ = sample;
            sample_valid_ = true;
            samples_.push_back(sample);
            while (samples_.size() > kMaxCurveSamples) samples_.pop_front();
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            last_error_ = std::string("read LSADC failed: ") + std::strerror(errno);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kSampleIntervalMs));
    }
}

std::string NativeBattery::json_escape(const std::string &value)
{
    std::ostringstream output;
    for (const unsigned char ch : value) {
        if (ch == '"' || ch == '\\') output << '\\' << ch;
        else output << static_cast<char>(ch);
    }
    return output.str();
}

std::string NativeBattery::status_json() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream output;
    output << "{\"ok\":true,\"running\":" << (running_ ? "true" : "false")
           << ",\"sample_valid\":" << (sample_valid_ ? "true" : "false")
           << ",\"raw\":" << (sample_valid_ ? latest_.raw : -1)
           << ",\"battery_mv\":" << (sample_valid_ ? latest_.battery_mv : 0)
           << ",\"battery_v\":" << (sample_valid_ ? latest_.battery_mv / 1000.0 : 0.0)
           << ",\"percent\":" << (sample_valid_ ? latest_.percent : 0)
           << ",\"sample_timestamp_ms\":" << (sample_valid_ ? latest_.timestamp_ms : 0)
           << ",\"sample_count\":" << samples_.size()
           << ",\"last_error\":\"" << json_escape(last_error_) << "\"}";
    return output.str();
}

std::string NativeBattery::curve_json() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream output;
    output << "{\"ok\":true,\"points\":[";
    bool first = true;
    for (const Sample &sample : samples_) {
        if (!first) output << ',';
        first = false;
        output << "{\"timestamp_ms\":" << sample.timestamp_ms
               << ",\"raw\":" << sample.raw
               << ",\"battery_mv\":" << sample.battery_mv
               << ",\"percent\":" << sample.percent << '}';
    }
    output << "]}";
    return output.str();
}

void NativeBattery::clear_curve()
{
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.clear();
}

} // namespace hardware_test
