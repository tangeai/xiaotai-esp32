/* CoreS3-SE power-key short press. GPIO0 is reserved for audio MCLK. */
#include "starter_button.h"

#include <stdbool.h>
#include <stdint.h>

#include "starter_board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "starter_runtime.h"

#define BUTTON_POLL_MS 50U
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
                     "PMIC key queued AI stop from state=%s",
                     starter_runtime_state_name(status.state));
        }
    } else {
        err = starter_runtime_ai_start();
        if (err == ESP_OK) {
            ESP_LOGI(TAG,
                     "PMIC key queued AI start from state=%s",
                     starter_runtime_state_name(status.state));
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "PMIC key request ignored in state=%s: %s",
                 starter_runtime_state_name(status.state),
                 esp_err_to_name(err));
    }
}

static void button_task(void *argument)
{
    (void)argument;
    unsigned errors = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        uint8_t press = 0;
        esp_err_t err = starter_board_power_key(&press);
        if (err != ESP_OK) {
            if (errors++ % 100 == 0)
                ESP_LOGE(TAG, "PMIC key read failed: %s", esp_err_to_name(err));
        } else {
            errors = 0;
            if (press & 2) toggle_ai();
        }
    }
}

esp_err_t starter_button_start(void)
{
    if (s_button_task != NULL) {
        return ESP_OK;
    }
    uint8_t discard;
    esp_err_t err = starter_board_power_key(&discard);
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
             "PMIC short press toggles AI; GPIO0 remains MCLK");
    return ESP_OK;
}
