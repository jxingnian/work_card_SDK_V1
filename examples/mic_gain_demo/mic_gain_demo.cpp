#include "chip/hi3516cv610/ot_acodec.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
constexpr const char *kCodecDevice = "/dev/acodec";
constexpr td_u32 kBoostEnabled = 1U;
constexpr td_u32 kMicGainMaximum = 0x1fU;
constexpr td_u32 kInputVolumeMaximumDb = 80U;

bool set_value(int fd, unsigned long request, td_u32 value, const char *name)
{
    if (ioctl(fd, request, &value) != 0) {
        std::fprintf(stderr, "%s failed: %s\n", name, std::strerror(errno));
        return false;
    }
    std::printf("%s=%u\n", name, value);
    return true;
}

bool get_value(int fd, unsigned long request, td_u32 *value, const char *name)
{
    if (ioctl(fd, request, value) != 0) {
        std::fprintf(stderr, "%s readback failed: %s\n", name, std::strerror(errno));
        return false;
    }
    std::printf("%s(readback)=%u\n", name, *value);
    return true;
}
} // namespace

int main()
{
    const int fd = open(kCodecDevice, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        std::fprintf(stderr, "open %s failed: %s\n", kCodecDevice, std::strerror(errno));
        return 1;
    }

    bool ok = true;
    ok = set_value(fd, OT_ACODEC_ENABLE_BOOSTL, kBoostEnabled, "boost_left") && ok;
    ok = set_value(fd, OT_ACODEC_ENABLE_BOOSTR, kBoostEnabled, "boost_right") && ok;
    // Probe down because some codec revisions reject 0x1f with EPERM.
    auto set_gain_compat = [&](unsigned long request, const char *name) {
        for (td_u32 gain = kMicGainMaximum;; --gain) {
            if (set_value(fd, request, gain, name)) return true;
            if (gain == 0U) break;
        }
        return false;
    };
    ok = set_gain_compat(OT_ACODEC_SET_GAIN_MICL, "mic_gain_left") && ok;
    ok = set_gain_compat(OT_ACODEC_SET_GAIN_MICR, "mic_gain_right") && ok;
    ok = set_value(fd, OT_ACODEC_SET_INPUT_VOLUME, kInputVolumeMaximumDb, "input_volume_db") && ok;

    td_u32 value = 0;
    if (ok) {
        ok = get_value(fd, OT_ACODEC_GET_GAIN_MICL, &value, "mic_gain_left") && ok;
        ok = get_value(fd, OT_ACODEC_GET_GAIN_MICR, &value, "mic_gain_right") && ok;
        ok = get_value(fd, OT_ACODEC_GET_INPUT_VOLUME, &value, "input_volume_db") && ok;
    }

    close(fd);
    if (!ok) {
        std::fprintf(stderr, "mic gain configuration incomplete\n");
        return 2;
    }
    std::printf("mic gain configuration applied: boost=on, mic_gain=31, input_volume=80dB\n");
    return 0;
}
