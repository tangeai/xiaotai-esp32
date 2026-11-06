#ifndef STARTER_PRODUCT_H
#define STARTER_PRODUCT_H

/** @file starter_product.h AIROBOT S3 本地产品屏幕、触摸、设置与休眠控制。 */
#include "esp_err.h"
#include "starter_voice.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 ST7789、FT6336/FT5x06 兼容触摸、LVGL 和产品 UI；重复调用安全。 */
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

#ifdef __cplusplus
}
#endif
#endif
