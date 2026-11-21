// SPDX-License-Identifier: MIT
/* Port diagnostics on ESP-IDF. The task list needs FreeRTOS's trace facility
 * (CONFIG_FREERTOS_USE_TRACE_FACILITY); without it the call reports
 * XIAOTAI_ERR_NOT_SUPPORTED. Chip-level facts are only read when present in
 * the selected build, without reconfiguring counters or the USB controller. */
#include "xiaotai/port/sys.h"

#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/port_config.h"
#include "port_report.h"

#if defined(XIAOTAI_PORT_NATIVE_STARTUP) && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG && XIAOTAI_PORT_USB_MONITOR_FIX
#include "usb_monitor/xiaotai_usb_diagnostics.h"
#define PORT_HAS_USB_EVIDENCE 1
#else
#define PORT_HAS_USB_EVIDENCE 0
#endif

#if defined(XIAOTAI_PORT_NATIVE_STARTUP) && CONFIG_IDF_TARGET_ESP32S3
#include "esp_timer.h"
#include "soc/extmem_reg.h"
#include "soc/soc.h"
#define PORT_HAS_CACHE_COUNTERS 1
#else
#define PORT_HAS_CACHE_COUNTERS 0
#endif

/* Tasks that may be created between counting and walking the list. */
#define TASK_LIST_SLACK 4U

void xiaotai_port_report(xiaotai_port_write_fn_t write, void *ctx)
{
    if (!write) return;
    (void)ctx;
#if PORT_HAS_USB_EVIDENCE
    usb_serial_jtag_sof_diagnostic_t usb;
    if (usb_serial_jtag_get_sof_diagnostic(&usb)) {
        port_report_usb(write, ctx, usb.timestamp_us, usb.disconnect_count, usb.sof_history,
                        usb.since_sof_us, usb.missing_ticks);
        port_report_series(write, ctx, "usb_delta_us", usb.delta_us, USB_SERIAL_JTAG_SOF_DIAG_SAMPLES);
        uint32_t samples[USB_SERIAL_JTAG_SOF_DIAG_SAMPLES];
        if (usb.frame_index_valid) {
            for (size_t i = 0; i < USB_SERIAL_JTAG_SOF_DIAG_SAMPLES; ++i) samples[i] = usb.frame_index[i];
            port_report_series(write, ctx, "usb_frame_index", samples, USB_SERIAL_JTAG_SOF_DIAG_SAMPLES);
        }
        for (size_t i = 0; i < USB_SERIAL_JTAG_SOF_DIAG_SAMPLES; ++i) samples[i] = usb.sample_flags[i];
        port_report_series(write, ctx, "usb_sample_flags", samples, USB_SERIAL_JTAG_SOF_DIAG_SAMPLES);
    }
#endif
#if PORT_HAS_CACHE_COUNTERS
    /* Shared hardware counters are read sequentially; this is not an atomic snapshot. */
    port_cache_report_t cache;
    cache.timestamp_us = (uint64_t)esp_timer_get_time();
    cache.data = REG_READ(EXTMEM_DBUS_ACS_CNT_REG);
    cache.psram_miss = REG_READ(EXTMEM_DBUS_ACS_SPIRAM_MISS_CNT_REG);
    cache.flash_miss = REG_READ(EXTMEM_DBUS_ACS_FLASH_MISS_CNT_REG);
    cache.instruction = REG_READ(EXTMEM_IBUS_ACS_CNT_REG);
    cache.instruction_miss = REG_READ(EXTMEM_IBUS_ACS_MISS_CNT_REG);
    cache.flags = REG_READ(EXTMEM_CACHE_ILG_INT_ST_REG);
    cache.data_start = REG_READ(EXTMEM_DBUS_TO_FLASH_START_VADDR_REG);
    cache.data_end = REG_READ(EXTMEM_DBUS_TO_FLASH_END_VADDR_REG);
    cache.instruction_start = REG_READ(EXTMEM_IBUS_TO_FLASH_START_VADDR_REG);
    cache.instruction_end = REG_READ(EXTMEM_IBUS_TO_FLASH_END_VADDR_REG);
    cache.read_us = (uint64_t)esp_timer_get_time() - cache.timestamp_us;
    port_report_cache(write, ctx, &cache);
#endif
}

xiaotai_err_t xiaotai_port_probe(const char *name, xiaotai_port_write_fn_t write, void *ctx)
{
#if PORT_HAS_USB_EVIDENCE && XIAOTAI_PORT_USB_TIMING_PROBE
    if (name && strcmp(name, "usb_tick") == 0) {
        xiaotai_err_t result = usb_serial_jtag_test_delay_tick_once();
        if (result == XIAOTAI_OK && write) write(ctx, "usb_tick_stall_us=8000");
        return result;
    }
#else
    (void)name;
    (void)write;
    (void)ctx;
#endif
    return XIAOTAI_ERR_NOT_FOUND;
}

const char *const *xiaotai_port_probes(void)
{
#if PORT_HAS_USB_EVIDENCE && XIAOTAI_PORT_USB_TIMING_PROBE
    static const char *const probes[] = {"usb_tick", NULL};
    return probes;
#else
    static const char *const none[] = {NULL};
    return none;
#endif
}

xiaotai_err_t xiaotai_task_list(xiaotai_task_info_t *tasks, size_t capacity, size_t *count)
{
    if (!count || (capacity && !tasks)) return XIAOTAI_ERR_INVALID_ARG;
    *count = 0;
#if configUSE_TRACE_FACILITY
    /* The walk writes one record per task with the scheduler suspended; the
     * array may live in external RAM because the cache stays on. */
    const UBaseType_t room = uxTaskGetNumberOfTasks() + TASK_LIST_SLACK;
    TaskStatus_t *status = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, room, sizeof(*status));
    if (!status) return XIAOTAI_ERR_NO_MEM;
    const UBaseType_t found = uxTaskGetSystemState(status, room, NULL);
    if (found == 0) {
        /* More tasks than room appeared meanwhile. */
        xiaotai_mem_free(status);
        return XIAOTAI_FAIL;
    }
    for (UBaseType_t i = 0; i < found && i < capacity; ++i) {
        xiaotai_task_info_t *info = &tasks[i];
        memset(info, 0, sizeof(*info));
        if (status[i].pcTaskName) strncpy(info->name, status[i].pcTaskName, sizeof(info->name) - 1);
        /* ESP-IDF counts stacks in bytes (StackType_t is one byte wide). */
        info->stack_min_free = (uint32_t)status[i].usStackHighWaterMark * (uint32_t)sizeof(StackType_t);
        info->priority = (unsigned)status[i].uxCurrentPriority;
#if defined(configTASKLIST_INCLUDE_COREID) && configTASKLIST_INCLUDE_COREID
        info->core = status[i].xCoreID == tskNO_AFFINITY ? XIAOTAI_CORE_ANY : (int)status[i].xCoreID;
#else
        info->core = XIAOTAI_CORE_ANY;
#endif
    }
    *count = (size_t)found;
    xiaotai_mem_free(status);
    return XIAOTAI_OK;
#else
    return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
}
