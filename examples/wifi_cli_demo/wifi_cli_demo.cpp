#include "ehal_wifi.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

struct Options {
    const char *iface = "wlan0";
    const char *wpa_config = "/etc/wireless/wpa_supplicant.conf";
    const char *ssid = nullptr;
    const char *password = nullptr;
    const char *ap_ip = "192.168.4.1";
    const char *ap_netmask = "255.255.255.0";
    const char *dhcp_start = "192.168.4.100";
    const char *dhcp_end = "192.168.4.200";
    unsigned channel = 6;
    unsigned timeout_ms = 15000;
    bool save_config = false;
};

const char *state_name(ehal_wifi_state_t state)
{
    switch (state) {
    case EHAL_WIFI_STATE_DISABLED: return "disabled";
    case EHAL_WIFI_STATE_DISCONNECTED: return "disconnected";
    case EHAL_WIFI_STATE_SCANNING: return "scanning";
    case EHAL_WIFI_STATE_CONNECTING: return "connecting";
    case EHAL_WIFI_STATE_CONNECTED: return "connected";
    case EHAL_WIFI_STATE_AP: return "ap";
    default: return "unknown";
    }
}

const char *auth_name(ehal_wifi_auth_t auth)
{
    switch (auth) {
    case EHAL_WIFI_AUTH_OPEN: return "open";
    case EHAL_WIFI_AUTH_WPA_PSK: return "wpa";
    case EHAL_WIFI_AUTH_WPA2_PSK: return "wpa2";
    case EHAL_WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
    default: return "unknown";
    }
}

void usage(const char *program)
{
    std::printf(
        "Usage:\n"
        "  %s [--iface wlan0] [--wpa-config path] scan\n"
        "  %s [--iface wlan0] [--wpa-config path] status\n"
        "  %s [--iface wlan0] [--wpa-config path] connect --ssid SSID [--password PASS] [--open] [--timeout-ms N] [--save]\n"
        "  %s [--iface wlan0] disconnect\n"
        "  %s [--iface wlan0] ap-start --ssid SSID [--password PASS] [--channel N] [--ip IP] [--netmask MASK] [--dhcp-start IP] [--dhcp-end IP]\n"
        "  %s [--iface wlan0] ap-stop\n"
        "  %s [--iface wlan0] ap-status\n",
        program, program, program, program, program, program, program);
}

bool read_value(int argc, char **argv, int *index, const char **value)
{
    if (*index + 1 >= argc) {
        return false;
    }
    *value = argv[++(*index)];
    return true;
}

bool parse_common_arg(int argc, char **argv, int *index, Options *options)
{
    const char *arg = argv[*index];
    const char *value = nullptr;
    if (std::strcmp(arg, "--iface") == 0) {
        if (!read_value(argc, argv, index, &value)) {
            return false;
        }
        options->iface = value;
        return true;
    }
    if (std::strcmp(arg, "--wpa-config") == 0) {
        if (!read_value(argc, argv, index, &value)) {
            return false;
        }
        options->wpa_config = value;
        return true;
    }
    return false;
}

bool parse_command_options(int argc, char **argv, int start, Options *options, bool *open_auth)
{
    for (int i = start; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value = nullptr;
        if (parse_common_arg(argc, argv, &i, options)) {
            continue;
        }
        if (std::strcmp(arg, "--ssid") == 0) {
            if (!read_value(argc, argv, &i, &value)) {
                return false;
            }
            options->ssid = value;
        } else if (std::strcmp(arg, "--password") == 0) {
            if (!read_value(argc, argv, &i, &value)) {
                return false;
            }
            options->password = value;
        } else if (std::strcmp(arg, "--channel") == 0) {
            if (!read_value(argc, argv, &i, &value)) {
                return false;
            }
            options->channel = static_cast<unsigned>(std::strtoul(value, nullptr, 10));
        } else if (std::strcmp(arg, "--timeout-ms") == 0) {
            if (!read_value(argc, argv, &i, &value)) {
                return false;
            }
            options->timeout_ms = static_cast<unsigned>(std::strtoul(value, nullptr, 10));
        } else if (std::strcmp(arg, "--ip") == 0) {
            if (!read_value(argc, argv, &i, &value)) {
                return false;
            }
            options->ap_ip = value;
        } else if (std::strcmp(arg, "--netmask") == 0) {
            if (!read_value(argc, argv, &i, &value)) {
                return false;
            }
            options->ap_netmask = value;
        } else if (std::strcmp(arg, "--dhcp-start") == 0) {
            if (!read_value(argc, argv, &i, &value)) {
                return false;
            }
            options->dhcp_start = value;
        } else if (std::strcmp(arg, "--dhcp-end") == 0) {
            if (!read_value(argc, argv, &i, &value)) {
                return false;
            }
            options->dhcp_end = value;
        } else if (std::strcmp(arg, "--open") == 0) {
            *open_auth = true;
        } else if (std::strcmp(arg, "--save") == 0) {
            options->save_config = true;
        } else {
            return false;
        }
    }
    return true;
}

void print_result(const char *operation, int ret)
{
    std::printf("%s: %s (%d)\n", operation, ehal_wifi_error_string(ret), ret);
}

int create_wifi(const Options &options, ehal_wifi_t **wifi)
{
    int ret = ehal_wifi_create(wifi);
    if (ret != EHAL_OK) {
        print_result("create", ret);
        return ret;
    }

    ehal_wifi_config_t config{};
    config.interface_name = options.iface;
    config.wpa_config_path = options.wpa_config;
    config.connect_timeout_ms = options.timeout_ms;
    config.dhcp_timeout_ms = 10000;
    ret = ehal_wifi_configure(*wifi, &config);
    if (ret != EHAL_OK) {
        print_result("configure", ret);
        ehal_wifi_destroy(*wifi);
        *wifi = nullptr;
    }
    return ret;
}

int command_scan(ehal_wifi_t *wifi)
{
    ehal_wifi_ap_info_t aps[64]{};
    uint32_t count = 0;
    int ret = ehal_wifi_scan(wifi, aps, 64, &count);
    print_result("scan", ret);
    if (ret != EHAL_OK) {
        return ret;
    }
    std::printf("count=%u\n", count);
    uint32_t shown = count < 64 ? count : 64;
    for (uint32_t i = 0; i < shown; ++i) {
        std::printf("[%02u] ssid=\"%s\" bssid=%s signal=%d freq=%u auth=%s\n",
                    i,
                    aps[i].ssid[0] != '\0' ? aps[i].ssid : "(hidden)",
                    aps[i].bssid,
                    aps[i].signal_dbm,
                    aps[i].frequency_mhz,
                    auth_name(aps[i].auth));
    }
    return EHAL_OK;
}

int command_status(ehal_wifi_t *wifi)
{
    ehal_wifi_status_t status{};
    int ret = ehal_wifi_get_status(wifi, &status);
    print_result("status", ret);
    if (ret == EHAL_OK) {
        std::printf("state=%s ssid=\"%s\" bssid=%s signal=%d ip=%s\n",
                    state_name(status.state),
                    status.ssid,
                    status.bssid,
                    status.signal_dbm,
                    status.ip);
    }

    ehal_wifi_ip_info_t ip{};
    ret = ehal_wifi_get_ip_info(wifi, &ip);
    print_result("ip-info", ret);
    if (ret == EHAL_OK) {
        std::printf("ip=%s netmask=%s gateway=%s dns1=%s dns2=%s\n",
                    ip.ip, ip.netmask, ip.gateway, ip.dns1, ip.dns2);
    }
    return EHAL_OK;
}

int command_connect(ehal_wifi_t *wifi, const Options &options, bool open_auth)
{
    if (options.ssid == nullptr || options.ssid[0] == '\0') {
        std::fprintf(stderr, "connect requires --ssid\n");
        return 2;
    }
    if (!open_auth && (options.password == nullptr || options.password[0] == '\0')) {
        std::fprintf(stderr, "connect requires --password or --open\n");
        return 2;
    }

    ehal_wifi_connect_config_t config{};
    config.ssid = options.ssid;
    config.password = options.password;
    config.auth = open_auth ? EHAL_WIFI_AUTH_OPEN : EHAL_WIFI_AUTH_WPA_WPA2_PSK;
    config.timeout_ms = options.timeout_ms;
    config.save_config = options.save_config ? 1 : 0;
    int ret = ehal_wifi_connect(wifi, &config);
    print_result("connect", ret);
    if (ret == EHAL_OK) {
        command_status(wifi);
    }
    return ret;
}

int command_ap_start(ehal_wifi_t *wifi, const Options &options)
{
    if (options.ssid == nullptr || options.ssid[0] == '\0') {
        std::fprintf(stderr, "ap-start requires --ssid\n");
        return 2;
    }

    ehal_wifi_ap_config_t config{};
    config.ssid = options.ssid;
    config.password = options.password;
    config.channel = options.channel;
    config.ip = options.ap_ip;
    config.netmask = options.ap_netmask;
    config.dhcp_start = options.dhcp_start;
    config.dhcp_end = options.dhcp_end;
    config.lease_seconds = 86400;
    config.hidden = 0;
    int ret = ehal_wifi_start_ap(wifi, &config);
    print_result("ap-start", ret);
    return ret;
}

int command_ap_status(ehal_wifi_t *wifi)
{
    ehal_wifi_ap_status_t status{};
    int ret = ehal_wifi_get_ap_status(wifi, &status);
    print_result("ap-status", ret);
    if (ret == EHAL_OK) {
        std::printf("running=%d ssid=\"%s\" ip=%s netmask=%s channel=%u stations=%u\n",
                    status.running,
                    status.ssid,
                    status.ip,
                    status.netmask,
                    status.channel,
                    status.station_count);
    }
    return ret;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    Options options;
    int command_index = -1;
    for (int i = 1; i < argc; ++i) {
        if (parse_common_arg(argc, argv, &i, &options)) {
            continue;
        }
        command_index = i;
        break;
    }

    if (command_index < 0) {
        usage(argv[0]);
        return 2;
    }

    const char *command = argv[command_index];
    bool open_auth = false;
    if (!parse_command_options(argc, argv, command_index + 1, &options, &open_auth)) {
        usage(argv[0]);
        return 2;
    }

    ehal_wifi_t *wifi = nullptr;
    int ret = create_wifi(options, &wifi);
    if (ret != EHAL_OK) {
        return ret;
    }

    if (std::strcmp(command, "scan") == 0) {
        ret = command_scan(wifi);
    } else if (std::strcmp(command, "status") == 0) {
        ret = command_status(wifi);
    } else if (std::strcmp(command, "connect") == 0) {
        ret = command_connect(wifi, options, open_auth);
    } else if (std::strcmp(command, "disconnect") == 0) {
        ret = ehal_wifi_disconnect(wifi);
        print_result("disconnect", ret);
    } else if (std::strcmp(command, "ap-start") == 0) {
        ret = command_ap_start(wifi, options);
    } else if (std::strcmp(command, "ap-stop") == 0) {
        ret = ehal_wifi_stop_ap(wifi);
        print_result("ap-stop", ret);
    } else if (std::strcmp(command, "ap-status") == 0) {
        ret = command_ap_status(wifi);
    } else {
        usage(argv[0]);
        ret = 2;
    }

    ehal_wifi_destroy(wifi);
    return ret == EHAL_OK ? 0 : ret;
}
