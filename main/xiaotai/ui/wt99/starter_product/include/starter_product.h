#ifndef STARTER_PRODUCT_H
#define STARTER_PRODUCT_H

#include <stdbool.h>
#include <stdint.h>

/** @file starter_product.h AIROBOT S3 本地产品屏幕、触摸、设置与休眠控制。 */
#include "esp_err.h"
#include "starter_voice.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 ST7789、FT6336/FT5x06 兼容触摸、LVGL 和产品 UI；重复调用安全。 */
esp_err_t starter_product_start(void);

/** Bounded UART maintenance requests; consumed by the LVGL owner, never a
 * second UI writer. These controls cannot dial, accept or unlock a door. */
typedef enum {
    STARTER_PRODUCT_DOOR_STATUS = 1,
    STARTER_PRODUCT_DOOR_HOME,
    STARTER_PRODUCT_DOOR_NEXT,
    STARTER_PRODUCT_DOOR_PREVIOUS,
    STARTER_PRODUCT_DOOR_VIEWER,
    STARTER_PRODUCT_DOOR_SENDER,
    STARTER_PRODUCT_DOOR_DEVICES,
    STARTER_PRODUCT_DOOR_BACK,
} starter_product_door_control_t;
esp_err_t starter_product_door_control(starter_product_door_control_t control);

/** UART-only UI inspection and pointer samples, consumed by the LVGL owner. */
esp_err_t starter_product_ui_inspect(void);
esp_err_t starter_product_ui_pointer(uint16_t x, uint16_t y, bool pressed);

typedef enum {
    STARTER_BINDING_CHECKING = 0,
    STARTER_BINDING_REQUIRED,
    STARTER_BINDING_READY,
    STARTER_BINDING_FAILED,
    STARTER_BINDING_EXPIRED,
} starter_product_binding_state_t;

typedef enum {
    STARTER_INITIALIZATION_PREPARING = 0,
    STARTER_INITIALIZATION_TIRTC,
    STARTER_INITIALIZATION_PLATFORM,
    STARTER_INITIALIZATION_READY,
    STARTER_INITIALIZATION_FAILED,
} starter_product_initialization_state_t;

/** Startup owns credential validation; this nonblocking UI notification never
 * reads NVS or creates LVGL objects on the caller's task. */
void starter_product_set_binding_state(starter_product_binding_state_t state);

/** Startup publishes its current readiness stage without touching LVGL.
 * Business pages remain hidden until TiRTC and platform signaling are ready. */
void starter_product_set_initialization_state(starter_product_initialization_state_t state);

typedef enum {
    STARTER_COMPANION_UPDATE_CHECKING = 0,
    STARTER_COMPANION_UPDATE_VERIFYING,
    STARTER_COMPANION_UPDATE_READY,
    STARTER_COMPANION_UPDATE_RECOVERY,
    STARTER_COMPANION_UPDATE_UNSUPPORTED,
    STARTER_COMPANION_UPDATE_STARTUP_FAILED,
} starter_product_companion_update_state_t;

/** Nonblocking C61 check status. This board never writes companion firmware. */
void starter_product_set_companion_update(starter_product_companion_update_state_t state,
                                   uint8_t percent, esp_err_t error);
/** Short, read-only C61 firmware identity. */
void starter_product_set_companion_identity(const char *identity);
bool starter_product_take_companion_retry(void);
bool starter_product_request_companion_retry(void);

/**
 * 非阻塞投递已经识别出的本地语音意图；UI 与产品状态只在 LVGL 任务内修改。
 * 开发控制台和后续 WakeNet/MultiNet 适配器必须共用此入口。
 */
esp_err_t starter_product_submit_voice(const starter_voice_result_t *result);

#ifdef __cplusplus
}
#endif
#endif
