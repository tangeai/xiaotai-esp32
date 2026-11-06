#ifndef STARTER_PRODUCT_H
#define STARTER_PRODUCT_H

/** @file starter_product.h AIROBOT S3 本地产品屏幕、触摸、设置与休眠控制。 */
#include "esp_err.h"
#include "starter_voice.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize ILI9342C, FT6336U, LVGL and the product UI. */
esp_err_t starter_product_start(void);

typedef enum {
    STARTER_BINDING_CHECKING = 0,
    STARTER_BINDING_REQUIRED,
    STARTER_BINDING_READY,
    STARTER_BINDING_FAILED,
} starter_product_binding_state_t;

/** Startup owns credential validation; this nonblocking UI notification never
 * reads NVS or creates LVGL objects on the caller's task. */
void starter_product_set_binding_state(starter_product_binding_state_t state);

/**
 * 非阻塞投递已经识别出的本地语音意图；UI 与产品状态只在 LVGL 任务内修改。
 * 开发控制台和后续 WakeNet/MultiNet 适配器必须共用此入口。
 */
esp_err_t starter_product_submit_voice(const starter_voice_result_t *result);

#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE
/* Development-only UI evidence: virtual touch uses the normal LVGL input path.
 * Snapshot memory is caller-owned PSRAM and never held during USB output. */
esp_err_t starter_product_debug_tap(uint16_t x, uint16_t y);
esp_err_t starter_product_debug_swipe(uint16_t x, uint16_t y, uint16_t end_x,
                                      uint16_t end_y, uint16_t duration_ms);
esp_err_t starter_product_debug_snapshot(void *pixels, size_t capacity,
                                        size_t *length, unsigned *page);
esp_err_t starter_product_debug_state(unsigned *page, bool *awake);
typedef struct {
    uint64_t pixels, refresh_ms, flush_bytes, wait_us, face_us;
    uint64_t face_clear_us, face_raster_us;
    uint64_t face_blit_us, face_blit_pixels, flush_submit_us;
    uint64_t face_blit_rows, face_byte_rows, face_byte_pixels;
    uint32_t refreshes, flushes, waits, face_frames;
    uint32_t face_blits, face_blit_flags;
} starter_product_display_stats_t;
esp_err_t starter_product_debug_display(starter_product_display_stats_t *stats);
#endif

#ifdef __cplusplus
}
#endif
#endif
