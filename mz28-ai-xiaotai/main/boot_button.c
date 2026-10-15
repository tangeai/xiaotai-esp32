#include "boot_button.h"
#include "boot_button_logic.h"

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#define BUTTON_POLL_MS 20
#define BUTTON_DEBOUNCE_SAMPLES 3

static gpio_num_t s_gpio;
static boot_button_pressed_cb_t s_single_click_cb;
static boot_button_pressed_cb_t s_double_click_cb;
static boot_button_pressed_cb_t s_quadruple_click_cb;
static boot_button_pressed_cb_t s_long_press_cb;
static uint32_t s_long_press_ms;
static uint32_t s_multi_click_ms;
static StaticTimer_t s_timer_storage;
static TimerHandle_t s_timer;
static int s_stable_level;
static int s_candidate_level;
static uint8_t s_candidate_samples;
static boot_button_logic_t s_logic;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void dispatch_button_event(boot_button_event_t event)
{
    if (event == BOOT_BUTTON_EVENT_SINGLE_CLICK && s_single_click_cb) {
        s_single_click_cb();
    } else if (event == BOOT_BUTTON_EVENT_DOUBLE_CLICK &&
               s_double_click_cb) {
        s_double_click_cb();
    } else if (event == BOOT_BUTTON_EVENT_QUADRUPLE_CLICK &&
               s_quadruple_click_cb) {
        s_quadruple_click_cb();
    } else if (event == BOOT_BUTTON_EVENT_LONG_PRESS && s_long_press_cb) {
        s_long_press_cb();
    }
}

static void poll_button(TimerHandle_t timer)
{
    (void)timer;
    int level = gpio_get_level(s_gpio);
    if (level == s_stable_level) {
        s_candidate_level = level;
        s_candidate_samples = 0;
        dispatch_button_event(boot_button_logic_update(
            &s_logic, level == 0, now_ms(), s_long_press_ms,
            s_multi_click_ms));
        return;
    }
    if (level != s_candidate_level) {
        s_candidate_level = level;
        s_candidate_samples = 1;
        return;
    }
    if (++s_candidate_samples < BUTTON_DEBOUNCE_SAMPLES)
        return;

    s_stable_level = level;
    s_candidate_samples = 0;
    dispatch_button_event(boot_button_logic_update(
        &s_logic, level == 0, now_ms(), s_long_press_ms,
        s_multi_click_ms));
}

esp_err_t boot_button_start(gpio_num_t gpio,
                            boot_button_pressed_cb_t single_click_cb,
                            boot_button_pressed_cb_t double_click_cb,
                            boot_button_pressed_cb_t quadruple_click_cb,
                            boot_button_pressed_cb_t long_press_cb,
                            uint32_t long_press_ms,
                            uint32_t multi_click_ms)
{
    if (s_timer || !single_click_cb || !double_click_cb ||
        !quadruple_click_cb || !long_press_cb || long_press_ms == 0 ||
        multi_click_ms == 0 || multi_click_ms >= long_press_ms || gpio < 0 ||
        gpio >= GPIO_NUM_MAX)
        return ESP_ERR_INVALID_ARG;

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK)
        return err;

    s_gpio = gpio;
    s_single_click_cb = single_click_cb;
    s_double_click_cb = double_click_cb;
    s_quadruple_click_cb = quadruple_click_cb;
    s_long_press_cb = long_press_cb;
    s_long_press_ms = long_press_ms;
    s_multi_click_ms = multi_click_ms;
    s_stable_level = gpio_get_level(gpio);
    s_candidate_level = s_stable_level;
    boot_button_logic_init(&s_logic, s_stable_level == 0, now_ms());
    s_timer = xTimerCreateStatic("boot_btn", pdMS_TO_TICKS(BUTTON_POLL_MS), pdTRUE, NULL,
                                 poll_button, &s_timer_storage);
    if (!s_timer)
        return ESP_ERR_NO_MEM;
    if (xTimerStart(s_timer, 0) != pdPASS) {
        s_timer = NULL;
        s_single_click_cb = NULL;
        s_double_click_cb = NULL;
        s_quadruple_click_cb = NULL;
        s_long_press_cb = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
