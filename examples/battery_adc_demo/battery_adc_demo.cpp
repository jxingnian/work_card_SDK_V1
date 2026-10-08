#include <cstdint>
#include "chip/hi3516cv610/ot_adc.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <csignal>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
constexpr const char *kDevice = "/dev/ot_lsadc";
constexpr int kBatteryChannel = 1;       // GPIO1_5 = LSADC_CH1
constexpr int kAdcMaxCode = 1023;        // 10-bit LSADC: codes 0..1023
constexpr int kAdcFullScaleMv = 3300;     // Nominal LSADC supply; configurable for calibration.
constexpr int kDividerNumerator = 3;      // R6=200k, R11=100k: VBAT=3*BAT_ADC
volatile sig_atomic_t running = 1;
void stop(int) { running = 0; }
struct PinMux {
    int fd = -1;
    volatile uint32_t *reg = nullptr;
    uint32_t saved = 0;
    bool setup() {
        fd = ::open("/dev/mem", O_RDWR | O_SYNC);
        if (fd < 0) return false;
        void *p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x11130000);
        if (p == MAP_FAILED) return false;
        reg = static_cast<volatile uint32_t *>(p);
        saved = *reg;
        // Pin table: function[3:0]=4, pull-down[9]=0 for analog input.
        *reg = (saved & ~0x20fU) | 4U;
        std::printf("pinmux 0x11130000: 0x%08x -> 0x%08x\n", saved, *reg);
        return *reg == ((saved & ~0x20fU) | 4U);
    }
    ~PinMux() {
        if (reg) { *reg = saved; munmap(const_cast<uint32_t *>(reg), 4096); }
        if (fd >= 0) ::close(fd);
    }
};
}

int main(int argc, char **argv)
{
    char *end = nullptr;
    const long count = argc > 1 ? std::strtol(argv[1], &end, 10) : 10;
    if (argc > 3 || count <= 0 || count > 100000 || (argc > 1 && (!*argv[1] || *end))) {
        std::fprintf(stderr, "usage: %s [sample_count:1..100000] [vref_mv:1..5000]\n", argv[0]);
        return 2;
    }
    const long vref = argc > 2 ? std::strtol(argv[2], &end, 10) : kAdcFullScaleMv;
    if (vref <= 0 || vref > 5000 || (argc > 2 && (!*argv[2] || *end))) return 2;
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    const int fd = ::open(kDevice, O_RDWR);
    if (fd < 0) {
        std::perror("open /dev/ot_lsadc");
        return 1;
    }
    int mode = 1;
    int channel = kBatteryChannel;
    PinMux mux;
    if (!mux.setup()) { std::perror("configure pinmux"); ::close(fd); return 1; }
    if (::ioctl(fd, LSADC_IOC_MODEL_SEL, &mode) < 0 ||
        ::ioctl(fd, LSADC_IOC_CHN_ENABLE, &channel) < 0 ||
        ::ioctl(fd, LSADC_IOC_START) < 0) {
        std::perror("configure LSADC");
        ::ioctl(fd, LSADC_IOC_STOP);
        ::ioctl(fd, LSADC_IOC_CHN_DISABLE, &channel);
        ::close(fd);
        return 1;
    }
    std::printf("battery_adc_demo: GPIO1_5 -> LSADC_CH1, samples=%ld vref_mv=%ld divider=3 (nominal, uncalibrated)\n", count, vref);
    usleep(200000);
    int status = 0;
    for (int i = 0; i < count && running; ++i) {
        const int raw = ::ioctl(fd, LSADC_IOC_GET_CHNVAL, &channel);
        if (raw < 0 || raw > kAdcMaxCode) {
            std::fprintf(stderr, "read LSADC failed: raw=%d errno=%d\n", raw, errno);
            status = 1; break;
        }
        // The 10-bit output spans code 0..1023; use the full-scale code for
        // the voltage conversion. The divider is R6=200k over R11=100k.
        const double adc_mv = raw * static_cast<double>(vref) / kAdcMaxCode;
        const double battery_mv = adc_mv * kDividerNumerator;
        std::printf("sample=%d raw=%d adc_mv=%.2f battery_mv=%.2f battery=%.3fV%s\n",
                    i + 1, raw, adc_mv, battery_mv, battery_mv / 1000.0,
                    raw == 1023 ? " SATURATED" : "");
        std::fflush(stdout);
        usleep(200000);
    }
    if (::ioctl(fd, LSADC_IOC_STOP) < 0) { std::perror("stop LSADC"); status = 1; }
    if (::ioctl(fd, LSADC_IOC_CHN_DISABLE, &channel) < 0) { std::perror("disable LSADC"); status = 1; }
    ::close(fd);
    return status;
}
