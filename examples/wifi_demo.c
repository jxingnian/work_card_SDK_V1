#include "ehal_wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *program)
{
    printf("usage:\n");
    printf("  %s scan\n", program);
    printf("  %s connect <ssid> <password>\n", program);
    printf("  %s status\n", program);
    printf("  %s disconnect\n", program);
    printf("  %s ap-start <ssid> <password> [ip] [channel]\n", program);
    printf("  %s ap-stop\n", program);
    printf("  %s ap-status\n", program);
}

static const char *state_name(ehal_wifi_state_t state)
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

static const char *auth_name(ehal_wifi_auth_t auth)
{
    switch (auth) {
    case EHAL_WIFI_AUTH_OPEN: return "open";
    case EHAL_WIFI_AUTH_WPA_PSK: return "wpa";
    case EHAL_WIFI_AUTH_WPA2_PSK: return "wpa2";
    case EHAL_WIFI_AUTH_WPA_WPA2_PSK: return "wpa/wpa2";
    default: return "unknown";
    }
}

static int create_wifi(ehal_wifi_t **wifi)
{
    ehal_wifi_config_t config;
    int ret = ehal_wifi_create(wifi);
    if (ret != EHAL_OK) {
        printf("ehal_wifi_create failed: %s\n", ehal_wifi_error_string(ret));
        return ret;
    }

    memset(&config, 0, sizeof(config));
    config.interface_name = "wlan0";
    config.wpa_config_path = "/etc/wireless/wpa_supplicant.conf";
    config.connect_timeout_ms = 15000;
    config.dhcp_timeout_ms = 10000;
    ret = ehal_wifi_configure(*wifi, &config);
    if (ret != EHAL_OK) {
        printf("ehal_wifi_configure failed: %s\n", ehal_wifi_error_string(ret));
        ehal_wifi_destroy(*wifi);
        *wifi = NULL;
    }
    return ret;
}

static int do_scan(ehal_wifi_t *wifi)
{
    ehal_wifi_ap_info_t aps[32];
    uint32_t count = 0;
    int ret = ehal_wifi_scan(wifi, aps, 32, &count);
    if (ret != EHAL_OK) {
        printf("scan failed: %s\n", ehal_wifi_error_string(ret));
        return 1;
    }

    printf("scan result count=%u\n", count);
    for (uint32_t i = 0; i < count && i < 32; ++i) {
        printf("%2u ssid=%s bssid=%s signal=%d freq=%u auth=%s\n",
               i + 1,
               aps[i].ssid,
               aps[i].bssid,
               aps[i].signal_dbm,
               aps[i].frequency_mhz,
               auth_name(aps[i].auth));
    }
    return 0;
}

static int do_connect(ehal_wifi_t *wifi, const char *ssid, const char *password)
{
    ehal_wifi_connect_config_t config;
    int ret;

    memset(&config, 0, sizeof(config));
    config.ssid = ssid;
    config.password = password;
    config.auth = password[0] == '\0' ? EHAL_WIFI_AUTH_OPEN : EHAL_WIFI_AUTH_WPA_WPA2_PSK;
    config.timeout_ms = 15000;
    config.save_config = 1;

    ret = ehal_wifi_connect(wifi, &config);
    if (ret != EHAL_OK) {
        printf("connect failed: %s\n", ehal_wifi_error_string(ret));
        return 1;
    }
    printf("connect success\n");
    return 0;
}

static int do_status(ehal_wifi_t *wifi)
{
    ehal_wifi_status_t status;
    ehal_wifi_ip_info_t ip_info;
    int ret = ehal_wifi_get_status(wifi, &status);
    if (ret != EHAL_OK) {
        printf("status failed: %s\n", ehal_wifi_error_string(ret));
        return 1;
    }

    printf("state=%s ssid=%s bssid=%s signal=%d ip=%s\n",
           state_name(status.state),
           status.ssid,
           status.bssid,
           status.signal_dbm,
           status.ip);

    memset(&ip_info, 0, sizeof(ip_info));
    ret = ehal_wifi_get_ip_info(wifi, &ip_info);
    if (ret == EHAL_OK) {
        printf("ip=%s netmask=%s gateway=%s dns1=%s dns2=%s\n",
               ip_info.ip,
               ip_info.netmask,
               ip_info.gateway,
               ip_info.dns1,
               ip_info.dns2);
    }
    return 0;
}

static int do_ap_start(ehal_wifi_t *wifi,
                       const char *ssid,
                       const char *password,
                       const char *ip,
                       const char *channel)
{
    ehal_wifi_ap_config_t config;
    int ret;

    memset(&config, 0, sizeof(config));
    config.ssid = ssid;
    config.password = password;
    config.channel = channel != NULL ? (uint32_t)atoi(channel) : 6;
    config.ip = ip;
    config.netmask = "255.255.255.0";
    config.dhcp_start = "192.168.4.100";
    config.dhcp_end = "192.168.4.200";
    config.lease_seconds = 86400;

    ret = ehal_wifi_start_ap(wifi, &config);
    if (ret != EHAL_OK) {
        printf("ap-start failed: %s\n", ehal_wifi_error_string(ret));
        return 1;
    }
    printf("ap-start success ssid=%s ip=%s channel=%u\n",
           ssid,
           ip != NULL ? ip : "192.168.4.1",
           config.channel);
    return 0;
}

static int do_ap_status(ehal_wifi_t *wifi)
{
    ehal_wifi_ap_status_t status;
    int ret = ehal_wifi_get_ap_status(wifi, &status);
    if (ret != EHAL_OK) {
        printf("ap-status failed: %s\n", ehal_wifi_error_string(ret));
        return 1;
    }
    printf("running=%d ssid=%s ip=%s netmask=%s channel=%u stations=%u\n",
           status.running,
           status.ssid,
           status.ip,
           status.netmask,
           status.channel,
           status.station_count);
    return 0;
}

int main(int argc, char **argv)
{
    ehal_wifi_t *wifi = NULL;
    int exit_code = 0;

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    if (create_wifi(&wifi) != EHAL_OK) {
        return 1;
    }

    if (strcmp(argv[1], "scan") == 0) {
        exit_code = do_scan(wifi);
    } else if (strcmp(argv[1], "connect") == 0) {
        if (argc < 4) {
            print_usage(argv[0]);
            exit_code = 1;
        } else {
            exit_code = do_connect(wifi, argv[2], argv[3]);
        }
    } else if (strcmp(argv[1], "status") == 0) {
        exit_code = do_status(wifi);
    } else if (strcmp(argv[1], "disconnect") == 0) {
        int ret = ehal_wifi_disconnect(wifi);
        if (ret != EHAL_OK) {
            printf("disconnect failed: %s\n", ehal_wifi_error_string(ret));
            exit_code = 1;
        } else {
            printf("disconnect success\n");
        }
    } else if (strcmp(argv[1], "ap-start") == 0) {
        if (argc < 4) {
            print_usage(argv[0]);
            exit_code = 1;
        } else {
            exit_code = do_ap_start(wifi,
                                    argv[2],
                                    argv[3],
                                    argc >= 5 ? argv[4] : NULL,
                                    argc >= 6 ? argv[5] : NULL);
        }
    } else if (strcmp(argv[1], "ap-stop") == 0) {
        int ret = ehal_wifi_stop_ap(wifi);
        if (ret != EHAL_OK) {
            printf("ap-stop failed: %s\n", ehal_wifi_error_string(ret));
            exit_code = 1;
        } else {
            printf("ap-stop success\n");
        }
    } else if (strcmp(argv[1], "ap-status") == 0) {
        exit_code = do_ap_status(wifi);
    } else {
        print_usage(argv[0]);
        exit_code = 1;
    }

    ehal_wifi_destroy(wifi);
    return exit_code;
}
