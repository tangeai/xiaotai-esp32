#ifndef STARTER_AEC_H
#define STARTER_AEC_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "starter_signal_level.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    STARTER_AEC_SAMPLE_RATE_HZ = 16000,
    STARTER_AEC_TRANSPORT_RATE_HZ = 8000,
    /* ES8311 standard stereo: ADC microphone left, internal DAC reference right.
     * Two I2S channels do not mean two physical microphones. */
    STARTER_AEC_CAPTURE_DMA_CHANNELS = 2,
    STARTER_AEC_MIC_SLOT = 0,
    STARTER_AEC_REFERENCE_SLOT = 1,
};

typedef struct {
    /** AEC/高通后的 16 kHz 唤醒输入；启用 AGC 时含限幅增益和 10 ms 流水延时。 */
    const int16_t *pcm_16k;
    size_t samples_16k;
    uint32_t wake_delay_ms;
    const int16_t *pcm_8k;
    size_t samples;
    uint32_t mic_clipped;
    uint32_t reference_clipped;
    starter_signal_level_t level[3]; /**< mic/ref/clean before AGC, same frame. */
    /* HPF output and AGC output. AGC has 10 ms latency: window levels are
     * diagnostic amplitudes, not sample-aligned gain or echo-reduction ratios. */
    starter_signal_level_t post_level[2];
    uint32_t highpass_clipped;
    uint32_t highpass_us;
    uint32_t measurement_us;
} starter_aec_output_t;

/** 创建常驻的 ESP-SR 全双工 AEC，并在 PSRAM 中分配其状态和工作帧。 */
esp_err_t starter_aec_init(void);

/** 返回一次 I2S 读取必须填满的双通道压紧 DMA 字节数。 */
size_t starter_aec_capture_bytes(void);

/** 返回供 I2S 写入的、16 字节对齐的 MIC/DAC-ref 双通道缓冲区。 */
int16_t *starter_aec_capture_buffer(void);

/**
 * 将 ES8311 麦克风（左声道）和 DAC 回采（右声道）送入 AEC，同时返回只读的
 * 16 kHz AEC/AGC PCM 给本地识别，并抗混叠降采样到 8 kHz 给 TiRTC。
 * capture_bytes 必须等于 starter_aec_capture_bytes()。
 */
esp_err_t starter_aec_process_capture(size_t capture_bytes,
                                      starter_aec_output_t *output);

/** ESP-SR AEC 的 16 kHz 帧采样数，用于启动日志和实机诊断。 */
size_t starter_aec_frame_samples(void);

#ifdef __cplusplus
}
#endif

#endif
