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
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t bitrate_kbps;
    ehal_video_codec_t codec;
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

/** 摄像头实例句柄，不透明类型。 */
typedef struct ehal_camera ehal_camera_t;

const char *ehal_camera_version(void);
const char *ehal_camera_error_string(int code);

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

#ifdef __cplusplus
}
#endif

#endif /* EHAL_CAMERA_H */
