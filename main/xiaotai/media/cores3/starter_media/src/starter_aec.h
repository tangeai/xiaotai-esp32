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
    STARTER_AEC_TRANSPORT_RATE_HZ = 16000,
    /* ES7210 TDM-I2S wire order: MIC1/MIC3-reference/MIC2/MIC4-unused.
     * The AFE processes MIC1 + reference; MIC2 is measured for diagnostics. */
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
    /** AEC/AGC 后直接用于 Opus 的 16 kHz PCM。 */
    const int16_t *transport_pcm;
    size_t transport_samples;
    uint32_t mic_clipped;
    uint32_t reference_clipped;
    /** Input mic/ref stamp and processed clean; DSP latency is not compensated. */
    starter_signal_level_t level[3];
    uint32_t measurement_us;
    int64_t captured_ms;
    uint32_t capture_epoch;
    uint32_t mute_epoch;
    /** Input samples lost immediately before this frame: full input pool, AFE
     * ring full, failed ADC read or a dropped fetch. Zero means continuous. */
    uint32_t lost_samples;
    uint32_t feed_us;
    /* fetch includes waiting for fresh audio/SE, not pure DSP CPU time. */
    uint32_t fetch_wait_us, agc_us;
    starter_signal_level_t uplink_level; /* 16 kHz after AEC/AGC, before TX gain */
} starter_aec_output_t;

typedef struct {
    uint32_t feed_frames, fetch_frames, input_full, fetch_timeouts, max_feed_us;
    /* Recoverable drops: AFE ring back-pressure, fetched frame without stamp. */
    uint32_t feed_ring_full, stamp_timeouts;
    uint32_t feed_avg_us;
    bool detail_enabled;
    uint32_t aec_calls, aec_avg_us, aec_max_us; /* Wall time includes preemption. */
    uint32_t nlp_calls, nlp_avg_us, nlp_max_us;
    uint32_t se_calls, se_avg_us, se_max_us; /* BSS probe; stays zero without SE. */
    uint32_t aec_run_ticks, aec_preempt_ticks;
    uint32_t nlp_run_ticks, nlp_preempt_ticks;
    uint32_t se_run_ticks, se_preempt_ticks;
    uint32_t dsp_cycles[3];
    /* Per stage: all/busy/PIF/store/lookup/uncached/bank/instruction, permille. */
    uint32_t dsp_stall_pm[3][8];
    uint32_t dsp_perf_errors;
    int channel_id;
    bool failed;
} starter_aec_stats_t;

/** 创建常驻 ESP-SR AEC：工作帧在 PSRAM，关键 DSP 工作集采用官方均衡分配。 */
esp_err_t starter_aec_init(void);

/** 返回一次 I2S 读取必须填满的四通道字节数。 */
size_t starter_aec_capture_bytes(void);

/** I2S 驱动从内部 DMA 拷贝到此 PSRAM 缓冲区，不把此地址交给 DMA。 */
int16_t *starter_aec_capture_buffer(void);

/** ADC owner only. Copy one complete frame to the bounded PSRAM input pool.
 * A full pool returns an error; it never blocks I2S or overwrites old PCM. */
esp_err_t starter_aec_submit_capture(size_t capture_bytes, int64_t captured_ms,
                                     uint32_t capture_epoch, uint32_t mute_epoch);
/** ADC owner only. A failed read delivered no frame; the next accepted frame
 * reports one capture block as lost before it. */
void starter_aec_report_capture_loss(void);

/** Single consumer only. Official single-mic FD AEC+NLP on MIC1 with the MIC3
 * reference, then one shared AGC. Output storage is valid until the next fetch. */
esp_err_t starter_aec_fetch(starter_aec_output_t *output);
void starter_aec_get_stats(starter_aec_stats_t *stats);
/** Development-only aggregate, printed by the console, never by DSP calls. */
void starter_aec_log_kernels(void);
esp_err_t starter_aec_debug_detail(bool enabled);

/** ESP-SR AEC 的 16 kHz 帧采样数，用于启动日志和实机诊断。 */
size_t starter_aec_frame_samples(void);

#ifdef __cplusplus
}
#endif

#endif
