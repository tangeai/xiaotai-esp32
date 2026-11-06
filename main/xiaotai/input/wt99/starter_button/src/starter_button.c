/*
 * 立创·实战派 ESP32-S3 V1.0.1 板载 BOOT/用户按键。
 *
 * 官方原理图将 SW2 接到 GPIO0：松开时 10 kΩ 上拉，按下时接地。本任务
 * 采用轮询消抖，避免占用全局 GPIO ISR service。按住只触发一次，并且启动
 * 时若按键已经按下，必须先松开才会响应，避免下载/复位操作误开 AI。
 */
#include "starter_button.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "starter_runtime.h"

#if CONFIG_IDF_TARGET_ESP32P4
#define AI_BUTTON_GPIO GPIO_NUM_35
#else
#define AI_BUTTON_GPIO GPIO_NUM_0
#endif
#define BUTTON_POLL_MS 10U
#define BUTTON_DEBOUNCE_MS 50U
#define BUTTON_TASK_STACK_BYTES 3072U
#define BUTTON_TASK_PRIORITY 4U

static const char *TAG = "starter_button";
static TaskHandle_t s_button_task;

static void toggle_ai(void)
{
    starter_runtime_status_t status = starter_runtime_status();
    esp_err_t err;
    if (status.state == STARTER_RUNTIME_AI_CONNECTING ||
        status.state == STARTER_RUNTIME_AI_ACTIVE) {
        err = starter_runtime_ai_stop();
        if (err == ESP_OK) {
            ESP_LOGI(TAG,
                     "BOOT button queued AI stop from state=%s",
                     starter_runtime_state_name(status.state));
        }
    } else {
        err = starter_runtime_ai_start();
        if (err == ESP_OK) {
            ESP_LOGI(TAG,
                     "BOOT button queued AI start from state=%s",
                     starter_runtime_state_name(status.state));
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "BOOT button request ignored in state=%s: %s",
                 starter_runtime_state_name(status.state),
                 esp_err_to_name(err));
    }
}

static void button_task(void *argument)
{
    (void)argument;
    bool stable_pressed = gpio_get_level(AI_BUTTON_GPIO) == 0;
    bool candidate_pressed = stable_pressed;
    bool armed = !stable_pressed;
    uint32_t candidate_ms = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        bool pressed = gpio_get_level(AI_BUTTON_GPIO) == 0;
        if (pressed != candidate_pressed) {
            candidate_pressed = pressed;
            candidate_ms = 0;
            continue;
        }
        if (candidate_ms < BUTTON_DEBOUNCE_MS) {
            candidate_ms += BUTTON_POLL_MS;
            continue;
        }
        if (stable_pressed == candidate_pressed) {
            continue;
        }

        stable_pressed = candidate_pressed;
        if (!stable_pressed) {
            armed = true;
        } else if (armed) {
            armed = false;
            toggle_ai();
        }
    }
}

esp_err_t starter_button_start(void)
{
    if (s_button_task != NULL) {
        return ESP_OK;
    }
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << AI_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreateWithCaps(button_task,
                            "ai_button",
                            BUTTON_TASK_STACK_BYTES,
                            NULL,
                            BUTTON_TASK_PRIORITY,
                            &s_button_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_button_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "BOOT/user button ready on GPIO%d; press once to toggle AI talk",
             AI_BUTTON_GPIO);
    return ESP_OK;
}
