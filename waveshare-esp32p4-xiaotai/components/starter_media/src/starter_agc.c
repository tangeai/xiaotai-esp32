#include "starter_agc.h"
#include "starter_agc_stream.h"
#include "esp_agc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "sdkconfig.h"

static EXT_RAM_BSS_ATTR struct {
    void *handle;
    starter_agc_stream_t stream;
} s_agc;

/* TX-owner only: fixed gain after resampling, never fed back into wake/AEC.
 * One 8 kHz/10 ms scratch frame avoids relying on vendor in-place support. */
#define UPLINK_BOOST_SAMPLES 80U
/* ESP-SR accepts integer dB only. 10 dB plus this input attenuation gives
 * 9.5219 dB: nominally 1.5x the previous 6 dB stage, below limiter onset.
 * Attenuate before the vendor limiter, never amplify int16 PCM into clipping.
 * No analog, AEC/reference, wake or speaker gain is changed. */
#define UPLINK_BOOST_GAIN_DB 10
#define UPLINK_BOOST_INPUT_Q15 31013
static EXT_RAM_BSS_ATTR struct {
    void *handle;
    int16_t input[UPLINK_BOOST_SAMPLES];
} s_uplink_boost;

esp_err_t starter_agc_init(void)
{
    if (s_agc.handle) return ESP_OK;
    /* Verify the vendor allocation instead of assuming its heap policy. */
    s_agc.handle = esp_agc_open(AGC_MODE_2, 16000);
    if (!s_agc.handle) return ESP_ERR_NO_MEM;
    if (!esp_ptr_external_ram(s_agc.handle)) {
        starter_agc_deinit();
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Preserve the 16 kHz adaptive frontend used by wake recognition.
     * Recording gain belongs to the independent 8 kHz TX limiter below. */
    set_agc_config(s_agc.handle, CONFIG_XIAOTAI_CAPTURE_AGC_GAIN_DB, 1, 6);
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    s_uplink_boost.handle = esp_agc_open(AGC_MODE_3, 8000);
    if (!s_uplink_boost.handle || !esp_ptr_external_ram(s_uplink_boost.handle)) {
        esp_err_t err = s_uplink_boost.handle ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_NO_MEM;
        starter_agc_deinit();
        return err;
    }
    set_agc_config(s_uplink_boost.handle, UPLINK_BOOST_GAIN_DB, 1, 3);
#endif
    ESP_LOGI("starter_agc", "WebRTC adaptive digital: compression=%ddB target=-6dBFS limiter=on state=PSRAM delay=10ms",
             CONFIG_XIAOTAI_CAPTURE_AGC_GAIN_DB);
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    ESP_LOGI("starter_agc", "TX nominal gain=9.52dB (previous x1.5) limiter=on target=-3dBFS state=PSRAM; wake unchanged");
#endif
    return ESP_OK;
}

void starter_agc_deinit(void)
{
    if (s_agc.handle) esp_agc_close(s_agc.handle);
    if (s_uplink_boost.handle) esp_agc_close(s_uplink_boost.handle);
    memset(&s_agc, 0, sizeof(s_agc));
    memset(&s_uplink_boost, 0, sizeof(s_uplink_boost));
}

static bool process_frame(void *context, int16_t *input, int16_t *output)
{
    return esp_agc_process(context, input, output, STARTER_AGC_SAMPLES, 16000) >= ESP_AGC_SUCCESS;
}

void starter_agc_discard_pending(void)
{
    memset(&s_agc.stream, 0, sizeof(s_agc.stream));
}

esp_err_t starter_agc_process(const int16_t *clean, int16_t *uplink, size_t samples)
{
    if (!s_agc.handle) return ESP_ERR_INVALID_STATE;
    if (!clean || !uplink || !samples) return ESP_ERR_INVALID_ARG;
    return starter_agc_stream_process(&s_agc.stream, clean, uplink, samples,
                                      process_frame, s_agc.handle) ? ESP_OK : ESP_FAIL;
}

esp_err_t starter_agc_boost_uplink(int16_t *pcm, size_t samples)
{
    if (!s_uplink_boost.handle) return ESP_ERR_INVALID_STATE;
    if (!pcm || !samples || samples % UPLINK_BOOST_SAMPLES) return ESP_ERR_INVALID_ARG;
    for (size_t offset = 0; offset < samples; offset += UPLINK_BOOST_SAMPLES) {
        for (size_t i = 0; i < UPLINK_BOOST_SAMPLES; ++i) {
            int32_t scaled = (int32_t)pcm[offset + i] * UPLINK_BOOST_INPUT_Q15;
            s_uplink_boost.input[i] = (int16_t)(
                (scaled + (scaled >= 0 ? 16384 : -16384)) / 32768);
        }
        if (esp_agc_process(s_uplink_boost.handle, s_uplink_boost.input, pcm + offset,
                            UPLINK_BOOST_SAMPLES, 8000) < ESP_AGC_SUCCESS) return ESP_FAIL;
    }
    return ESP_OK;
}
