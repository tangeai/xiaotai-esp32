/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_lvgl_port.h"
#include "esp_lvgl_port_priv.h"
#include "lvgl.h"

static const char *TAG = "LVGL";

/*******************************************************************************
* Types definitions
*******************************************************************************/

typedef struct lvgl_port_ctx_s {
    TaskHandle_t        lvgl_task;
    SemaphoreHandle_t   lvgl_mux;
    esp_timer_handle_t  tick_timer;
    bool                running;
    int                 task_max_sleep_ms;
    int                 timer_period_ms;
} lvgl_port_ctx_t;

typedef struct {
    portMUX_TYPE mux;
    TaskHandle_t owner;
    const char *owner_name;
    uint32_t depth;
    int64_t since_us;
} lvgl_port_lock_trace_t;

/*******************************************************************************
* Local variables
*******************************************************************************/
static lvgl_port_ctx_t lvgl_port_ctx;
static lvgl_port_lock_trace_t lvgl_port_lock_trace = {
    .mux = portMUX_INITIALIZER_UNLOCKED,
};

/*******************************************************************************
* Function definitions
*******************************************************************************/
static void lvgl_port_task(void *arg);
static esp_err_t lvgl_port_tick_init(void);
static void lvgl_port_task_deinit(void);

/*******************************************************************************
* Public API functions
*******************************************************************************/

esp_err_t lvgl_port_init(const lvgl_port_cfg_t *cfg)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    ESP_GOTO_ON_FALSE(cfg->task_affinity < (configNUM_CORES), ESP_ERR_INVALID_ARG, err, TAG,
                      "Bad core number for task! Maximum core number is %d", (configNUM_CORES - 1));

    memset(&lvgl_port_ctx, 0, sizeof(lvgl_port_ctx));

    /* LVGL init */
    lv_init();
    /* Tick init */
    lvgl_port_ctx.timer_period_ms = cfg->timer_period_ms;
    ESP_RETURN_ON_ERROR(lvgl_port_tick_init(), TAG, "");
    /* Create task */
    lvgl_port_ctx.task_max_sleep_ms = cfg->task_max_sleep_ms;
    if (lvgl_port_ctx.task_max_sleep_ms == 0) {
        lvgl_port_ctx.task_max_sleep_ms = 500;
    }
    /* LVGL semaphore */
    lvgl_port_ctx.lvgl_mux = xSemaphoreCreateRecursiveMutex();
    ESP_GOTO_ON_FALSE(lvgl_port_ctx.lvgl_mux, ESP_ERR_NO_MEM, err, TAG, "Create LVGL mutex fail!");

    BaseType_t res;
    const uint32_t caps = cfg->task_stack_caps ? cfg->task_stack_caps : MALLOC_CAP_INTERNAL |
                          MALLOC_CAP_DEFAULT; // caps cannot be zero
    if (cfg->task_affinity < 0) {
        res = xTaskCreateWithCaps(lvgl_port_task, "taskLVGL", cfg->task_stack, NULL, cfg->task_priority,
                                  &lvgl_port_ctx.lvgl_task, caps);
    } else {
        res = xTaskCreatePinnedToCoreWithCaps(lvgl_port_task, "taskLVGL", cfg->task_stack, NULL, cfg->task_priority,
                                              &lvgl_port_ctx.lvgl_task, cfg->task_affinity, caps);
    }
    ESP_GOTO_ON_FALSE(res == pdPASS, ESP_FAIL, err, TAG, "Create LVGL task fail!");

err:
    if (ret != ESP_OK) {
        lvgl_port_deinit();
    }

    return ret;
}

esp_err_t lvgl_port_resume(void)
{
    esp_err_t ret = ESP_ERR_INVALID_STATE;

    if (lvgl_port_ctx.tick_timer != NULL) {
        lv_timer_enable(true);
        ret = esp_timer_start_periodic(lvgl_port_ctx.tick_timer, lvgl_port_ctx.timer_period_ms * 1000);
    }

    return ret;
}

esp_err_t lvgl_port_stop(void)
{
    esp_err_t ret = ESP_ERR_INVALID_STATE;

    if (lvgl_port_ctx.tick_timer != NULL) {
        lv_timer_enable(false);
        ret = esp_timer_stop(lvgl_port_ctx.tick_timer);
    }

    return ret;
}

esp_err_t lvgl_port_deinit(void)
{
    /* Stop running task */
    if (lvgl_port_ctx.running) {
        lvgl_port_ctx.running = false;
    }

    return ESP_OK;
}

bool lvgl_port_lock(uint32_t timeout_ms)
{
    assert(lvgl_port_ctx.lvgl_mux && "lvgl_port_init must be called first");

    const TickType_t timeout_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTakeRecursive(lvgl_port_ctx.lvgl_mux, timeout_ticks) == pdTRUE) {
        TaskHandle_t current = xTaskGetCurrentTaskHandle();
        taskENTER_CRITICAL(&lvgl_port_lock_trace.mux);
        if (lvgl_port_lock_trace.owner == current) {
            lvgl_port_lock_trace.depth++;
        } else {
            lvgl_port_lock_trace.owner = current;
            lvgl_port_lock_trace.owner_name = pcTaskGetName(current);
            lvgl_port_lock_trace.depth = 1;
            lvgl_port_lock_trace.since_us = esp_timer_get_time();
        }
        taskEXIT_CRITICAL(&lvgl_port_lock_trace.mux);
        return true;
    }

    const char *owner_name = "none";
    uint32_t owner_depth = 0;
    int64_t held_ms = 0;
    taskENTER_CRITICAL(&lvgl_port_lock_trace.mux);
    if (lvgl_port_lock_trace.owner_name != NULL) {
        owner_name = lvgl_port_lock_trace.owner_name;
        owner_depth = lvgl_port_lock_trace.depth;
        held_ms = (esp_timer_get_time() - lvgl_port_lock_trace.since_us) / 1000;
    }
    taskEXIT_CRITICAL(&lvgl_port_lock_trace.mux);
    ESP_LOGE(TAG,
             "LVGL mutex lock timeout: waiter=%s timeout=%ums owner=%s depth=%lu held=%lldms",
             pcTaskGetName(NULL),
             (unsigned)timeout_ms,
             owner_name,
             (unsigned long)owner_depth,
             (long long)held_ms);
    return false;
}

void lvgl_port_unlock(void)
{
    assert(lvgl_port_ctx.lvgl_mux && "lvgl_port_init must be called first");
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    taskENTER_CRITICAL(&lvgl_port_lock_trace.mux);
    if (lvgl_port_lock_trace.owner == current && lvgl_port_lock_trace.depth > 0) {
        lvgl_port_lock_trace.depth--;
        if (lvgl_port_lock_trace.depth == 0) {
            lvgl_port_lock_trace.owner = NULL;
            lvgl_port_lock_trace.owner_name = NULL;
            lvgl_port_lock_trace.since_us = 0;
        }
    }
    taskEXIT_CRITICAL(&lvgl_port_lock_trace.mux);
    xSemaphoreGiveRecursive(lvgl_port_ctx.lvgl_mux);
}

esp_err_t lvgl_port_task_wake(lvgl_port_event_type_t event, void *param)
{
    ESP_LOGE(TAG, "Task wake is not supported, when used LVGL8!");
    return ESP_ERR_NOT_SUPPORTED;
}

IRAM_ATTR bool lvgl_port_task_notify(uint32_t value)
{
    BaseType_t need_yield = pdFALSE;

    // Notify LVGL task
    if (xPortInIsrContext() == pdTRUE) {
        xTaskNotifyFromISR(lvgl_port_ctx.lvgl_task, value, eNoAction, &need_yield);
    } else {
        xTaskNotify(lvgl_port_ctx.lvgl_task, value, eNoAction);
    }

    return (need_yield == pdTRUE);
}

/*******************************************************************************
* Private functions
*******************************************************************************/

static void lvgl_port_task(void *arg)
{
    uint32_t task_delay_ms = lvgl_port_ctx.task_max_sleep_ms;
    int64_t health_at_us = esp_timer_get_time();
    uint32_t max_lock_us = 0;
    uint32_t max_handler_us = 0;
    uint32_t slow_handlers = 0;

    ESP_LOGI(TAG, "Starting LVGL task");
    lvgl_port_ctx.running = true;
    while (lvgl_port_ctx.running) {
        int64_t lock_at_us = esp_timer_get_time();
        bool locked = lvgl_port_lock(1000);
        uint32_t lock_us = (uint32_t)(esp_timer_get_time() - lock_at_us);
        if (lock_us > max_lock_us) max_lock_us = lock_us;
        if (locked) {
            int64_t handler_at_us = esp_timer_get_time();
            task_delay_ms = lv_timer_handler();
            uint32_t handler_us = (uint32_t)(esp_timer_get_time() - handler_at_us);
            if (handler_us > max_handler_us) max_handler_us = handler_us;
            if (handler_us > 45000U) slow_handlers++;
            lvgl_port_unlock();
        }
        int64_t now_us = esp_timer_get_time();
        if (now_us - health_at_us >= 10000000LL) {
            if (max_lock_us > 45000U || max_handler_us > 45000U) {
                ESP_LOGW(TAG, "UIH lock_max=%luus handler_max=%luus slow=%lu",
                         (unsigned long)max_lock_us,
                         (unsigned long)max_handler_us,
                         (unsigned long)slow_handlers);
            }
            health_at_us = now_us;
            max_lock_us = 0;
            max_handler_us = 0;
            slow_handlers = 0;
        }
        if (task_delay_ms > lvgl_port_ctx.task_max_sleep_ms) {
            task_delay_ms = lvgl_port_ctx.task_max_sleep_ms;
        } else if (task_delay_ms < 5) {
            task_delay_ms = 5;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }

    ESP_LOGI(TAG, "Stopped LVGL task");

    lvgl_port_task_deinit();

    /* Close task */
    vTaskDelete( NULL );
}

static void lvgl_port_task_deinit(void)
{
    /* Stop and delete timer */
    if (lvgl_port_ctx.tick_timer != NULL) {
        esp_timer_stop(lvgl_port_ctx.tick_timer);
        esp_timer_delete(lvgl_port_ctx.tick_timer);
        lvgl_port_ctx.tick_timer = NULL;
    }

    if (lvgl_port_ctx.lvgl_mux) {
        vSemaphoreDelete(lvgl_port_ctx.lvgl_mux);
    }
    memset(&lvgl_port_ctx, 0, sizeof(lvgl_port_ctx));
#if LV_ENABLE_GC || !LV_MEM_CUSTOM
    /* Deinitialize LVGL */
    lv_deinit();
#endif
}

static void lvgl_port_tick_increment(void *arg)
{
    /* Tell LVGL how many milliseconds have elapsed */
    lv_tick_inc(lvgl_port_ctx.timer_period_ms);
}

static esp_err_t lvgl_port_tick_init(void)
{
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &lvgl_port_tick_increment,
        .name = "LVGL tick",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&lvgl_tick_timer_args, &lvgl_port_ctx.tick_timer), TAG,
                        "Creating LVGL timer filed!");
    return esp_timer_start_periodic(lvgl_port_ctx.tick_timer, lvgl_port_ctx.timer_period_ms * 1000);
}
