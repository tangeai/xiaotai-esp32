#include "platform_nvs_async.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "app_memory_policy.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "platform_storage.h"

static const char *TAG = "nvs_async";

#define PLATFORM_NVS_ASYNC_NAME_MAX        16U
#define PLATFORM_NVS_ASYNC_QUEUE_LEN       8U
#define PLATFORM_NVS_ASYNC_TASK_STACK      (6U * 1024U)
#define PLATFORM_NVS_ASYNC_TASK_PRIORITY   3
#define PLATFORM_NVS_ASYNC_ENQUEUE_WAIT_MS 50U

typedef enum {
    PLATFORM_NVS_ASYNC_OP_SET_BLOB = 0,
    PLATFORM_NVS_ASYNC_OP_SET_STR,
    PLATFORM_NVS_ASYNC_OP_SET_U8,
    PLATFORM_NVS_ASYNC_OP_ERASE_KEY,
    PLATFORM_NVS_ASYNC_OP_GET_BLOB,
} platform_nvs_async_op_t;

typedef struct {
    platform_nvs_async_op_t op;
    char namespace_name[PLATFORM_NVS_ASYNC_NAME_MAX];
    char key[PLATFORM_NVS_ASYNC_NAME_MAX];
    size_t value_len;
    size_t result_len;
    esp_err_t result;
    SemaphoreHandle_t done;
    uint8_t value[];
} platform_nvs_async_request_t;

static QueueHandle_t s_nvs_async_queue;
static TaskHandle_t s_nvs_async_task;

static bool platform_nvs_name_valid(const char *value)
{
    return value != NULL &&
           value[0] != '\0' &&
           strnlen(value, PLATFORM_NVS_ASYNC_NAME_MAX) < PLATFORM_NVS_ASYNC_NAME_MAX;
}

static esp_err_t platform_nvs_run_request(platform_nvs_async_request_t *request)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(platform_storage_init(), TAG, "nvs init failed");

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(request->namespace_name,
                             request->op == PLATFORM_NVS_ASYNC_OP_GET_BLOB ?
                             NVS_READONLY : NVS_READWRITE,
                             &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    switch (request->op) {
    case PLATFORM_NVS_ASYNC_OP_SET_BLOB:
        ret = nvs_set_blob(handle, request->key, request->value, request->value_len);
        break;
    case PLATFORM_NVS_ASYNC_OP_SET_STR:
        ret = nvs_set_str(handle, request->key, (const char *)request->value);
        break;
    case PLATFORM_NVS_ASYNC_OP_SET_U8:
        ret = request->value_len == sizeof(uint8_t) ?
              nvs_set_u8(handle, request->key, request->value[0]) :
              ESP_ERR_INVALID_SIZE;
        break;
    case PLATFORM_NVS_ASYNC_OP_ERASE_KEY:
        ret = nvs_erase_key(handle, request->key);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
        break;
    case PLATFORM_NVS_ASYNC_OP_GET_BLOB:
        request->result_len = request->value_len;
        ret = nvs_get_blob(handle, request->key, request->value, &request->result_len);
        break;
    default:
        ret = ESP_ERR_INVALID_ARG;
        break;
    }

    if (ret == ESP_OK && request->op != PLATFORM_NVS_ASYNC_OP_GET_BLOB) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

static void platform_nvs_async_task(void *ctx)
{
    (void)ctx;

    while (true) {
        platform_nvs_async_request_t *request = NULL;
        if (xQueueReceive(s_nvs_async_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        esp_err_t ret = platform_nvs_run_request(request);
        request->result = ret;
        if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG,
                     "nvs op failed: ns=%s key=%s op=%d ret=%s",
                     request->namespace_name,
                     request->key,
                     (int)request->op,
                     esp_err_to_name(ret));
        }

        if (request->done != NULL) {
            xSemaphoreGive(request->done);
        } else {
            free(request);
        }
    }
}

esp_err_t platform_nvs_async_init(void)
{
    if (s_nvs_async_queue == NULL) {
        s_nvs_async_queue = xQueueCreateWithCaps(PLATFORM_NVS_ASYNC_QUEUE_LEN,
                                                sizeof(platform_nvs_async_request_t *),
                                                APP_QUEUE_CAPS_CONTROL);
        ESP_RETURN_ON_FALSE(s_nvs_async_queue != NULL,
                            ESP_ERR_NO_MEM,
                            TAG,
                            "create nvs queue failed");
    }

    if (s_nvs_async_task != NULL) {
        return ESP_OK;
    }

    BaseType_t task_ret = xTaskCreateWithCaps(platform_nvs_async_task,
                                              "nvs_async",
                                              PLATFORM_NVS_ASYNC_TASK_STACK,
                                              NULL,
                                              PLATFORM_NVS_ASYNC_TASK_PRIORITY,
                                              &s_nvs_async_task,
                                              APP_TASK_STACK_CAPS_INTERNAL);
    if (task_ret != pdPASS) {
        s_nvs_async_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "nvs worker ready: stack=%u caps=internal queue=%u",
             (unsigned)PLATFORM_NVS_ASYNC_TASK_STACK,
             (unsigned)PLATFORM_NVS_ASYNC_QUEUE_LEN);
    return ESP_OK;
}

static esp_err_t platform_nvs_enqueue_request(platform_nvs_async_op_t op,
                                              const char *namespace_name,
                                              const char *key,
                                              const void *value,
                                              size_t value_len,
                                              bool wait_for_completion)
{
    if (!platform_nvs_name_valid(namespace_name) || !platform_nvs_name_valid(key)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (((op == PLATFORM_NVS_ASYNC_OP_SET_BLOB && value_len > 0U) ||
         op == PLATFORM_NVS_ASYNC_OP_SET_STR ||
         op == PLATFORM_NVS_ASYNC_OP_SET_U8) && value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(platform_nvs_async_init(), TAG, "nvs worker init failed");

    platform_nvs_async_request_t *request =
        heap_caps_calloc(1,
                         sizeof(*request) + value_len,
                         APP_MEMORY_CAPS_CONTROL);
    if (request == NULL) {
        return ESP_ERR_NO_MEM;
    }

    request->op = op;
    strlcpy(request->namespace_name, namespace_name, sizeof(request->namespace_name));
    strlcpy(request->key, key, sizeof(request->key));
    request->value_len = value_len;
    if (value_len > 0U) {
        memcpy(request->value, value, value_len);
    }

    SemaphoreHandle_t done = NULL;
    if (wait_for_completion) {
        done = xSemaphoreCreateBinaryWithCaps(APP_SYNC_CAPS_CONTROL);
        if (done == NULL) {
            free(request);
            return ESP_ERR_NO_MEM;
        }
        request->done = done;
    }

    if (xQueueSend(s_nvs_async_queue,
                   &request,
                   pdMS_TO_TICKS(PLATFORM_NVS_ASYNC_ENQUEUE_WAIT_MS)) != pdPASS) {
        if (done != NULL) {
            vSemaphoreDeleteWithCaps(done);
        }
        free(request);
        return ESP_ERR_TIMEOUT;
    }
    if (!wait_for_completion) {
        return ESP_OK;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    esp_err_t ret = request->result;
    vSemaphoreDeleteWithCaps(done);
    free(request);
    return ret;
}

esp_err_t platform_nvs_async_set_blob(const char *namespace_name,
                                      const char *key,
                                      const void *value,
                                      size_t value_len)
{
    if (value_len > 0U && value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return platform_nvs_enqueue_request(PLATFORM_NVS_ASYNC_OP_SET_BLOB,
                                        namespace_name,
                                        key,
                                        value,
                                        value_len,
                                        false);
}

esp_err_t platform_nvs_async_set_blob_and_wait(const char *namespace_name,
                                               const char *key,
                                               const void *value,
                                               size_t value_len)
{
    if (value_len > 0U && value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return platform_nvs_enqueue_request(PLATFORM_NVS_ASYNC_OP_SET_BLOB,
                                        namespace_name,
                                        key,
                                        value,
                                        value_len,
                                        true);
}

esp_err_t platform_nvs_async_set_str(const char *namespace_name,
                                     const char *key,
                                     const char *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return platform_nvs_enqueue_request(PLATFORM_NVS_ASYNC_OP_SET_STR,
                                        namespace_name,
                                        key,
                                        value,
                                        strlen(value) + 1U,
                                        false);
}

esp_err_t platform_nvs_async_set_str_and_wait(const char *namespace_name,
                                              const char *key,
                                              const char *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return platform_nvs_enqueue_request(PLATFORM_NVS_ASYNC_OP_SET_STR,
                                        namespace_name,
                                        key,
                                        value,
                                        strlen(value) + 1U,
                                        true);
}

esp_err_t platform_nvs_async_set_u8(const char *namespace_name,
                                    const char *key,
                                    uint8_t value)
{
    return platform_nvs_enqueue_request(PLATFORM_NVS_ASYNC_OP_SET_U8,
                                        namespace_name,
                                        key,
                                        &value,
                                        sizeof(value),
                                        false);
}

esp_err_t platform_nvs_async_set_u8_and_wait(const char *namespace_name,
                                             const char *key,
                                             uint8_t value)
{
    return platform_nvs_enqueue_request(PLATFORM_NVS_ASYNC_OP_SET_U8,
                                        namespace_name,
                                        key,
                                        &value,
                                        sizeof(value),
                                        true);
}

esp_err_t platform_nvs_async_erase_key(const char *namespace_name,
                                       const char *key)
{
    return platform_nvs_enqueue_request(PLATFORM_NVS_ASYNC_OP_ERASE_KEY,
                                        namespace_name,
                                        key,
                                        NULL,
                                        0,
                                        false);
}

esp_err_t platform_nvs_async_erase_key_and_wait(const char *namespace_name,
                                                const char *key)
{
    return platform_nvs_enqueue_request(PLATFORM_NVS_ASYNC_OP_ERASE_KEY,
                                        namespace_name,
                                        key,
                                        NULL,
                                        0,
                                        true);
}

esp_err_t platform_nvs_async_get_blob(const char *namespace_name,
                                      const char *key,
                                      void *value,
                                      size_t *value_len)
{
    if (!platform_nvs_name_valid(namespace_name) ||
        !platform_nvs_name_valid(key) ||
        value == NULL ||
        value_len == NULL ||
        *value_len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(platform_nvs_async_init(), TAG, "nvs worker init failed");

    const size_t max_len = *value_len;
    platform_nvs_async_request_t *request =
        heap_caps_calloc(1,
                         sizeof(*request) + max_len,
                         APP_MEMORY_CAPS_CONTROL);
    if (request == NULL) {
        return ESP_ERR_NO_MEM;
    }

    SemaphoreHandle_t done = xSemaphoreCreateBinaryWithCaps(APP_SYNC_CAPS_CONTROL);
    if (done == NULL) {
        free(request);
        return ESP_ERR_NO_MEM;
    }

    request->op = PLATFORM_NVS_ASYNC_OP_GET_BLOB;
    strlcpy(request->namespace_name, namespace_name, sizeof(request->namespace_name));
    strlcpy(request->key, key, sizeof(request->key));
    request->value_len = max_len;
    request->done = done;

    if (xQueueSend(s_nvs_async_queue,
                   &request,
                   pdMS_TO_TICKS(PLATFORM_NVS_ASYNC_ENQUEUE_WAIT_MS)) != pdPASS) {
        vSemaphoreDeleteWithCaps(done);
        free(request);
        return ESP_ERR_TIMEOUT;
    }

    xSemaphoreTake(done, portMAX_DELAY);
    esp_err_t ret = request->result;
    *value_len = request->result_len;
    if (ret == ESP_OK) {
        memcpy(value, request->value, request->result_len);
    }

    vSemaphoreDeleteWithCaps(done);
    free(request);
    return ret;
}
