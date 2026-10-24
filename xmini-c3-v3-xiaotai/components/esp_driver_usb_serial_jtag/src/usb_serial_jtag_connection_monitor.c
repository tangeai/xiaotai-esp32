/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include "driver/usb_serial_jtag.h"
#include "hal/usb_serial_jtag_ll.h"
#include "esp_private/startup_internal.h"
#include "esp_freertos_hooks.h"
#include "esp_pm.h"
#include "esp_private/rtc_clk.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#if CONFIG_XMINI_USB_TIMING_PROBE
#include "esp_rom_sys.h"
#endif

static volatile bool s_usb_serial_jtag_conn_status;
#if CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION
static esp_pm_lock_handle_t s_usb_serial_jtag_pm_lock;
#endif

// FreeRTOS tick interrupt may get delayed in handling, which could lead to no SOF coming in within the period from
// a delayed tick to the following tick. Therefore, when FREERTOS_HZ is comparable to SOF packet frequency, a NO_SOF
// tolerance needs to be given to avoid a false disconnection detection.
#define USJ_DISCONNECT_CONFIRM_PERIOD_MS    3
#define ALLOWED_NO_SOF_TICKS                pdMS_TO_TICKS(USJ_DISCONNECT_CONFIRM_PERIOD_MS)
#define USJ_DISCONNECT_CONFIRM_PERIOD_US    (USJ_DISCONNECT_CONFIRM_PERIOD_MS * 1000U)

static uint32_t remaining_allowed_no_sof_ticks;
static uint32_t s_sof_history;
static volatile uint32_t s_last_disconnect_sof, s_disconnect_count;
static int64_t s_last_tick_us, s_last_sof_us;
static uint32_t s_missing_ticks;
static uint32_t s_tick_delta_us[USB_SERIAL_JTAG_SOF_DIAG_SAMPLES];
static uint16_t s_sof_frame_index[USB_SERIAL_JTAG_SOF_DIAG_SAMPLES];
static uint8_t s_sof_sample_flags[USB_SERIAL_JTAG_SOF_DIAG_SAMPLES];
static unsigned s_tick_delta_next;
static portMUX_TYPE s_diagnostic_lock = portMUX_INITIALIZER_UNLOCKED;
static usb_serial_jtag_sof_diagnostic_t s_last_diagnostic;

#if CONFIG_XMINI_USB_TIMING_PROBE
static bool s_tick_delay_probe_fired;

esp_err_t usb_serial_jtag_test_delay_tick_once(void)
{
    portENTER_CRITICAL(&s_diagnostic_lock);
    if (s_tick_delay_probe_fired || !s_usb_serial_jtag_conn_status) {
        portEXIT_CRITICAL(&s_diagnostic_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_tick_delay_probe_fired = true;
    esp_rom_delay_us(8000);
    portEXIT_CRITICAL(&s_diagnostic_lock);
    return ESP_OK;
}
#endif

uint32_t usb_serial_jtag_get_last_disconnect_sof(uint32_t *count)
{
    portENTER_CRITICAL(&s_diagnostic_lock);
    if (count) *count = s_disconnect_count;
    uint32_t result = s_last_disconnect_sof;
    portEXIT_CRITICAL(&s_diagnostic_lock);
    return result;
}

bool usb_serial_jtag_get_sof_diagnostic(usb_serial_jtag_sof_diagnostic_t *diagnostic)
{
    if (!diagnostic) return false;
    portENTER_CRITICAL(&s_diagnostic_lock);
    *diagnostic = s_last_diagnostic;
    bool valid = s_disconnect_count != 0;
    portEXIT_CRITICAL(&s_diagnostic_lock);
    return valid;
}

static __attribute__((unused)) const char *USB_SERIAL_JTAG_CONN_MONITOR_TAG = "usb_serial_jtag";

bool usb_serial_jtag_is_connected(void)
{
    return s_usb_serial_jtag_conn_status;
}

static void IRAM_ATTR usb_serial_jtag_sof_tick_hook(void)
{
    // SOF packet is sent by the HOST every 1ms on a full speed bus
    // SYSTIMER can invoke several tick hooks back-to-back after a delayed IRQ.
    // A pending SOF is consumed by the first hook; subsequent catch-up hooks
    // must not spend a millisecond of tolerance in only a few microseconds.
    // Here, the strategy is: Always assume USB Serial/JTAG is connected until we are sure it is not connected
    // Confirm only after the tick credit is spent AND at least 3 real ms have
    // elapsed since observing SOF. Keep the existing credit at lower tick rates.
    bool sof_received = (usb_serial_jtag_ll_get_intraw_mask() & USB_SERIAL_JTAG_INTR_SOF);
    uint16_t frame_index = 0;
#if CONFIG_IDF_TARGET_ESP32C3
    frame_index = USB_SERIAL_JTAG.fram_num.sof_frame_index;
#endif
    // A zero read owns no event to acknowledge. Unconditional W1C can erase
    // a hardware SOF that arrives between this read and the clear write.
    if (sof_received) {
        usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SOF);
    }
    bool raw_after = (usb_serial_jtag_ll_get_intraw_mask() & USB_SERIAL_JTAG_INTR_SOF);
    int64_t now = esp_timer_get_time();
    uint64_t delta = (uint64_t)(now - s_last_tick_us);
    s_tick_delta_us[s_tick_delta_next] = delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
    s_sof_frame_index[s_tick_delta_next] = frame_index;
    s_sof_sample_flags[s_tick_delta_next] =
        (sof_received ? USB_SERIAL_JTAG_SOF_SAMPLE_OBSERVED | USB_SERIAL_JTAG_SOF_SAMPLE_CLEARED : 0) |
        (raw_after ? USB_SERIAL_JTAG_SOF_SAMPLE_RAW_AFTER : 0);
    s_tick_delta_next = (s_tick_delta_next + 1) % USB_SERIAL_JTAG_SOF_DIAG_SAMPLES;
    s_last_tick_us = now;
    s_sof_history = (s_sof_history << 1) | (uint32_t)sof_received;
    // Replenish tolerance on every SOF: only consecutive missing ticks count.
    if (sof_received) {
        remaining_allowed_no_sof_ticks = ALLOWED_NO_SOF_TICKS;
        s_last_sof_us = now;
        s_missing_ticks = 0;
    } else if (s_missing_ticks != UINT32_MAX) {
        ++s_missing_ticks;
    }
    if (s_usb_serial_jtag_conn_status != sof_received) {
        if (!sof_received) {
            if (remaining_allowed_no_sof_ticks > 0) {
                remaining_allowed_no_sof_ticks--;
            } else if ((uint64_t)(now - s_last_sof_us) >= USJ_DISCONNECT_CONFIRM_PERIOD_US) {
                // Tick catch-up is not elapsed time. The host must have been
                // silent for the real confirmation interval before disconnect.
                portENTER_CRITICAL_ISR(&s_diagnostic_lock);
                s_last_disconnect_sof = s_sof_history;
                ++s_disconnect_count;
                s_last_diagnostic.timestamp_us = (uint64_t)now;
                s_last_diagnostic.disconnect_count = s_disconnect_count;
                s_last_diagnostic.sof_history = s_sof_history;
                uint64_t since_sof = (uint64_t)(now - s_last_sof_us);
                s_last_diagnostic.since_sof_us = since_sof > UINT32_MAX ? UINT32_MAX : (uint32_t)since_sof;
                s_last_diagnostic.missing_ticks = s_missing_ticks;
                for (unsigned i = 0; i < USB_SERIAL_JTAG_SOF_DIAG_SAMPLES; ++i) {
                    s_last_diagnostic.delta_us[i] = s_tick_delta_us[(s_tick_delta_next + i) % USB_SERIAL_JTAG_SOF_DIAG_SAMPLES];
                    s_last_diagnostic.frame_index[i] = s_sof_frame_index[(s_tick_delta_next + i) % USB_SERIAL_JTAG_SOF_DIAG_SAMPLES];
                    s_last_diagnostic.sample_flags[i] = s_sof_sample_flags[(s_tick_delta_next + i) % USB_SERIAL_JTAG_SOF_DIAG_SAMPLES];
                }
#if CONFIG_IDF_TARGET_ESP32C3
                s_last_diagnostic.frame_index_valid = 1;
#endif
                portEXIT_CRITICAL_ISR(&s_diagnostic_lock);
#if CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION
                esp_pm_lock_release(s_usb_serial_jtag_pm_lock);
#endif
#if USB_SERIAL_JTAG_LL_PHY_DEPENDS_ON_BBPLL
                rtc_clk_bbpll_remove_consumer();
#endif
                s_usb_serial_jtag_conn_status = false;
            }
        } else {
            // USJ re-connected
#if CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION
            esp_pm_lock_acquire(s_usb_serial_jtag_pm_lock);
#endif
#if USB_SERIAL_JTAG_LL_PHY_DEPENDS_ON_BBPLL
            rtc_clk_bbpll_add_consumer();
#endif
            s_usb_serial_jtag_conn_status = true;
            remaining_allowed_no_sof_ticks = ALLOWED_NO_SOF_TICKS;
        }
    }
}

ESP_SYSTEM_INIT_FN(usb_serial_jtag_conn_status_init, SECONDARY, BIT(0), 230)
{
#if CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION
    ESP_RETURN_ON_ERROR(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb_serial_jtag", &s_usb_serial_jtag_pm_lock),
                        USB_SERIAL_JTAG_CONN_MONITOR_TAG, "create NO_LIGHT_SLEEP lock failed");
    // We always assume it is connected at first, so acquires the lock to avoid auto light sleep
    esp_pm_lock_acquire(s_usb_serial_jtag_pm_lock);
#endif
#if USB_SERIAL_JTAG_LL_PHY_DEPENDS_ON_BBPLL
// TODO: esp32p4 USJ rely on SPLL, if it will also be disabled during sleep, we need to call spll_add_consumer? IDF-9947
    rtc_clk_bbpll_add_consumer();
#endif
    s_usb_serial_jtag_conn_status = true;
    remaining_allowed_no_sof_ticks = ALLOWED_NO_SOF_TICKS;
    s_last_tick_us = s_last_sof_us = esp_timer_get_time();

    return esp_register_freertos_tick_hook(usb_serial_jtag_sof_tick_hook);
}

void usb_serial_jtag_connection_monitor_include(void)
{
    // Linker hook function, exists to make the linker examine this file
}
