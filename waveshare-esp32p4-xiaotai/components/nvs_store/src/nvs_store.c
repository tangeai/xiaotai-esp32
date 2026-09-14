#include "nvs_store.h"

#include <stdbool.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define STORE_STACK_BYTES 3072U
#define STORE_NAME_BYTES 16U
_Static_assert(NVS_STORE_SLOTS == 8U, "ticket layout requires eight slots");
typedef struct {
    nvs_store_kind_t kind;
    char key[STORE_NAME_BYTES];
    uint16_t offset, size;
} stored_op_t;
typedef struct {
    char namespace_name[STORE_NAME_BYTES];
    uint8_t count;
    stored_op_t ops[NVS_STORE_MAX_OPS];
    uint32_t read_size;
    uint8_t data[NVS_STORE_MAX_DATA];
} stored_job_t;
typedef enum { SLOT_FREE, SLOT_QUEUED, SLOT_RUNNING, SLOT_DONE } slot_state_t;
typedef struct {
    nvs_store_ticket_t ticket;
    slot_state_t state;
    esp_err_t result;
    bool abandoned;
} slot_t;

static const char *TAG = "nvs_store";
static EXT_RAM_BSS_ATTR stored_job_t s_jobs[NVS_STORE_SLOTS];
/* The sole writer stages into internal BSS before any cache-disabling call.
 * Large payloads do not increase its stack or each client's internal heap. */
static DRAM_ATTR stored_job_t s_active;
static DRAM_ATTR slot_t s_slots[NVS_STORE_SLOTS];
static DRAM_ATTR StackType_t s_stack[STORE_STACK_BYTES / sizeof(StackType_t)];
static DRAM_ATTR StaticTask_t s_task_storage;
static DRAM_ATTR StaticQueue_t s_queue_storage;
static DRAM_ATTR uint8_t s_queue_bytes[NVS_STORE_SLOTS];
static DRAM_ATTR StaticSemaphore_t s_lock_storage;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static uint32_t s_sequence;

static void wipe(void *memory, size_t size)
{
    volatile uint8_t *bytes = memory;
    while (size--) *bytes++ = 0;
}

static bool valid_name(const char *name)
{
    return name && name[0] && strnlen(name, STORE_NAME_BYTES) < STORE_NAME_BYTES;
}

static esp_err_t validate(const char *name, const nvs_store_op_t *ops, size_t count)
{
    if (!valid_name(name) || !ops || count == 0 || count > NVS_STORE_MAX_OPS)
        return ESP_ERR_INVALID_ARG;
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        const nvs_store_op_t *op = &ops[i];
        if (op->kind < NVS_STORE_U8 || op->kind > NVS_STORE_GET_BLOB ||
            (op->kind != NVS_STORE_ERASE_ALL && !valid_name(op->key)))
            return ESP_ERR_INVALID_ARG;
        if (op->size > NVS_STORE_MAX_DATA - used) return ESP_ERR_INVALID_SIZE;
        if (op->kind <= NVS_STORE_BLOB) {
            if (!op->value || op->size == 0) return ESP_ERR_INVALID_ARG;
            if (op->kind == NVS_STORE_U8 && op->size != 1) return ESP_ERR_INVALID_SIZE;
            if (op->kind == NVS_STORE_STRING &&
                strnlen(op->value, op->size) != op->size - 1) return ESP_ERR_INVALID_ARG;
        } else if (op->kind >= NVS_STORE_GET_U8) {
            if (count != 1 || op->value || op->size == 0) return ESP_ERR_INVALID_ARG;
            if (op->kind == NVS_STORE_GET_U8 && op->size != 1) return ESP_ERR_INVALID_SIZE;
        } else if (op->size != 0 || op->value != NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        used += op->size;
    }
    return ESP_OK;
}

static bool is_read(const stored_job_t *job)
{
    return job->count == 1 && job->ops[0].kind >= NVS_STORE_GET_U8;
}

static esp_err_t run_job(stored_job_t *job)
{
    nvs_handle_t handle = 0;
    bool read = is_read(job);
    esp_err_t result = nvs_open(job->namespace_name, read ? NVS_READONLY : NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    if (read) {
        const stored_op_t *op = &job->ops[0];
        size_t read_size = op->size;
        switch (op->kind) {
        case NVS_STORE_GET_U8: result = nvs_get_u8(handle, op->key, job->data); break;
        case NVS_STORE_GET_STRING: result = nvs_get_str(handle, op->key, (char *)job->data, &read_size); break;
        case NVS_STORE_GET_BLOB: result = nvs_get_blob(handle, op->key, job->data, &read_size); break;
        default: result = ESP_ERR_INVALID_ARG; break;
        }
        job->read_size = read_size;
        nvs_close(handle);
        return result;
    }
    for (unsigned i = 0; i < job->count && result == ESP_OK; ++i) {
        const stored_op_t *op = &job->ops[i];
        const uint8_t *value = job->data + op->offset;
        switch (op->kind) {
        case NVS_STORE_U8: result = nvs_set_u8(handle, op->key, value[0]); break;
        case NVS_STORE_STRING: result = nvs_set_str(handle, op->key, (const char *)value); break;
        case NVS_STORE_BLOB: result = nvs_set_blob(handle, op->key, value, op->size); break;
        case NVS_STORE_ERASE_KEY:
            result = nvs_erase_key(handle, op->key);
            if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
            break;
        case NVS_STORE_ERASE_ALL: result = nvs_erase_all(handle); break;
        default: result = ESP_ERR_INVALID_ARG; break;
        }
    }
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

static void store_task(void *argument)
{
    (void)argument;
    bool reported = false;
    bool stack_warning = false;
    for (;;) {
        uint8_t index;
        if (xQueueReceive(s_queue, &index, portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        slot_t *slot = &s_slots[index];
        s_active = s_jobs[index];
        wipe(&s_jobs[index], sizeof(s_jobs[index]));
        slot->state = SLOT_RUNNING;
        xSemaphoreGive(s_lock);
        esp_err_t result = run_job(&s_active);
        bool read = is_read(&s_active);
        unsigned stack_free = (unsigned)uxTaskGetStackHighWaterMark(NULL);
        if (!read && !reported) {
            ESP_LOGI(TAG, "first write: result=0x%x stack-free=%u", result, stack_free);
            reported = true;
        }
        if (!stack_warning && stack_free < 512U) {
            ESP_LOGW(TAG, "writer stack low: free=%u", stack_free);
            stack_warning = true;
        }
        if (!read && result != ESP_OK) ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(result));
        if (read && result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGE(TAG, "read failed: %s", esp_err_to_name(result));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (read && !slot->abandoned) s_jobs[index] = s_active;
        wipe(&s_active, sizeof(s_active));
        slot->result = result;
        slot->state = slot->abandoned ? SLOT_FREE : SLOT_DONE;
        xSemaphoreGive(s_lock);
    }
}

esp_err_t nvs_store_init(void)
{
    if (s_task != NULL) return ESP_OK;
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_queue = xQueueCreateStatic(NVS_STORE_SLOTS, sizeof(uint8_t), s_queue_bytes, &s_queue_storage);
    if (!s_queue) {
        vSemaphoreDelete(s_lock); s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_task = xTaskCreateStaticPinnedToCore(store_task, "nvs_store", STORE_STACK_BYTES,
        NULL, 1, s_stack, &s_task_storage, 0);
    if (!s_task) {
        vQueueDelete(s_queue); s_queue = NULL;
        vSemaphoreDelete(s_lock); s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready: internal stack=%u staging=%u, PSRAM pool=%u slots=%u",
             STORE_STACK_BYTES, (unsigned)sizeof(s_active), (unsigned)sizeof(s_jobs), NVS_STORE_SLOTS);
    return ESP_OK;
}

esp_err_t nvs_store_submit(const char *name, const nvs_store_op_t *ops,
                           size_t count, nvs_store_ticket_t *ticket)
{
    if (!ticket) return ESP_ERR_INVALID_ARG;
    *ticket = 0;
    esp_err_t err = validate(name, ops, count);
    if (err != ESP_OK) return err;
    if (!s_task) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    uint8_t index = 0;
    while (index < NVS_STORE_SLOTS && s_slots[index].state != SLOT_FREE) ++index;
    if (index == NVS_STORE_SLOTS) {
        xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM;
    }
    stored_job_t *job = &s_jobs[index];
    memset(job, 0, sizeof(*job));
    memcpy(job->namespace_name, name, strlen(name) + 1);
    job->count = count;
    if (ops[0].kind >= NVS_STORE_GET_U8) job->read_size = ops[0].size;
    size_t used = 0;
    for (unsigned i = 0; i < count; ++i) {
        job->ops[i].kind = ops[i].kind;
        if (ops[i].kind != NVS_STORE_ERASE_ALL)
            memcpy(job->ops[i].key, ops[i].key, strlen(ops[i].key) + 1);
        job->ops[i].offset = used;
        job->ops[i].size = ops[i].size;
        if (ops[i].size && ops[i].value) memcpy(job->data + used, ops[i].value, ops[i].size);
        used += ops[i].size;
    }
    /* 3 index bits, 29 generation bits. Old completions cannot free a new slot. */
    s_sequence = s_sequence % 0x1fffffffU + 1;
    slot_t *slot = &s_slots[index];
    *slot = (slot_t){.ticket = (s_sequence << 3) | index, .state = SLOT_QUEUED};
    if (xQueueSend(s_queue, &index, 0) != pdTRUE) {
        slot->state = SLOT_FREE;
        wipe(job, sizeof(*job));
        xSemaphoreGive(s_lock); return ESP_ERR_TIMEOUT;
    }
    *ticket = slot->ticket;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

static esp_err_t take_result(nvs_store_ticket_t ticket, esp_err_t *result,
                              void *value, size_t *size)
{
    if (!ticket || !result) return ESP_ERR_INVALID_ARG;
    if (!s_task) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) return ESP_ERR_NOT_FINISHED;
    slot_t *slot = &s_slots[ticket & 7U];
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (slot->ticket == ticket && slot->state != SLOT_FREE && !slot->abandoned) {
        err = ESP_ERR_NOT_FINISHED;
        if (slot->state == SLOT_DONE) {
            stored_job_t *job = &s_jobs[ticket & 7U];
            if (size) {
                if (!is_read(job)) {
                    xSemaphoreGive(s_lock); return ESP_ERR_INVALID_ARG;
                }
                size_t capacity = *size;
                *size = job->read_size;
                if (slot->result == ESP_OK) {
                    if (capacity < job->read_size) {
                        xSemaphoreGive(s_lock); return ESP_ERR_INVALID_SIZE;
                    }
                    memcpy(value, job->data, job->read_size);
                }
            }
            *result = slot->result;
            slot->state = SLOT_FREE;
            wipe(job, sizeof(*job));
            err = ESP_OK;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t nvs_store_take_result(nvs_store_ticket_t ticket, esp_err_t *result)
{
    return take_result(ticket, result, NULL, NULL);
}

esp_err_t nvs_store_take_read_result(nvs_store_ticket_t ticket, esp_err_t *result,
                                    void *value, size_t *size)
{
    if (!value || !size) return ESP_ERR_INVALID_ARG;
    return take_result(ticket, result, value, size);
}

esp_err_t nvs_store_read_async(const char *name, const char *key,
                              nvs_store_kind_t kind, size_t capacity,
                              nvs_store_ticket_t *ticket)
{
    if (kind < NVS_STORE_GET_U8 || kind > NVS_STORE_GET_BLOB) return ESP_ERR_INVALID_ARG;
    const nvs_store_op_t op = {kind, key, NULL, capacity};
    return nvs_store_submit(name, &op, 1, ticket);
}

esp_err_t nvs_store_abandon(nvs_store_ticket_t ticket)
{
    if (!ticket) return ESP_ERR_INVALID_ARG;
    if (!s_task) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY); /* bounded memory copies only under lock */
    slot_t *slot = &s_slots[ticket & 7U];
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (slot->ticket == ticket && slot->state != SLOT_FREE && !slot->abandoned) {
        slot->abandoned = true;
        if (slot->state == SLOT_DONE) {
            wipe(&s_jobs[ticket & 7U], sizeof(s_jobs[0]));
            slot->state = SLOT_FREE;
        }
        err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t nvs_store_execute(const char *name, const nvs_store_op_t *ops,
                            size_t count, uint32_t timeout_ms)
{
    if (s_task && xTaskGetCurrentTaskHandle() == s_task) return ESP_ERR_INVALID_STATE;
    nvs_store_ticket_t ticket = 0;
    esp_err_t err = nvs_store_submit(name, ops, count, &ticket);
    if (err != ESP_OK) return err;
    int64_t until_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        esp_err_t result = ESP_OK;
        err = nvs_store_take_result(ticket, &result);
        if (err == ESP_OK) return result;
        if (err != ESP_ERR_NOT_FINISHED) return err;
        if (esp_timer_get_time() >= until_us) {
            (void)nvs_store_abandon(ticket);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

esp_err_t nvs_store_read(const char *name, const char *key, nvs_store_kind_t kind,
                        void *value, size_t *size, uint32_t timeout_ms)
{
    if (!value || !size) return ESP_ERR_INVALID_ARG;
    if (s_task && xTaskGetCurrentTaskHandle() == s_task) return ESP_ERR_INVALID_STATE;
    nvs_store_ticket_t ticket = 0;
    esp_err_t err = nvs_store_read_async(name, key, kind, *size, &ticket);
    if (err != ESP_OK) return err;
    int64_t until_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        esp_err_t result = ESP_OK;
        err = nvs_store_take_read_result(ticket, &result, value, size);
        if (err == ESP_OK) return result;
        if (err != ESP_ERR_NOT_FINISHED) {
            (void)nvs_store_abandon(ticket);
            return err;
        }
        if (esp_timer_get_time() >= until_us) {
            (void)nvs_store_abandon(ticket);
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
