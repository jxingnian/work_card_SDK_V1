#ifndef EHAL_WIFI_H
#define EHAL_WIFI_H

#include "ehal_common.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EHAL_WIFI_VERSION_MAJOR 1
#define EHAL_WIFI_VERSION_MINOR 0
#define EHAL_WIFI_VERSION_PATCH 0

#define EHAL_WIFI_MAX_SSID_LEN 32
#define EHAL_WIFI_MAX_BSSID_LEN 17
#define EHAL_WIFI_MAX_IPV4_LEN 15

typedef enum {
    EHAL_WIFI_AUTH_OPEN = 0,
    EHAL_WIFI_AUTH_WPA_PSK = 1,
    EHAL_WIFI_AUTH_WPA2_PSK = 2,
    EHAL_WIFI_AUTH_WPA_WPA2_PSK = 3
} ehal_wifi_auth_t;

typedef enum {
    EHAL_WIFI_STATE_DISABLED = 0,
    EHAL_WIFI_STATE_DISCONNECTED = 1,
    EHAL_WIFI_STATE_SCANNING = 2,
    EHAL_WIFI_STATE_CONNECTING = 3,
    EHAL_WIFI_STATE_CONNECTED = 4,
    EHAL_WIFI_STATE_AP = 5
} ehal_wifi_state_t;

typedef struct {
    const char *interface_name;
    const char *wpa_config_path;
    uint32_t connect_timeout_ms;
    uint32_t dhcp_timeout_ms;
} ehal_wifi_config_t;

typedef struct {
    char ssid[EHAL_WIFI_MAX_SSID_LEN + 1];
    char bssid[EHAL_WIFI_MAX_BSSID_LEN + 1];
    int signal_dbm;
    uint32_t frequency_mhz;
    ehal_wifi_auth_t auth;
} ehal_wifi_ap_info_t;

typedef struct {
    const char *ssid;
    const char *password;
    ehal_wifi_auth_t auth;
    uint32_t timeout_ms;
    int save_config;
} ehal_wifi_connect_config_t;

typedef struct {
    const char *ssid;
    const char *password;
    uint32_t channel;
    const char *ip;
    const char *netmask;
    const char *dhcp_start;
    const char *dhcp_end;
    uint32_t lease_seconds;
    int hidden;
} ehal_wifi_ap_config_t;

typedef struct {
    ehal_wifi_state_t state;
    char ssid[EHAL_WIFI_MAX_SSID_LEN + 1];
    char bssid[EHAL_WIFI_MAX_BSSID_LEN + 1];
    int signal_dbm;
    char ip[EHAL_WIFI_MAX_IPV4_LEN + 1];
} ehal_wifi_status_t;

typedef struct {
    char ip[EHAL_WIFI_MAX_IPV4_LEN + 1];
    char netmask[EHAL_WIFI_MAX_IPV4_LEN + 1];
    char gateway[EHAL_WIFI_MAX_IPV4_LEN + 1];
    char dns1[EHAL_WIFI_MAX_IPV4_LEN + 1];
    char dns2[EHAL_WIFI_MAX_IPV4_LEN + 1];
} ehal_wifi_ip_info_t;

typedef struct {
    int running;
    char ssid[EHAL_WIFI_MAX_SSID_LEN + 1];
    char ip[EHAL_WIFI_MAX_IPV4_LEN + 1];
    char netmask[EHAL_WIFI_MAX_IPV4_LEN + 1];
    uint32_t channel;
    uint32_t station_count;
} ehal_wifi_ap_status_t;

typedef struct ehal_wifi ehal_wifi_t;

const char *ehal_wifi_version(void);
const char *ehal_wifi_error_string(int code);

int ehal_wifi_create(ehal_wifi_t **wifi);
int ehal_wifi_configure(ehal_wifi_t *wifi,
                        const ehal_wifi_config_t *config);
void ehal_wifi_destroy(ehal_wifi_t *wifi);

int ehal_wifi_scan(ehal_wifi_t *wifi,
                   ehal_wifi_ap_info_t *aps,
                   uint32_t max_count,
                   uint32_t *actual_count);
int ehal_wifi_connect(ehal_wifi_t *wifi,
                      const ehal_wifi_connect_config_t *config);
int ehal_wifi_disconnect(ehal_wifi_t *wifi);
int ehal_wifi_start_ap(ehal_wifi_t *wifi,
                       const ehal_wifi_ap_config_t *config);
int ehal_wifi_stop_ap(ehal_wifi_t *wifi);
int ehal_wifi_get_ap_status(ehal_wifi_t *wifi,
                            ehal_wifi_ap_status_t *status);
int ehal_wifi_get_status(ehal_wifi_t *wifi,
                         ehal_wifi_status_t *status);
int ehal_wifi_get_ip_info(ehal_wifi_t *wifi,
                          ehal_wifi_ip_info_t *ip_info);

#ifdef __cplusplus
}
#endif

#endif /* EHAL_WIFI_H */
