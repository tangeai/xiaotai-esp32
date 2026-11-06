#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_LED_BOOT,
    STATUS_LED_WIFI_PROVISIONING,
    STATUS_LED_BINDING,
    STATUS_LED_CONNECTING,
    STATUS_LED_READY,
    STATUS_LED_OUTGOING,
    STATUS_LED_INCOMING,
    STATUS_LED_CALL_CONNECTING,
    STATUS_LED_ACTIVE,
    STATUS_LED_CLOSING,
    STATUS_LED_ERROR,
} status_led_state_t;

typedef enum {
    STATUS_LED_FEEDBACK_TAP,
    STATUS_LED_FEEDBACK_DOUBLE_TAP,
    STATUS_LED_FEEDBACK_SUCCESS,
    STATUS_LED_FEEDBACK_WARNING,
} status_led_feedback_t;

typedef struct {
    gpio_num_t data_gpio;
    uint8_t pixel_count;
    uint8_t brightness;
} status_led_config_t;

esp_err_t status_led_init(const status_led_config_t *config);
void status_led_set_state(status_led_state_t state);
void status_led_feedback(status_led_feedback_t feedback);
void status_led_tick(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
