#include "nvs_worker.h"

#include <stdbool.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define NVS_WORKER_STACK_BYTES 4096U
#define NVS_WORKER_SUBMIT_WAIT_MS 5U

typedef enum { SLOT_FREE, SLOT_QUEUED, SLOT_RUNNING, SLOT_DONE } slot_state_t;
typedef struct {
    nvs_worker_fn_t function;
    size_t size;
    esp_err_t result;
    slot_state_t state;
    bool asynchronous, abandoned;
    _Alignas(max_align_t) uint8_t data[NVS_WORKER_DATA_BYTES];
} nvs_job_t;

static const char *TAG = "nvs_worker";
static EXT_RAM_BSS_ATTR nvs_job_t s_jobs[NVS_WORKER_SLOTS];
static EXT_RAM_BSS_ATTR nvs_worker_status_t s_status;
static TaskHandle_t s_task;
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_mutex;
static EventGroupHandle_t s_done;
static DRAM_ATTR StaticTask_t s_tcb;
static DRAM_ATTR StackType_t s_stack[NVS_WORKER_STACK_BYTES / sizeof(StackType_t)];
static DRAM_ATTR StaticQueue_t s_queue_control;
static DRAM_ATTR uint8_t s_queue_storage[NVS_WORKER_SLOTS * sizeof(uint8_t)];
static DRAM_ATTR StaticSemaphore_t s_mutex_control;
static DRAM_ATTR StaticEventGroup_t s_done_control;

/* All slot transitions are serialized by s_mutex. A running callback owns its
 * data exclusively, with the mutex released throughout the Flash operation. */
static void release_job(nvs_job_t *job)
{
    memset(job, 0, sizeof(*job));
    --s_status.in_use;
}

static void worker_task(void *argument)
{
    (void)argument;
    for (;;) {
        uint8_t index;
        if (xQueueReceive(s_queue, &index, portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        nvs_job_t *job = &s_jobs[index];
        if (job->abandoned) {
            release_job(job);
            xSemaphoreGive(s_mutex);
            continue;
        }
        job->state = SLOT_RUNNING;
        xSemaphoreGive(s_mutex);

        esp_err_t result = job->function(job->data, job->size);

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        ++s_status.completed;
        bool report_error = job->asynchronous && result != ESP_OK;
        if (result != ESP_OK) ++s_status.failures;
        if (job->asynchronous || job->abandoned) {
            release_job(job);
        } else {
            job->result = result;
            job->state = SLOT_DONE;
            xEventGroupSetBits(s_done, (EventBits_t)1U << index);
        }
        xSemaphoreGive(s_mutex);
        if (report_error) ESP_LOGE(TAG, "async save failed: %s", esp_err_to_name(result));
    }
}

esp_err_t nvs_worker_init(void)
{
    if (s_task != NULL) return ESP_OK;
    /* Initialization belongs to app_main; no lazy allocation from UI/SDK callbacks. */
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_control);
    s_done = xEventGroupCreateStatic(&s_done_control);
    s_queue = xQueueCreateStatic(NVS_WORKER_SLOTS, sizeof(uint8_t),
                                 s_queue_storage, &s_queue_control);
    if (s_mutex != NULL && s_done != NULL && s_queue != NULL) {
        s_task = xTaskCreateStaticPinnedToCore(worker_task, "nvs_worker",
                    NVS_WORKER_STACK_BYTES, NULL, 1, s_stack, &s_tcb, 0);
        if (s_task != NULL) return ESP_OK;
    }
    if (s_queue != NULL) vQueueDelete(s_queue);
    if (s_done != NULL) vEventGroupDelete(s_done);
    if (s_mutex != NULL) vSemaphoreDelete(s_mutex);
    s_queue = NULL;
    s_done = NULL;
    s_mutex = NULL;
    return ESP_ERR_NO_MEM;
}

static esp_err_t submit(nvs_worker_fn_t function, const void *data, size_t size,
                         bool asynchronous, uint8_t *submitted_index)
{
    if (function == NULL || (size != 0 && data == NULL)) return ESP_ERR_INVALID_ARG;
    if (size > NVS_WORKER_DATA_BYTES) return ESP_ERR_INVALID_SIZE;
    if (s_task == NULL || xPortInIsrContext() || xTaskGetCurrentTaskHandle() == s_task)
        return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(NVS_WORKER_SUBMIT_WAIT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (asynchronous) {
        for (unsigned i = 0; i < NVS_WORKER_SLOTS; ++i) {
            nvs_job_t *job = &s_jobs[i];
            if (job->state == SLOT_QUEUED && job->asynchronous && job->function == function) {
                memset(job->data, 0, sizeof(job->data));
                if (size != 0) memcpy(job->data, data, size);
                job->size = size;
                ++s_status.coalesced;
                xSemaphoreGive(s_mutex);
                return ESP_OK;
            }
        }
    }
    for (uint8_t i = 0; i < NVS_WORKER_SLOTS; ++i) {
        nvs_job_t *job = &s_jobs[i];
        if (job->state != SLOT_FREE) continue;
        job->function = function;
        job->size = size;
        job->asynchronous = asynchronous;
        if (size != 0) memcpy(job->data, data, size);
        job->state = SLOT_QUEUED;
        xEventGroupClearBits(s_done, (EventBits_t)1U << i);
        if (xQueueSend(s_queue, &i, 0) != pdPASS) {
            memset(job, 0, sizeof(*job));
            ++s_status.full;
            xSemaphoreGive(s_mutex);
            return ESP_ERR_NO_MEM;
        }
        ++s_status.submitted;
        ++s_status.in_use;
        if (submitted_index != NULL) *submitted_index = i;
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    ++s_status.full;
    xSemaphoreGive(s_mutex);
    return ESP_ERR_NO_MEM;
}

esp_err_t nvs_worker_call(nvs_worker_fn_t function, void *data, size_t size,
                          uint32_t timeout_ms)
{
    if (timeout_ms == 0 || timeout_ms > NVS_WORKER_WAIT_MS) return ESP_ERR_INVALID_ARG;
    uint8_t index;
    esp_err_t result = submit(function, data, size, false, &index);
    if (result != ESP_OK) return result;
    (void)xEventGroupWaitBits(s_done, (EventBits_t)1U << index, pdTRUE, pdTRUE,
                              pdMS_TO_TICKS(timeout_ms));
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_job_t *job = &s_jobs[index];
    if (job->state == SLOT_DONE) {
        /* Completion wins a timeout race while this slot is still owned by us. */
        result = job->result;
        if (size != 0) memcpy(data, job->data, size);
        release_job(job);
    } else {
        /* Never free/reuse data that the queue or flash writer still owns. */
        job->abandoned = true;
        ++s_status.timeouts;
        result = ESP_ERR_TIMEOUT;
    }
    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t nvs_worker_submit_latest(nvs_worker_fn_t function, const void *data, size_t size)
{
    return submit(function, data, size, true, NULL);
}

esp_err_t nvs_worker_get_status(nvs_worker_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    if (s_task == NULL || xPortInIsrContext()) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(NVS_WORKER_SUBMIT_WAIT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    *status = s_status;
    xSemaphoreGive(s_mutex);
    status->stack_free_min = uxTaskGetStackHighWaterMark(s_task);
    return ESP_OK;
}
