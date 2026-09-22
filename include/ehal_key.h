#ifndef EHAL_KEY_H
#define EHAL_KEY_H

#include "ehal_common.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EHAL_KEY_VERSION_MAJOR 1
#define EHAL_KEY_VERSION_MINOR 0
#define EHAL_KEY_VERSION_PATCH 0

typedef enum {
    EHAL_KEY_FUNCTION = 0,
    EHAL_KEY_VOLUME_UP,
    EHAL_KEY_POWER,
    EHAL_KEY_VOLUME_DOWN,
    EHAL_KEY_COUNT
} ehal_key_code_t;

typedef enum {
    EHAL_KEY_EVENT_PRESS = 0,
    EHAL_KEY_EVENT_RELEASE,
    EHAL_KEY_EVENT_LONG_PRESS
} ehal_key_event_type_t;

typedef struct {
    ehal_key_code_t key;
    ehal_key_event_type_t type;
    uint32_t duration_ms;
    uint64_t timestamp_ms;
} ehal_key_event_t;

typedef struct {
    const char *device_name;
    uint32_t long_press_ms;
} ehal_key_config_t;

typedef struct {
    int pressed;
    uint64_t press_count;
    uint64_t release_count;
    uint64_t long_press_count;
} ehal_key_state_t;

typedef void (*ehal_key_callback_t)(const ehal_key_event_t *event,
                                    void *user_data);

typedef struct ehal_key ehal_key_t;

const char *ehal_key_version(void);
const char *ehal_key_error_string(int code);

int ehal_key_create(ehal_key_t **key);
int ehal_key_configure(ehal_key_t *key, const ehal_key_config_t *config);
int ehal_key_set_callback(ehal_key_t *key,
                          ehal_key_callback_t callback,
                          void *user_data);
int ehal_key_start(ehal_key_t *key);
int ehal_key_stop(ehal_key_t *key);
void ehal_key_destroy(ehal_key_t *key);

int ehal_key_get_state(ehal_key_t *key,
                       ehal_key_code_t key_code,
                       ehal_key_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* EHAL_KEY_H */
