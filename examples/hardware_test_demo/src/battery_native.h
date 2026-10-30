#ifndef HARDWARE_TEST_BATTERY_NATIVE_H
#define HARDWARE_TEST_BATTERY_NATIVE_H

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace hardware_test {

/*
 * 电池采样模块独立管理厂家 LSADC 资源。
 * 采样点是 GPIO1_5/LSADC_CH1，硬件分压倍率为 3，所有采样点和曲线数据
 * 都保存在模块内部，HTTP 层只读取快照，不直接操作 ADC 寄存器。
 */
class NativeBattery {
public:
    NativeBattery();
    ~NativeBattery();

    NativeBattery(const NativeBattery &) = delete;
    NativeBattery &operator=(const NativeBattery &) = delete;

    /* 打开 ADC、配置引脚并启动后台采样线程。 */
    bool start(std::string *error_message);

    /* 停止采样、关闭 ADC 并恢复启动前的引脚复用配置。 */
    void stop();

    /* 返回当前电压、电量、原始码和采样状态。 */
    std::string status_json() const;

    /* 返回从模块启动以来保存的电压曲线，最多保留 3600 个点。 */
    std::string curve_json() const;

    /* 清空曲线但不停止当前采样。 */
    void clear_curve();

private:
    struct Sample {
        uint64_t timestamp_ms = 0;
        int raw = 0;
        int battery_mv = 0;
        int percent = 0;
    };

    static std::string json_escape(const std::string &value);
    static uint64_t monotonic_ms();
    static int voltage_to_percent(int battery_mv);
    void worker();
    bool configure_adc(std::string *error_message);
    void close_adc();

    mutable std::mutex mutex_;
    std::deque<Sample> samples_;
    std::string last_error_;
    int fd_ = -1;
    int channel_ = 1;
    uint32_t saved_pinmux_ = 0;
    void *pinmux_mapping_ = nullptr;
    bool pinmux_saved_ = false;
    bool adc_started_ = false;
    bool running_ = false;
    bool sample_valid_ = false;
    Sample latest_;
    class Worker;
    Worker *worker_holder_ = nullptr;
};

} // namespace hardware_test

#endif
