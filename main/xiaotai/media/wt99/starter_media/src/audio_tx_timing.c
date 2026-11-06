#include "audio_tx_timing.h"
#if CONFIG_APP_AUDIO_TX_TIMING_DIAGNOSTICS
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_attr.h"
#include "esp_freertos_hooks.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/opt.h"

#define TX_TIMING_CAPACITY 1024U
EXT_RAM_BSS_ATTR static audio_tx_timing_record_t s_records[TX_TIMING_CAPACITY];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_written, s_fault_us;
static bool s_armed, s_fault;
static atomic_uintptr_t s_worker;
static atomic_uint s_worker_ticks[portNUM_PROCESSORS];
static atomic_uintptr_t s_spi_worker, s_tcpip_worker;
static atomic_uint s_spi_ticks[portNUM_PROCESSORS], s_tcpip_ticks[portNUM_PROCESSORS];
static atomic_int s_tick_error = ESP_ERR_INVALID_STATE;

/* Sample the interrupted task only. No TCB layout, trace ABI or PSRAM in ISR. */
static void IRAM_ATTR tx_tick(void)
{
    uintptr_t task = (uintptr_t)xTaskGetCurrentTaskHandle();
    unsigned core = xPortGetCoreID();
    if (task == atomic_load_explicit(&s_worker, memory_order_relaxed))
        atomic_fetch_add_explicit(&s_worker_ticks[core], 1, memory_order_relaxed);
    if (task == atomic_load_explicit(&s_spi_worker, memory_order_relaxed))
        atomic_fetch_add_explicit(&s_spi_ticks[core], 1, memory_order_relaxed);
    if (task == atomic_load_explicit(&s_tcpip_worker, memory_order_relaxed))
        atomic_fetch_add_explicit(&s_tcpip_ticks[core], 1, memory_order_relaxed);
}

esp_err_t audio_tx_timing_worker_started(void)
{
    atomic_store(&s_worker, (uintptr_t)xTaskGetCurrentTaskHandle());
    esp_err_t err = ESP_OK;
    for (unsigned core = 0; core < portNUM_PROCESSORS && err == ESP_OK; ++core)
        err = esp_register_freertos_tick_hook_for_cpu(tx_tick, core);
    if (err != ESP_OK) esp_deregister_freertos_tick_hook(tx_tick);
    atomic_store(&s_tick_error, err);
    return err;
}

uint32_t audio_tx_timing_ticks(void)
{
    uint32_t ticks = 0;
    for (unsigned core = 0; core < portNUM_PROCESSORS; ++core)
        ticks += atomic_load_explicit(&s_worker_ticks[core], memory_order_relaxed);
    return ticks;
}

void audio_tx_timing_network_ticks(unsigned core, uint32_t *spi, uint32_t *tcpip)
{
    *spi = atomic_load_explicit(&s_spi_ticks[core], memory_order_relaxed);
    *tcpip = atomic_load_explicit(&s_tcpip_ticks[core], memory_order_relaxed);
}

esp_err_t audio_tx_timing_start(void)
{
    if (atomic_load(&s_tick_error) != ESP_OK) return atomic_load(&s_tick_error);
    atomic_store(&s_spi_worker, (uintptr_t)xTaskGetHandle("eh_spi_rx"));
    atomic_store(&s_tcpip_worker, (uintptr_t)xTaskGetHandle(TCPIP_THREAD_NAME));
    portENTER_CRITICAL(&s_lock);
    s_written = s_fault_us = 0;
    s_fault = false;
    s_armed = true;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t audio_tx_timing_stop(void)
{
    portENTER_CRITICAL(&s_lock);
    s_armed = false;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void audio_tx_timing_record(const audio_tx_timing_record_t *record)
{
    uint32_t now = (uint32_t)esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (s_armed && s_fault && now - s_fault_us > 3000000U) s_armed = false;
    if (s_armed) {
        s_records[s_written++ % TX_TIMING_CAPACITY] = *record;
        if (record->kind && !s_fault) { s_fault = true; s_fault_us = now; }
    }
    portEXIT_CRITICAL(&s_lock);
}

void audio_tx_timing_drop(uint32_t generation, uint32_t source_ms)
{
    const audio_tx_timing_record_t record = {
        .generation = generation, .source_ms = source_ms,
        .begin_us = (uint32_t)esp_timer_get_time(), .kind = 1,
    };
    audio_tx_timing_record(&record);
}

esp_err_t audio_tx_timing_dump(void)
{
    portENTER_CRITICAL(&s_lock);
    bool armed = s_armed;
    portEXIT_CRITICAL(&s_lock);
    if (armed) return ESP_ERR_INVALID_STATE;
    uint32_t count = s_written < TX_TIMING_CAPACITY ? s_written : TX_TIMING_CAPACITY;
    uint32_t first = s_written - count;
    printf("+TXTRACE:BEGIN,count=%lu,bytes=%u,fault=%u,fault_us=%lu,spi_found=%u,tcpip_found=%u,hz=%u\r\n",
           (unsigned long)count, (unsigned)sizeof(s_records), (unsigned)s_fault,
           (unsigned long)s_fault_us, atomic_load(&s_spi_worker) != 0,
           atomic_load(&s_tcpip_worker) != 0, (unsigned)configTICK_RATE_HZ);
    puts("+TXTRACE:columns=generation,source_ms,queued_ms,begin_us,gain_us,pre_encode_us,encode_us,pre_send_us,send_us,total_us,pending,core,kind,gain_ticks,encode_ticks,send_ticks,total_ticks,encode_spi_ticks,encode_tcpip_ticks");
    for (uint32_t i = 0; i < count; ++i) {
        const audio_tx_timing_record_t *r = &s_records[(first + i) % TX_TIMING_CAPACITY];
        printf("+TXTRACE:%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\r\n",
               (unsigned long)r->generation, (unsigned long)r->source_ms,
               (unsigned long)r->queued_ms, (unsigned long)r->begin_us,
               (unsigned long)r->gain_us, (unsigned long)r->pre_encode_us,
               (unsigned long)r->encode_us, (unsigned long)r->pre_send_us,
               (unsigned long)r->send_us, (unsigned long)r->total_us,
               (unsigned long)r->pending, (unsigned long)r->core, (unsigned long)r->kind,
               (unsigned long)r->gain_ticks, (unsigned long)r->encode_ticks,
               (unsigned long)r->send_ticks, (unsigned long)r->total_ticks,
               (unsigned long)r->encode_spi_ticks, (unsigned long)r->encode_tcpip_ticks);
    }
    puts("+TXTRACE:END");
    return ESP_OK;
}
#else
esp_err_t audio_tx_timing_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_tx_timing_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t audio_tx_timing_dump(void) { return ESP_ERR_NOT_SUPPORTED; }
#endif
