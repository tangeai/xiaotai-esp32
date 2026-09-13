#ifndef STARTER_AEC_H
#define STARTER_AEC_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "starter_signal_level.h"

/* Keep 6 dB of ADC headroom for speaker leakage. Restore nominal speech level
 * only AFTER echo cancellation; boosting before AEC would recreate clipping. */
#define STARTER_AEC_MIC_PGA_DB 24.0f
#define STARTER_AEC_OUTPUT_GAIN 1.9952623f

#ifdef __cplusplus
extern "C" {
#endif

enum {
    STARTER_AEC_SAMPLE_RATE_HZ = 16000,
    STARTER_AEC_TRANSPORT_RATE_HZ = 8000,
    /* Full mask 0x0f preserves ES7210 wire order MIC1/MIC3/MIC2/MIC4.
     * MIC1 and MIC2 are people; MIC3 is the electrical playback reference. */
    STARTER_AEC_CAPTURE_DMA_CHANNELS = 4,
    STARTER_AEC_MIC_SLOT = 0,
    STARTER_AEC_REFERENCE_SLOT = 1,
    STARTER_AEC_SECOND_MIC_SLOT = 2,
    STARTER_AEC_MIC_CHANNELS = 2,
};

typedef struct {
    /** AEC 后、降采样前的 16 kHz clean PCM；仅在下一次 process 前有效。 */
    const int16_t *pcm_16k;
    size_t samples_16k;
    /** Read-only wake branch, with its extra processing delay. No resampling. */
    const int16_t *wake_pcm_16k;
    uint32_t wake_delay_ms;
    const int16_t *pcm_8k;
    size_t samples;
    uint32_t mic_clipped;
    uint32_t reference_clipped;
    /** Input mic/ref stamp and processed clean; DSP latency is not compensated. */
    starter_signal_level_t level[3];
    uint32_t measurement_us;
    int64_t captured_ms;
    uint32_t capture_epoch;
    uint32_t mute_epoch;
    uint32_t feed_us;
    /* fetch includes waiting for fresh audio/SE, not pure DSP CPU time. */
    uint32_t fetch_wait_us, agc_us, resample_us;
    starter_signal_level_t uplink_level; /* 8 kHz after AGC/resampling, before TX gain */
} starter_aec_output_t;

typedef struct {
    uint32_t feed_frames, fetch_frames, input_full, fetch_timeouts, max_feed_us;
    int channel_id;
    bool failed;
} starter_aec_stats_t;

/** 创建常驻的 ESP-SR 全双工 AEC，并在 PSRAM 中分配其状态和工作帧。 */
esp_err_t starter_aec_init(void);

/** 返回一次 I2S 读取必须填满的四通道字节数。 */
size_t starter_aec_capture_bytes(void);

/** I2S 驱动从内部 DMA 拷贝到此 PSRAM 缓冲区，不把此地址交给 DMA。 */
int16_t *starter_aec_capture_buffer(void);

/** ADC owner only. Copy one complete frame to the bounded PSRAM input pool.
 * A full pool returns an error; it never blocks I2S or overwrites old PCM. */
esp_err_t starter_aec_submit_capture(size_t capture_bytes, int64_t captured_ms,
                                     uint32_t capture_epoch, uint32_t mute_epoch);

/** Single consumer only. Official AFE dual-mic enhancement, then one shared
 * AGC/resampler. Output storage is valid until the next fetch. */
esp_err_t starter_aec_fetch(starter_aec_output_t *output);
void starter_aec_get_stats(starter_aec_stats_t *stats);

/** ESP-SR AEC 的 16 kHz 帧采样数，用于启动日志和实机诊断。 */
size_t starter_aec_frame_samples(void);

#ifdef __cplusplus
}
#endif

#endif
