#include "ehal_key.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {

const char *key_name(ehal_key_code_t key)
{
    switch (key) {
    case EHAL_KEY_FUNCTION: return "function";
    case EHAL_KEY_VOLUME_UP: return "volume_up";
    case EHAL_KEY_POWER: return "power";
    case EHAL_KEY_VOLUME_DOWN: return "volume_down";
    default: return "unknown";
    }
}

const char *event_name(ehal_key_event_type_t type)
{
    switch (type) {
    case EHAL_KEY_EVENT_PRESS: return "press";
    case EHAL_KEY_EVENT_RELEASE: return "release";
    case EHAL_KEY_EVENT_LONG_PRESS: return "long_press";
    default: return "unknown";
    }
}

void on_key(const ehal_key_event_t *event, void *)
{
    std::printf("key=%s event=%s duration_ms=%u timestamp_ms=%llu\n",
                key_name(event->key),
                event_name(event->type),
                event->duration_ms,
                static_cast<unsigned long long>(event->timestamp_ms));
    std::fflush(stdout);
}

void usage(const char *program)
{
    std::printf("Usage: %s [--device-name NAME] [--long-ms N]\n", program);
}

} // namespace

int main(int argc, char **argv)
{
    const char *device_name = "board-keys";
    uint32_t long_press_ms = 1500U;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--device-name") == 0 && i + 1 < argc) {
            device_name = argv[++i];
        } else if (std::strcmp(argv[i], "--long-ms") == 0 && i + 1 < argc) {
            long_press_ms = static_cast<uint32_t>(
                std::strtoul(argv[++i], nullptr, 10));
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    ehal_key_t *key = nullptr;
    int ret = ehal_key_create(&key);
    if (ret != EHAL_OK) {
        std::fprintf(stderr, "create: %s (%d)\n",
                     ehal_key_error_string(ret), ret);
        return 1;
    }

    ehal_key_config_t config{};
    config.device_name = device_name;
    config.long_press_ms = long_press_ms;
    ret = ehal_key_configure(key, &config);
    if (ret == EHAL_OK) {
        ret = ehal_key_set_callback(key, on_key, nullptr);
    }
    if (ret == EHAL_OK) {
        ret = ehal_key_start(key);
    }
    if (ret != EHAL_OK) {
        std::fprintf(stderr, "start: %s (%d)\n",
                     ehal_key_error_string(ret), ret);
        ehal_key_destroy(key);
        return 1;
    }

    std::printf("key_cli_demo started device=%s long_press_ms=%u\n",
                device_name, long_press_ms);
    std::printf("Press Ctrl-C to stop.\n");
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
