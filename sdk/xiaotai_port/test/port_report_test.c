// SPDX-License-Identifier: MIT
#include "xiaotai/port/sys.h"

#include <assert.h>
#include <ctype.h>
#include <string.h>

#if defined(XIAOTAI_PORT_NATIVE_STARTUP) && CONFIG_IDF_TARGET_ESP32S3
#include "esp_timer.h"
#include "soc/extmem_reg.h"
#include "soc/soc.h"

static unsigned register_reads, time_reads;
uint32_t report_test_reg_read(unsigned reg)
{
    assert(reg == register_reads++ % TEST_CACHE_REGS);
    return UINT32_MAX - reg;
}

int64_t esp_timer_get_time(void)
{
    return INT64_C(4294967296) + 14 * (int64_t)(time_reads++);
}
#endif

#ifdef XIAOTAI_PORT_NATIVE_STARTUP
#include "usb_monitor/xiaotai_usb_diagnostics.h"

static unsigned usb_reads;
static bool disconnected, with_index;

bool usb_serial_jtag_get_sof_diagnostic(usb_serial_jtag_sof_diagnostic_t *diagnostic)
{
    ++usb_reads;
    if (!disconnected) return false;
    *diagnostic = (usb_serial_jtag_sof_diagnostic_t){
        .timestamp_us = UINT64_C(4294967632), .disconnect_count = 2, .sof_history = 0x80000001U,
        .since_sof_us = 3100, .missing_ticks = 3, .frame_index_valid = with_index,
    };
    for (size_t i = 0; i < USB_SERIAL_JTAG_SOF_DIAG_SAMPLES; ++i) {
        diagnostic->delta_us[i] = UINT32_MAX - (uint32_t)i;
        diagnostic->frame_index[i] = (uint16_t)(100 + i);
        diagnostic->sample_flags[i] = (uint8_t)(i & 7);
    }
    return true;
}

#if CONFIG_XIAOTAI_PORT_USB_TIMING_PROBE
static bool ran_probe, attached;
static unsigned probe_requests;
esp_err_t usb_serial_jtag_test_delay_tick_once(void)
{
    ++probe_requests;
    if (!attached || ran_probe) return XIAOTAI_ERR_INVALID_STATE;
    ran_probe = true;
    return XIAOTAI_OK;
}
#endif
#endif

typedef struct {
    size_t count;
    char lines[8][200];
} output_t;

static void save_line(void *ctx, const char *line)
{
    output_t *out = ctx;
    assert(out->count < 8 && strlen(line) < sizeof(out->lines[0]));
    const char *p = line;
    while (*p) {
        assert(islower((unsigned char)*p++));
        while (islower((unsigned char)*p) || *p == '_') ++p;
        assert(*p++ == '=');
        const char *value = p;
        while (*p && *p != ' ') {
            assert(isxdigit((unsigned char)*p) || *p == '-' || *p == ',');
            ++p;
        }
        assert(p > value);
        if (*p) ++p;
    }
    strcpy(out->lines[out->count++], line);
}

int main(void)
{
    output_t out = {0};
    xiaotai_port_report(NULL, NULL);
    xiaotai_port_report(save_line, &out);
#if defined(XIAOTAI_PORT_NATIVE_STARTUP) && CONFIG_IDF_TARGET_ESP32S3
    const size_t cache_lines = 2;
    assert(out.count == cache_lines && register_reads == TEST_CACHE_REGS && time_reads == 2);
    assert(strcmp(out.lines[0], "cache_t_us=4294967296 cache_read_us=14 cache_data=4294967295"
                  " cache_psram_miss=4294967294 cache_flash_miss=4294967293") == 0);
    assert(strcmp(out.lines[1], "cache_instruction=4294967292 cache_instruction_miss=4294967291"
                  " cache_flags=fffffffa cache_data_filter=fffffff9-fffffff8"
                  " cache_instruction_filter=fffffff7-fffffff6") == 0);
#elif defined(XIAOTAI_PORT_NATIVE_STARTUP)
    const size_t cache_lines = 0;
    assert(!out.count);
#else
    assert(!out.count);
#endif

#ifdef XIAOTAI_PORT_NATIVE_STARTUP
    assert(usb_reads == 1);
    disconnected = true;
    out.count = 0;
    xiaotai_port_report(save_line, &out);
    assert(out.count == 3 + cache_lines && usb_reads == 2);
    assert(strcmp(out.lines[0], "usb_disconnects=2 usb_sof_history=80000001 usb_since_sof_us=3100"
                  " usb_missing_ticks=3 usb_t_us=4294967632") == 0);
    assert(strcmp(out.lines[1], "usb_delta_us=4294967295,4294967294,4294967293,4294967292,"
                  "4294967291,4294967290,4294967289,4294967288,4294967287,4294967286,4294967285,"
                  "4294967284,4294967283,4294967282,4294967281,4294967280") == 0);
    assert(strcmp(out.lines[2], "usb_sample_flags=0,1,2,3,4,5,6,7,0,1,2,3,4,5,6,7") == 0);
    with_index = true;
    out.count = 0;
    xiaotai_port_report(save_line, &out);
    assert(out.count == 4 + cache_lines && usb_reads == 3);
    assert(strcmp(out.lines[2], "usb_frame_index=100,101,102,103,104,105,106,107,108,109,110,111,"
                  "112,113,114,115") == 0);
#endif

    out.count = 0;
    const char *const *names = xiaotai_port_probes();
    assert(xiaotai_port_probe(NULL, save_line, &out) == XIAOTAI_ERR_NOT_FOUND);
    assert(xiaotai_port_probe("unknown", save_line, &out) == XIAOTAI_ERR_NOT_FOUND);
#if defined(XIAOTAI_PORT_NATIVE_STARTUP) && CONFIG_XIAOTAI_PORT_USB_TIMING_PROBE
    assert(strcmp(names[0], "usb_tick") == 0 && !names[1]);
    assert(xiaotai_port_probe("usb_tick", save_line, &out) == XIAOTAI_ERR_INVALID_STATE);
    assert(!out.count && probe_requests == 1);
    attached = true;
    assert(xiaotai_port_probe("usb_tick", save_line, &out) == XIAOTAI_OK);
    assert(xiaotai_port_probe("usb_tick", save_line, &out) == XIAOTAI_ERR_INVALID_STATE);
    assert(out.count == 1 && probe_requests == 3);
    assert(strcmp(out.lines[0], "usb_tick_stall_us=8000") == 0);
#else
    assert(!names[0] && xiaotai_port_probe("usb_tick", save_line, &out) == XIAOTAI_ERR_NOT_FOUND);
    assert(!out.count);
#endif
    return 0;
}
