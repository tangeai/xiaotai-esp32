#include "device.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "esp_freertos_hooks.h"
#include "nvs.h"
#include "platform_nvs_async.h"
#include "platform_storage.h"

static const char *TAG = "device";

#define DEVICE_BOOT_BUTTON_GPIO GPIO_NUM_0
#define DEVICE_SAMPLE_PERIOD_MS 10
#define DEVICE_CPU_REPORT_PERIOD_MS 250
#define DEVICE_NVS_NAMESPACE    "device"
#define DEVICE_NVS_KEY_UUID     "uuid"

static device_state_t s_device_state;
static device_boot_button_cb_t s_boot_cb;
static void *s_boot_cb_ctx;
static TaskHandle_t s_device_task;
static bool s_device_initialized;
static portMUX_TYPE s_device_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_idle_tick_counts[CONFIG_FREERTOS_NUMBER_OF_CORES];
static uint32_t s_last_idle_tick_counts[CONFIG_FREERTOS_NUMBER_OF_CORES];
static TickType_t s_last_cpu_report_tick;
static bool s_cpu_idle_hooks_registered;

static bool device_idle_hook_cpu0(void)
{
    s_idle_tick_counts[0]++;
    return true;
}

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
static bool device_idle_hook_cpu1(void)
{
    s_idle_tick_counts[1]++;
    return true;
}
#endif

static void device_snapshot_idle_ticks(uint32_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    for (size_t core_index = 0; core_index < CONFIG_FREERTOS_NUMBER_OF_CORES; ++core_index) {
        snapshot[core_index] = s_idle_tick_counts[core_index];
    }
}

static esp_err_t device_register_cpu_idle_hooks(void)
{
    if (s_cpu_idle_hooks_registered) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_register_freertos_idle_hook_for_cpu(device_idle_hook_cpu0, 0),
                        TAG,
                        "register cpu0 idle hook failed");

#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    esp_err_t ret = esp_register_freertos_idle_hook_for_cpu(device_idle_hook_cpu1, 1);
    if (ret != ESP_OK) {
        esp_deregister_freertos_idle_hook_for_cpu(device_idle_hook_cpu0, 0);
        return ret;
    }
#endif

    s_cpu_idle_hooks_registered = true;
    return ESP_OK;
}

static bool device_uuid_char_is_valid(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
}

static esp_err_t device_validate_uuid(const char *uuid)
{
    size_t length = 0;

    if (uuid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    length = strlen(uuid);
    if (length < DEVICE_UUID_MIN_LEN || length > DEVICE_UUID_EDIT_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t index = 0; index < length; ++index) {
        if (!device_uuid_char_is_valid(uuid[index])) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

static esp_err_t device_nvs_init(void)
{
    return platform_storage_init();
}

static esp_err_t device_load_saved_uuid(char *uuid, size_t uuid_len)
{
    nvs_handle_t nvs_handle = 0;
    char saved_uuid[DEVICE_UUID_MAX_LEN] = {0};
    size_t saved_uuid_len = sizeof(saved_uuid);

    esp_err_t ret = nvs_open(DEVICE_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_get_str(nvs_handle, DEVICE_NVS_KEY_UUID, saved_uuid, &saved_uuid_len);
    nvs_close(nvs_handle);
    if (ret != ESP_OK || saved_uuid[0] == '\0') {
        return ret == ESP_OK ? ESP_ERR_NVS_NOT_FOUND : ret;
    }

    ret = device_validate_uuid(saved_uuid);
    if (ret != ESP_OK) {
        return ret;
    }

    strlcpy(uuid, saved_uuid, uuid_len);
    return ESP_OK;
}

static esp_err_t device_save_uuid(const char *uuid)
{
    esp_err_t ret = platform_nvs_async_set_str_and_wait(DEVICE_NVS_NAMESPACE,
                                                         DEVICE_NVS_KEY_UUID,
                                                         uuid);

    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "device uuid saved to nvs");
    }
    return ret;
}

static void device_format_uuid(char *uuid, size_t uuid_len)
{
    uint8_t mac[6] = {0};

    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(uuid, uuid_len,
             "P4%02X%02X%02X%02X%02X",
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
}

static bool device_uuid_is_legacy_test_id(const char *uuid)
{
    return uuid != NULL && strncmp(uuid, "TESTSONGZC", strlen("TESTSONGZC")) == 0;
}

static void device_update_cpu_usage(void)
{
    TickType_t now_tick = xTaskGetTickCount();
    TickType_t report_ticks = pdMS_TO_TICKS(DEVICE_CPU_REPORT_PERIOD_MS);
    uint32_t idle_tick_snapshot[CONFIG_FREERTOS_NUMBER_OF_CORES] = {0};

    if (s_last_cpu_report_tick == 0) {
        s_last_cpu_report_tick = now_tick;
        device_snapshot_idle_ticks(s_last_idle_tick_counts);
        return;
    }

    if (report_ticks == 0) {
        report_ticks = 1;
    }

    TickType_t elapsed_ticks = now_tick - s_last_cpu_report_tick;
    if (elapsed_ticks < report_ticks) {
        return;
    }

    device_snapshot_idle_ticks(idle_tick_snapshot);

    uint64_t total_idle_ticks = 0;
    for (size_t core_index = 0; core_index < CONFIG_FREERTOS_NUMBER_OF_CORES; ++core_index) {
        total_idle_ticks += (uint32_t)(idle_tick_snapshot[core_index] - s_last_idle_tick_counts[core_index]);
        s_last_idle_tick_counts[core_index] = idle_tick_snapshot[core_index];
    }

    uint64_t total_ticks = (uint64_t)elapsed_ticks * CONFIG_FREERTOS_NUMBER_OF_CORES;
    uint32_t busy_percent = 0;
    if (total_ticks > 0) {
        uint64_t idle_percent = ((total_idle_ticks * 100ULL) + (total_ticks / 2ULL)) / total_ticks;
        if (idle_percent > 100ULL) {
            idle_percent = 100ULL;
        }
        busy_percent = (uint32_t)(100ULL - idle_percent);
    }

    taskENTER_CRITICAL(&s_device_lock);
    s_device_state.cpu_usage_percent = (uint8_t)busy_percent;
    taskEXIT_CRITICAL(&s_device_lock);

    s_last_cpu_report_tick = now_tick;
}

static void device_task(void *ctx)
{
    (void)ctx;
    bool last_pressed = false;

    while (true) {
        bool pressed = gpio_get_level(DEVICE_BOOT_BUTTON_GPIO) == 0;
        bool changed = false;
        device_boot_button_cb_t cb = NULL;
        void *cb_ctx = NULL;

        taskENTER_CRITICAL(&s_device_lock);
        if (s_device_state.boot_pressed != pressed) {
            s_device_state.boot_pressed = pressed;
            changed = true;
            cb = s_boot_cb;
            cb_ctx = s_boot_cb_ctx;
        }
        taskEXIT_CRITICAL(&s_device_lock);

        if (changed && pressed != last_pressed) {
            ESP_LOGD(TAG, "boot button changed: pressed=%d", pressed);
        }
        if (changed && cb != NULL) {
            cb(pressed, cb_ctx);
        }

        device_update_cpu_usage();
        last_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(DEVICE_SAMPLE_PERIOD_MS));
    }
}

esp_err_t device_init(device_boot_button_cb_t cb, void *ctx)
{
    char saved_uuid[DEVICE_UUID_MAX_LEN] = {0};

    if (s_device_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(device_nvs_init(), TAG, "device nvs init failed");
    ESP_RETURN_ON_ERROR(device_register_cpu_idle_hooks(), TAG, "cpu idle hook init failed");

    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << DEVICE_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_cfg), TAG, "boot button gpio config failed");

    device_format_uuid(s_device_state.uuid, sizeof(s_device_state.uuid));
    esp_err_t load_ret = device_load_saved_uuid(saved_uuid, sizeof(saved_uuid));
    if (load_ret == ESP_OK && !device_uuid_is_legacy_test_id(saved_uuid)) {
        strlcpy(s_device_state.uuid, saved_uuid, sizeof(s_device_state.uuid));
        ESP_LOGD(TAG, "device uuid loaded from nvs");
    } else if (load_ret == ESP_OK) {
        (void)device_save_uuid(s_device_state.uuid);
        ESP_LOGI(TAG, "legacy test device uuid replaced");
    } else if (load_ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "device uuid load failed: %s", esp_err_to_name(load_ret));
    }
    s_boot_cb = cb;
    s_boot_cb_ctx = ctx;
    s_device_state.boot_pressed = gpio_get_level(DEVICE_BOOT_BUTTON_GPIO) == 0;
    s_last_cpu_report_tick = xTaskGetTickCount();
    device_snapshot_idle_ticks(s_last_idle_tick_counts);
    device_update_cpu_usage();

    BaseType_t task_ok = xTaskCreatePinnedToCoreWithCaps(device_task,
                                                         "device_state",
                                                         3 * 1024,
                                                         NULL,
                                                         3,
                                                         &s_device_task,
                                                         0,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(task_ok == pdPASS, ESP_ERR_NO_MEM, TAG, "device task create failed");

    s_device_initialized = true;
    return ESP_OK;
}

esp_err_t device_set_uuid(const char *uuid)
{
    char next_uuid[DEVICE_UUID_MAX_LEN] = {0};

    if (!s_device_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(device_validate_uuid(uuid), TAG, "invalid device uuid");

    strlcpy(next_uuid, uuid, sizeof(next_uuid));
    ESP_RETURN_ON_ERROR(device_save_uuid(next_uuid), TAG, "save device uuid failed");

    taskENTER_CRITICAL(&s_device_lock);
    strlcpy(s_device_state.uuid, next_uuid, sizeof(s_device_state.uuid));
    taskEXIT_CRITICAL(&s_device_lock);
    return ESP_OK;
}

void device_get_state(device_state_t *state)
{
    if (state == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_device_lock);
    *state = s_device_state;
    taskEXIT_CRITICAL(&s_device_lock);
}
