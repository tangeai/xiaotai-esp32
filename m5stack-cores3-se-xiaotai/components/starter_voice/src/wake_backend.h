#pragma once
#include "esp_err.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t wake_backend_init(void);
esp_err_t wake_backend_run(const int16_t *pcm, float *probability);
esp_err_t wake_backend_run_window(const int16_t *pcm, uint32_t epoch,
                                  uint64_t end_sample, float *probability);
typedef struct {
    uint32_t mel_us, invoke_us, head_us, mel_reused_frames;
} wake_backend_timing_t;
/* The inference owner reads this after run; other tasks use voice_status. */
wake_backend_timing_t wake_backend_last_timing(void);
void wake_backend_deinit(void);
#ifdef __cplusplus
}
#endif
