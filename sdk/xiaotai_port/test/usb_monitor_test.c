// SPDX-License-Identifier: MIT
// Replaces legacy: tests/test_usb_connection_monitor.py
// Replaces legacy: tests/xmini/test_usb_monitor.c::usb_monitor
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "driver/usb_serial_jtag.h"
#include "hal/usb_serial_jtag_ll.h"
#include "sdkconfig.h"

static uint32_t raw;
static int64_t clock_us;
static void (*tick_hook)(void);
static bool sof_arrives_after_read;
static unsigned raw_reads_this_tick;
#if USB_SERIAL_JTAG_LL_PHY_DEPENDS_ON_BBPLL
static unsigned s_bbpll;
void rtc_clk_bbpll_add_consumer(void) { assert(s_bbpll == 0); ++s_bbpll; }
void rtc_clk_bbpll_remove_consumer(void) { assert(s_bbpll == 1); --s_bbpll; }
#endif
volatile usb_serial_jtag_test_dev_t USB_SERIAL_JTAG;
int64_t esp_timer_get_time(void) { return clock_us; }
void esp_rom_delay_us(uint32_t delay_us) { assert(delay_us == 8000); clock_us += delay_us; }
uint32_t usb_serial_jtag_ll_get_intraw_mask(void)
{
    uint32_t observed = raw;
    if (sof_arrives_after_read && raw_reads_this_tick == 0) {
        raw |= USB_SERIAL_JTAG_INTR_SOF;
        USB_SERIAL_JTAG.fram_num.sof_frame_index = (USB_SERIAL_JTAG.fram_num.sof_frame_index + 1) & 2047U;
    }
    ++raw_reads_this_tick;
    return observed;
}
void usb_serial_jtag_ll_clr_intsts_mask(uint32_t mask) { raw &= ~mask; }
int esp_register_freertos_tick_hook(void (*hook)(void)) { tick_hook = hook; return 0; }

#ifndef USB_MONITOR_SOURCE
#define USB_MONITOR_SOURCE "../esp-idf/usb_monitor/usb_connection_monitor.c"
#endif
#include USB_MONITOR_SOURCE

static void tick_after(bool sof, unsigned delta_us)
{
    clock_us += delta_us;
    raw = sof ? USB_SERIAL_JTAG_INTR_SOF : 0;
    if (sof) USB_SERIAL_JTAG.fram_num.sof_frame_index = (USB_SERIAL_JTAG.fram_num.sof_frame_index + 1) & 2047U;
    raw_reads_this_tick = 0;
    tick_hook();
#if USB_SERIAL_JTAG_LL_PHY_DEPENDS_ON_BBPLL
    assert(s_bbpll == (usb_serial_jtag_is_connected() ? 1U : 0U));
#endif
}

static void tick(bool sof) { tick_after(sof, 1000); }

int main(void)
{
    assert(usb_serial_jtag_conn_status_init() == 0 && tick_hook);
    if (TEST_USB_TICK_HZ < 1000U) {
        const unsigned period_us = 1000000U / TEST_USB_TICK_HZ;
        /* At a low tick rate pdMS_TO_TICKS(3) is zero; actual silence
         * still disconnects and reconnect must re-acquire the S3 clock. */
        tick_after(true, period_us);
        assert(usb_serial_jtag_is_connected());
        tick_after(false, 2999);
        assert(usb_serial_jtag_is_connected());
        tick_after(false, 1);
        assert(!usb_serial_jtag_is_connected());
        for (unsigned i = 0; i < 20; ++i) {
            tick_after(false, period_us);
            assert(!usb_serial_jtag_is_connected());
        }
        tick_after(true, period_us);
        assert(usb_serial_jtag_is_connected());
        tick_after(false, period_us);
        assert(!usb_serial_jtag_is_connected());
        return 0;
    }
#ifndef USB_MONITOR_LEGACY_ONLY
    usb_serial_jtag_sof_diagnostic_t diagnostic;
    assert(!usb_serial_jtag_get_sof_diagnostic(NULL));
    assert(!usb_serial_jtag_get_sof_diagnostic(&diagnostic));
#endif
    for (unsigned i = 0; i < 100; ++i) {
        tick(false);
        assert(usb_serial_jtag_is_connected());
        tick(true);
        assert(usb_serial_jtag_is_connected());
    }
    for (unsigned i = 0; i < 3; ++i) {
        tick(false);
        assert(usb_serial_jtag_is_connected());
    }
    tick(false);
    assert(!usb_serial_jtag_is_connected());
#if !defined(USB_MONITOR_LEGACY_ONLY) && CONFIG_XIAOTAI_PORT_USB_TIMING_PROBE
    assert(usb_serial_jtag_test_delay_tick_once() == ESP_ERR_INVALID_STATE);
#endif
#ifndef USB_MONITOR_LEGACY_ONLY
    assert(usb_serial_jtag_get_sof_diagnostic(&diagnostic));
    assert(diagnostic.disconnect_count == 1 && diagnostic.since_sof_us == 4000);
    assert(diagnostic.missing_ticks == 4 && (diagnostic.sof_history & 15) == 0);
    assert(diagnostic.frame_index_valid == 1);
    for (unsigned i = 0; i < USB_SERIAL_JTAG_SOF_DIAG_SAMPLES; ++i) {
        assert(diagnostic.delta_us[i] == 1000);
    }
    for (unsigned i = USB_SERIAL_JTAG_SOF_DIAG_SAMPLES - 4; i < USB_SERIAL_JTAG_SOF_DIAG_SAMPLES; ++i) {
        assert(diagnostic.sample_flags[i] == 0);
        assert(diagnostic.frame_index[i] == USB_SERIAL_JTAG.fram_num.sof_frame_index);
    }
    uint64_t first_timestamp = diagnostic.timestamp_us;
#endif
    for (unsigned i = 0; i < 10; ++i) { tick(false); assert(!usb_serial_jtag_is_connected()); }
    tick(true);
    assert(usb_serial_jtag_is_connected());
#ifndef USB_MONITOR_LEGACY_ONLY
    assert(usb_serial_jtag_get_sof_diagnostic(&diagnostic));
    assert(diagnostic.timestamp_us == first_timestamp);
#endif
    tick(false);
    assert(usb_serial_jtag_is_connected());
#ifndef USB_MONITOR_LEGACY_ONLY
    // Replay the measured COM8 trace: 8711 us to the delayed IRQ, then four
    // catch-up hooks only 4 us apart. The prior four-tick decision disconnects.
    unsigned previous_credit = 3;
    bool previous_connected = true;
    tick_after(true, 8711);
    for (unsigned i = 0; i < 4; ++i) {
        if (previous_credit) --previous_credit;
        else previous_connected = false;
        tick_after(false, 4);
        assert(usb_serial_jtag_is_connected());
    }
    assert(!previous_connected);
    assert(usb_serial_jtag_get_sof_diagnostic(&diagnostic));
    assert(diagnostic.disconnect_count == 1 && diagnostic.timestamp_us == first_timestamp);
    tick_after(false, 2983);
    assert(usb_serial_jtag_is_connected()); // 2999 real us; credit is already spent
    tick_after(false, 1);
    assert(!usb_serial_jtag_is_connected()); // 3000 real us without SOF
    assert(usb_serial_jtag_get_sof_diagnostic(&diagnostic));
    assert(diagnostic.disconnect_count == 2 && diagnostic.since_sof_us == 3000);
    assert(diagnostic.missing_ticks == 6);
    assert(diagnostic.delta_us[USB_SERIAL_JTAG_SOF_DIAG_SAMPLES - 7] == 8711);
    // Timestamp storage and age calculation must not wrap at 2^32 microseconds.
    clock_us = (int64_t)UINT32_MAX - 2000;
    tick(true);
    for (unsigned i = 0; i < 4; ++i) tick(false);
    assert(usb_serial_jtag_get_sof_diagnostic(&diagnostic));
    assert(diagnostic.timestamp_us > UINT32_MAX && diagnostic.since_sof_us == 4000);
    uint32_t count;
    assert(usb_serial_jtag_get_last_disconnect_sof(&count) == diagnostic.sof_history);
    assert(count == diagnostic.disconnect_count && count == 3);
#if CONFIG_XIAOTAI_PORT_USB_TIMING_PROBE
    tick(true);
    int64_t before_probe = clock_us;
    assert(usb_serial_jtag_test_delay_tick_once() == ESP_OK);
    assert(clock_us - before_probe == 8000);
    assert(usb_serial_jtag_test_delay_tick_once() == ESP_ERR_INVALID_STATE);
    tick_after(true, 0);
    for (unsigned i = 0; i < 4; ++i) tick_after(false, 10);
    assert(usb_serial_jtag_get_sof_diagnostic(&diagnostic));
    assert(usb_serial_jtag_is_connected() && diagnostic.disconnect_count == 3);
    for (unsigned i = 0; i < 10; ++i) {
        tick_after(true, 1000);
        tick_after(false, 4);
        assert(usb_serial_jtag_is_connected());
    }
#endif
    // A hardware SOF may arrive after a zero raw read and before its W1C.
    // Do not reset raw between ticks: the next sample must retain that event.
    tick(true);
    uint32_t before_race;
    usb_serial_jtag_get_last_disconnect_sof(&before_race);
    raw = 0;
    sof_arrives_after_read = true;
    for (unsigned i = 0; i < 24; ++i) {
        clock_us += 1000;
        raw_reads_this_tick = 0;
        tick_hook();
        unsigned sample = (s_tick_delta_next + USB_SERIAL_JTAG_SOF_DIAG_SAMPLES - 1) % USB_SERIAL_JTAG_SOF_DIAG_SAMPLES;
        assert(s_sof_frame_index[sample] == USB_SERIAL_JTAG.fram_num.sof_frame_index);
        assert(s_sof_sample_flags[sample] == (i % 2 ?
            USB_SERIAL_JTAG_SOF_SAMPLE_OBSERVED | USB_SERIAL_JTAG_SOF_SAMPLE_CLEARED :
            USB_SERIAL_JTAG_SOF_SAMPLE_RAW_AFTER));
        if (!usb_serial_jtag_is_connected()) {
            usb_serial_jtag_get_sof_diagnostic(&diagnostic);
            fprintf(stderr, "SOF read/W1C race: tick=%u count=%u since=%u delta=%u\n", i + 1,
                    diagnostic.disconnect_count, diagnostic.since_sof_us,
                    diagnostic.delta_us[USB_SERIAL_JTAG_SOF_DIAG_SAMPLES - 1]);
        }
        assert(usb_serial_jtag_is_connected());
    }
    sof_arrives_after_read = false;
    uint32_t after_race;
    usb_serial_jtag_get_last_disconnect_sof(&after_race);
    assert(after_race == before_race);
    USB_SERIAL_JTAG.fram_num.sof_frame_index = 2047;
    tick(true);
    for (unsigned i = 0; i < 4; ++i) tick(false);
    assert(usb_serial_jtag_get_sof_diagnostic(&diagnostic));
    assert(diagnostic.disconnect_count == before_race + 1 && diagnostic.frame_index_valid);
    for (unsigned i = USB_SERIAL_JTAG_SOF_DIAG_SAMPLES - 5; i < USB_SERIAL_JTAG_SOF_DIAG_SAMPLES; ++i)
        assert(diagnostic.frame_index[i] == 0);
#endif
    return 0;
}
