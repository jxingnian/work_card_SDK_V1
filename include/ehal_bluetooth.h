#ifndef EHAL_BLUETOOTH_H
#define EHAL_BLUETOOTH_H

#include "ehal_common.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EHAL_BT_VERSION_MAJOR 1
#define EHAL_BT_VERSION_MINOR 0
#define EHAL_BT_VERSION_PATCH 0
#define EHAL_BT_MAX_ADDRESS_LEN 17
#define EHAL_BT_MAX_NAME_LEN 248
#define EHAL_BT_MAX_UUID_LEN 40
#define EHAL_BT_MAX_CODEC_LEN 32
#define EHAL_BT_MAX_PCM_PATH_LEN 256

typedef enum {
    EHAL_BT_STATE_OFF = 0,
    EHAL_BT_STATE_ON = 1,
    EHAL_BT_STATE_DISCOVERING = 2
} ehal_bt_state_t;

typedef enum {
    EHAL_BT_EVENT_ADAPTER = 0,
    EHAL_BT_EVENT_DEVICE_FOUND,
    EHAL_BT_EVENT_DEVICE_CHANGED,
    EHAL_BT_EVENT_PAIRING_REQUEST,
    EHAL_BT_EVENT_PAIRING_COMPLETE,
    EHAL_BT_EVENT_CONNECTED,
    EHAL_BT_EVENT_DISCONNECTED,
    EHAL_BT_EVENT_PROFILE_CONNECTED,
    EHAL_BT_EVENT_PROFILE_DISCONNECTED,
    EHAL_BT_EVENT_AUDIO_CHANGED,
    EHAL_BT_EVENT_CALL_CHANGED,
    EHAL_BT_EVENT_ERROR
} ehal_bt_event_type_t;

typedef enum {
    EHAL_BT_PROFILE_ANY = 0,
    EHAL_BT_PROFILE_A2DP_SOURCE,
    EHAL_BT_PROFILE_A2DP_SINK,
    EHAL_BT_PROFILE_HFP_AG,
    EHAL_BT_PROFILE_HFP_HF,
    EHAL_BT_PROFILE_HSP_AG,
    EHAL_BT_PROFILE_HSP_HS
} ehal_bt_profile_t;

typedef enum {
    EHAL_BT_AUDIO_STATE_DISCONNECTED = 0,
    EHAL_BT_AUDIO_STATE_CONNECTING,
    EHAL_BT_AUDIO_STATE_CONNECTED,
    EHAL_BT_AUDIO_STATE_PLAYING,
    EHAL_BT_AUDIO_STATE_PAUSED
} ehal_bt_audio_state_t;

typedef enum {
    EHAL_BT_CALL_IDLE = 0,
    EHAL_BT_CALL_INCOMING,
    EHAL_BT_CALL_DIALING,
    EHAL_BT_CALL_ALERTING,
    EHAL_BT_CALL_ACTIVE,
    EHAL_BT_CALL_HELD,
    EHAL_BT_CALL_ENDED
} ehal_bt_call_state_t;

typedef struct {
    char address[EHAL_BT_MAX_ADDRESS_LEN + 1];
    char name[EHAL_BT_MAX_NAME_LEN + 1];
    char alias[EHAL_BT_MAX_NAME_LEN + 1];
    int16_t rssi;
    uint32_t class_of_device;
    int paired;
    int trusted;
    int connected;
    int blocked;
    int classic;
    int le;
} ehal_bt_device_info_t;

typedef struct {
    char address[EHAL_BT_MAX_ADDRESS_LEN + 1];
    char name[EHAL_BT_MAX_NAME_LEN + 1];
    char alias[EHAL_BT_MAX_NAME_LEN + 1];
    uint32_t class_of_device;
    char version[32];
    char manufacturer[64];
    int powered;
    int pairable;
    int discoverable;
    int discovering;
} ehal_bt_adapter_info_t;

typedef struct {
    ehal_bt_event_type_t type;
    ehal_bt_profile_t profile;
    ehal_bt_device_info_t device;
    ehal_bt_audio_state_t audio_state;
    ehal_bt_call_state_t call_state;
    int result;
    char message[256];
} ehal_bt_event_t;

typedef void (*ehal_bt_event_callback_t)(const ehal_bt_event_t *event,
                                          void *user_data);

typedef struct {
    const char *adapter;
    const char *dbus_name;
    uint32_t command_timeout_ms;
    uint32_t scan_timeout_ms;
    ehal_bt_event_callback_t event_callback;
    void *user_data;
} ehal_bt_config_t;

typedef struct {
    char codec[EHAL_BT_MAX_CODEC_LEN];
    char pcm_path[EHAL_BT_MAX_PCM_PATH_LEN];
    int sample_rate;
    int channels;
    int bit_width;
    int volume;
    int muted;
} ehal_bt_audio_info_t;

typedef struct ehal_bluetooth ehal_bluetooth_t;

const char *ehal_bt_version(void);
const char *ehal_bt_error_string(int code);

int ehal_bt_create(ehal_bluetooth_t **bt);
int ehal_bt_configure(ehal_bluetooth_t *bt, const ehal_bt_config_t *config);
int ehal_bt_start(ehal_bluetooth_t *bt);
int ehal_bt_stop(ehal_bluetooth_t *bt);
void ehal_bt_destroy(ehal_bluetooth_t *bt);

int ehal_bt_get_adapter_info(ehal_bluetooth_t *bt,
                             ehal_bt_adapter_info_t *info);
int ehal_bt_set_power(ehal_bluetooth_t *bt, int enabled);
int ehal_bt_set_name(ehal_bluetooth_t *bt, const char *name);
int ehal_bt_set_alias(ehal_bluetooth_t *bt, const char *alias);
int ehal_bt_set_pairable(ehal_bluetooth_t *bt, int enabled);
int ehal_bt_set_discoverable(ehal_bluetooth_t *bt, int enabled);

int ehal_bt_start_scan(ehal_bluetooth_t *bt);
int ehal_bt_stop_scan(ehal_bluetooth_t *bt);
int ehal_bt_is_scanning(ehal_bluetooth_t *bt, int *scanning);
int ehal_bt_get_devices(ehal_bluetooth_t *bt,
                        ehal_bt_device_info_t *devices,
                        uint32_t max_count,
                        uint32_t *actual_count);
int ehal_bt_get_device_info(ehal_bluetooth_t *bt,
                            const char *address,
                            ehal_bt_device_info_t *info);
int ehal_bt_pair(ehal_bluetooth_t *bt, const char *address);
int ehal_bt_cancel_pair(ehal_bluetooth_t *bt, const char *address);
int ehal_bt_remove_device(ehal_bluetooth_t *bt, const char *address);
int ehal_bt_set_trusted(ehal_bluetooth_t *bt, const char *address, int trusted);
int ehal_bt_set_blocked(ehal_bluetooth_t *bt, const char *address, int blocked);
int ehal_bt_connect(ehal_bluetooth_t *bt, const char *address);
int ehal_bt_disconnect(ehal_bluetooth_t *bt, const char *address);
int ehal_bt_connect_profile(ehal_bluetooth_t *bt,
                            const char *address,
                            ehal_bt_profile_t profile);
int ehal_bt_disconnect_profile(ehal_bluetooth_t *bt,
                               const char *address,
                               ehal_bt_profile_t profile);

int ehal_bt_gatt_discover_services(ehal_bluetooth_t *bt,
                                   const char *address);
int ehal_bt_gatt_read(ehal_bluetooth_t *bt,
                      const char *address,
                      const char *characteristic);
int ehal_bt_gatt_write(ehal_bluetooth_t *bt,
                       const char *address,
                       const char *characteristic,
                       const uint8_t *data,
                       uint32_t size);
int ehal_bt_gatt_start_notify(ehal_bluetooth_t *bt,
                              const char *address,
                              const char *characteristic);
int ehal_bt_gatt_stop_notify(ehal_bluetooth_t *bt,
                             const char *address,
                             const char *characteristic);

int ehal_bt_a2dp_get_info(ehal_bluetooth_t *bt,
                          const char *address,
                          ehal_bt_audio_info_t *info);
int ehal_bt_a2dp_set_volume(ehal_bluetooth_t *bt,
                            const char *address,
                            int volume);
int ehal_bt_a2dp_set_mute(ehal_bluetooth_t *bt,
                          const char *address,
                          int muted);
int ehal_bt_a2dp_play_file(ehal_bluetooth_t *bt,
                           const char *address,
                           const char *path);
int ehal_bt_a2dp_stop(ehal_bluetooth_t *bt, const char *address);
int ehal_bt_a2dp_write_pcm(ehal_bluetooth_t *bt,
                           const char *address,
                           const void *pcm,
                           uint32_t bytes,
                           uint32_t sample_rate,
                           uint8_t channels,
                           uint8_t bit_width);

int ehal_bt_hfp_open_sco(ehal_bluetooth_t *bt, const char *address);
int ehal_bt_hfp_close_sco(ehal_bluetooth_t *bt, const char *address);
int ehal_bt_hfp_send_at(ehal_bluetooth_t *bt,
                        const char *address,
                        const char *command);
int ehal_bt_call_dial(ehal_bluetooth_t *bt,
                      const char *address,
                      const char *number);
int ehal_bt_call_accept(ehal_bluetooth_t *bt, const char *address);
int ehal_bt_call_reject(ehal_bluetooth_t *bt, const char *address);
int ehal_bt_call_hangup(ehal_bluetooth_t *bt, const char *address);
int ehal_bt_call_hold(ehal_bluetooth_t *bt, const char *address);
int ehal_bt_call_mute(ehal_bluetooth_t *bt, const char *address, int muted);

int ehal_bt_avrcp_play(ehal_bluetooth_t *bt, const char *address);
int ehal_bt_avrcp_pause(ehal_bluetooth_t *bt, const char *address);
int ehal_bt_avrcp_stop(ehal_bluetooth_t *bt, const char *address);
int ehal_bt_avrcp_next(ehal_bluetooth_t *bt, const char *address);
int ehal_bt_avrcp_previous(ehal_bluetooth_t *bt, const char *address);

int ehal_bt_get_last_output(ehal_bluetooth_t *bt,
                            char *buffer,
                            uint32_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif
