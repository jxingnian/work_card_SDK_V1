#ifndef EHAL_CAMERA_H
#define EHAL_CAMERA_H

#include "ehal_common.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EHAL_CAMERA_VERSION_MAJOR 1
#define EHAL_CAMERA_VERSION_MINOR 0
#define EHAL_CAMERA_VERSION_PATCH 0

#define EHAL_CAMERA_MAX_CHANNELS 4

#define EHAL_CAMERA_CFG_WIDTH        (1U << 0)
#define EHAL_CAMERA_CFG_HEIGHT       (1U << 1)
#define EHAL_CAMERA_CFG_FPS          (1U << 2)
#define EHAL_CAMERA_CFG_BITRATE      (1U << 3)
#define EHAL_CAMERA_CFG_CODEC        (1U << 4)
#define EHAL_CAMERA_CFG_GOP          (1U << 5)
#define EHAL_CAMERA_CFG_RC_MODE      (1U << 6)
#define EHAL_CAMERA_CFG_PROFILE      (1U << 7)
#define EHAL_CAMERA_CFG_MIN_QP       (1U << 8)
#define EHAL_CAMERA_CFG_MAX_QP       (1U << 9)
#define EHAL_CAMERA_CFG_VPSS_NR      (1U << 10)
#define EHAL_CAMERA_CFG_VPSS_SHARPEN (1U << 11)
#define EHAL_CAMERA_CFG_VPSS_IESHARP (1U << 12)
#define EHAL_CAMERA_CFG_VI_WIDTH     (1U << 13)
#define EHAL_CAMERA_CFG_VI_HEIGHT    (1U << 14)
#define EHAL_CAMERA_CFG_VI_FPS       (1U << 15)
#define EHAL_CAMERA_CFG_VI_BIT_WIDTH (1U << 16)
#define EHAL_CAMERA_CFG_VI_WDR       (1U << 17)
#define EHAL_CAMERA_CFG_VI_NR        (1U << 18)
#define EHAL_CAMERA_CFG_VI_SHARPEN   (1U << 19)

/** SDK 通用返回码。 */
/** 视频编码格式。 */
typedef enum {
    EHAL_VIDEO_CODEC_H264 = 0,
    EHAL_VIDEO_CODEC_H265 = 1
} ehal_video_codec_t;

/**
 * 编码视频帧。
 *
 * 当前 HAL 通用数据回调只提供码流地址和长度，不能提供时间戳及关键帧标志；
 * 因此 `pts_ms` 当前为 0，`key_frame` 当前为 0。
 */
typedef struct {
    int channel;
    ehal_video_codec_t codec;
    const uint8_t *data;
    uint32_t size;
    uint64_t pts_ms;
    int key_frame;
} ehal_video_frame_t;

/** 视频帧回调。回调返回后 frame->data 指向的数据失效。 */
typedef void (*ehal_video_frame_callback_t)(
    const ehal_video_frame_t *frame,
    void *user_data);

/** 单个视频通道配置。 */
typedef struct {
    int channel;
    uint32_t config_mask;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t bitrate_kbps;
    uint32_t gop;
    uint32_t rc_mode;
    uint32_t profile;
    uint32_t min_qp;
    uint32_t max_qp;
    ehal_video_codec_t codec;
    uint32_t vpss_width;
    uint32_t vpss_height;
    int vpss_nr;
    int vpss_sharpen;
    int vpss_iesharp;
    uint32_t vi_width;
    uint32_t vi_height;
    uint32_t vi_fps;
    uint32_t vi_bit_width;
    int vi_wdr;
    int vi_nr;
    int vi_sharpen;
} ehal_video_channel_config_t;

/** 摄像头实例配置。 */
typedef struct {
    const char *default_config_path;
    const char *runtime_config_path;
    const char *user_config_path;
    const char *pipeline_name;
    const char *sensor_name;
    uint32_t channel_count;
    ehal_video_channel_config_t channels[EHAL_CAMERA_MAX_CHANNELS];
    ehal_video_frame_callback_t video_callback;
    void *user_data;
} ehal_camera_config_t;

/** 摄像头视频运行统计。 */
typedef struct {
    uint64_t video_frames;
    uint64_t video_bytes;
    uint64_t dropped_frames;
    uint64_t encode_errors;
    uint64_t callback_errors;
} ehal_camera_stats_t;

typedef struct {
    int code;
    int hal_error;
    ehal_camera_stage_e stage;
    int channel;
    uint32_t requested_width;
    uint32_t requested_height;
    uint32_t requested_fps;
    uint32_t requested_bitrate_kbps;
    char message[160];
} ehal_camera_error_detail_t;

/** 摄像头实例句柄，不透明类型。 */
typedef struct ehal_camera ehal_camera_t;

const char *ehal_camera_version(void);
const char *ehal_camera_error_string(int code);
const char *ehal_camera_stage_string(ehal_camera_stage_e stage);

int ehal_camera_create(ehal_camera_t **camera);
int ehal_camera_configure(ehal_camera_t *camera,
                          const ehal_camera_config_t *config);
int ehal_camera_start(ehal_camera_t *camera);
int ehal_camera_stop(ehal_camera_t *camera);
void ehal_camera_destroy(ehal_camera_t *camera);

int ehal_camera_set_video_callback(ehal_camera_t *camera,
                                   ehal_video_frame_callback_t callback,
                                   void *user_data);
int ehal_camera_request_idr(ehal_camera_t *camera, int channel);
int ehal_camera_set_bitrate(ehal_camera_t *camera,
                            int channel,
                            uint32_t bitrate_kbps);
int ehal_camera_snapshot(ehal_camera_t *camera,
                         int channel,
                         const char *jpeg_path);
int ehal_camera_get_stats(ehal_camera_t *camera,
                          ehal_camera_stats_t *stats);
int ehal_camera_get_last_error(ehal_camera_t *camera,
                               ehal_camera_error_detail_t *detail);

#ifdef __cplusplus
}
#endif

#endif /* EHAL_CAMERA_H */
