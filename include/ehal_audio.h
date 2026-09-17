#ifndef EHAL_AUDIO_H
#define EHAL_AUDIO_H

#include "ehal_common.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EHAL_AUDIO_VERSION_MAJOR 1
#define EHAL_AUDIO_VERSION_MINOR 0
#define EHAL_AUDIO_VERSION_PATCH 0

typedef enum {
    EHAL_AUDIO_CODEC_G711A = 0,
    EHAL_AUDIO_CODEC_G711U = 1,
    EHAL_AUDIO_CODEC_PCM = 2
} ehal_audio_codec_t;

typedef struct {
    int channel;
    ehal_audio_codec_t codec;
    const uint8_t *data;
    uint32_t size;
    uint64_t pts_ms;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bit_width;
} ehal_audio_frame_t;

typedef void (*ehal_audio_frame_callback_t)(
    const ehal_audio_frame_t *frame,
    void *user_data);

typedef struct {
    const char *default_config_path;
    const char *runtime_config_path;
    const char *user_config_path;
    const char *record_pipeline_name;
    const char *playback_pipeline_name;
    int enable_record;
    int enable_playback;
    int ai_dev;
    int ao_dev;
    int aenc_channel;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bit_width;
    uint32_t samples_per_frame;
    ehal_audio_codec_t codec;
    ehal_audio_frame_callback_t capture_callback;
    void *user_data;
} ehal_audio_config_t;

typedef struct {
    uint64_t capture_frames;
    uint64_t capture_bytes;
    uint64_t playback_frames;
    uint64_t playback_bytes;
    uint64_t callback_errors;
} ehal_audio_stats_t;

typedef struct ehal_audio ehal_audio_t;

const char *ehal_audio_version(void);
const char *ehal_audio_error_string(int code);

int ehal_audio_create(ehal_audio_t **audio);
int ehal_audio_configure(ehal_audio_t *audio,
                         const ehal_audio_config_t *config);
int ehal_audio_start(ehal_audio_t *audio);
int ehal_audio_stop(ehal_audio_t *audio);
void ehal_audio_destroy(ehal_audio_t *audio);

int ehal_audio_set_capture_callback(ehal_audio_t *audio,
                                    ehal_audio_frame_callback_t callback,
                                    void *user_data);
int ehal_audio_set_input_volume(ehal_audio_t *audio, int volume);
int ehal_audio_set_output_volume(ehal_audio_t *audio, int volume);
int ehal_audio_set_output_mute(ehal_audio_t *audio, int mute);
int ehal_audio_play_pcm(ehal_audio_t *audio,
                        const void *pcm,
                        uint32_t bytes,
                        int timeout_ms);
int ehal_audio_play_test_tone(ehal_audio_t *audio,
                              uint32_t frequency_hz,
                              uint32_t duration_ms,
                              int volume_percent);
int ehal_audio_get_stats(ehal_audio_t *audio,
                         ehal_audio_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* EHAL_AUDIO_H */
