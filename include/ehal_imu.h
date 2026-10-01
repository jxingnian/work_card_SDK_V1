#ifndef EHAL_IMU_H
#define EHAL_IMU_H

#include "ehal_common.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ehal_imu ehal_imu_t;

typedef struct {
    const char *i2c_device;       /* default: /dev/i2c-1 (I2C1_SDA/SCL) */
    unsigned int i2c_address;     /* 0x6a (SA0 high) or 0x6b (SA0 low) */
    const char *int1_value_path;  /* optional sysfs GPIO value path */
    unsigned int accel_odr_hz;    /* 0 keeps the QMI8658C default */
    unsigned int gyro_odr_hz;     /* 0 keeps the QMI8658C default */
    int enable_gyro;
    int enable_attitude_engine;
} ehal_imu_config_t;

typedef struct {
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
    int16_t temperature;
    uint8_t status0;
    uint8_t status1;
    uint64_t timestamp_ms;
} ehal_imu_sample_t;

typedef enum {
    EHAL_IMU_EVENT_MOTION = 1,
    EHAL_IMU_EVENT_DATA_READY = 2
} ehal_imu_event_type_t;

typedef void (*ehal_imu_callback_t)(ehal_imu_event_type_t event, void *user_data);

ehal_result_t ehal_imu_create(const ehal_imu_config_t *config, ehal_imu_t **out_imu);
ehal_result_t ehal_imu_destroy(ehal_imu_t *imu);
ehal_result_t ehal_imu_read_sample(ehal_imu_t *imu, ehal_imu_sample_t *sample);
ehal_result_t ehal_imu_enable_wake_on_motion(ehal_imu_t *imu, uint8_t threshold, uint8_t blanking);
ehal_result_t ehal_imu_set_callback(ehal_imu_t *imu, ehal_imu_callback_t callback, void *user_data);
ehal_result_t ehal_imu_poll_event(ehal_imu_t *imu, int timeout_ms, ehal_imu_event_type_t *event);
ehal_result_t ehal_imu_get_identity(ehal_imu_t *imu, uint8_t *who_am_i, uint8_t *revision_id);

#ifdef __cplusplus
}
#endif

#endif /* EHAL_IMU_H */
