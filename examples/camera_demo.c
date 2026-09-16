#include "ehal_camera.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_exit_requested;

static void handle_signal(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        g_exit_requested = 1;
    }
}

static void on_video_frame(const ehal_video_frame_t *frame, void *user_data)
{
    (void)user_data;
    if (frame == NULL || frame->data == NULL) {
        return;
    }

    printf("video frame channel=%d codec=%d size=%u key=%d\n",
           frame->channel,
           (int)frame->codec,
           frame->size,
           frame->key_frame);
}

int main(void)
{
    ehal_camera_t *camera = NULL;
    ehal_camera_config_t config;
    int ret;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    ret = ehal_camera_create(&camera);
    if (ret != EHAL_OK) {
        printf("ehal_camera_create failed: %s\n", ehal_camera_error_string(ret));
        return 1;
    }

    memset(&config, 0, sizeof(config));
    config.default_config_path = "configs/hal_default.json";
    config.runtime_config_path = "configs/hal.json";
    config.pipeline_name = "video_record";
    config.sensor_name = "sc2356";
    config.channel_count = 1;
    config.channels[0].channel = 0;
    config.channels[0].codec = EHAL_VIDEO_CODEC_H265;
    config.video_callback = on_video_frame;

    ret = ehal_camera_configure(camera, &config);
    if (ret != EHAL_OK) {
        printf("ehal_camera_configure failed: %s\n", ehal_camera_error_string(ret));
        ehal_camera_destroy(camera);
        return 1;
    }

    ret = ehal_camera_start(camera);
    if (ret != EHAL_OK) {
        printf("ehal_camera_start failed: %s\n", ehal_camera_error_string(ret));
        ehal_camera_destroy(camera);
        return 1;
    }

    printf("camera started, version=%s\n", ehal_camera_version());

    while (!g_exit_requested) {
        ehal_camera_stats_t stats;
        memset(&stats, 0, sizeof(stats));
        if (ehal_camera_get_stats(camera, &stats) == EHAL_OK) {
            printf("video frames=%llu bytes=%llu\n",
                   (unsigned long long)stats.video_frames,
                   (unsigned long long)stats.video_bytes);
        }
        sleep(1);
    }

    ehal_camera_stop(camera);
    ehal_camera_destroy(camera);
    return 0;
}
