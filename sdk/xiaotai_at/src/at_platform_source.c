// SPDX-License-Identifier: MIT
#include "internal.h"
#include "xiaotai/port/console.h"

/* A component-only host executable need not link a selected board. Firmware
 * always resolves the real HAL; whole-product host builds do so as well. */
#ifdef XIAOTAI_AT_HOST_PARTIAL
#pragma weak xiaotai_hal_identity
#pragma weak xiaotai_hal_report
#endif

void at_board_identity(xiaotai_board_identity_t *identity)
{
#ifdef XIAOTAI_AT_HOST_PARTIAL
    if (!xiaotai_hal_identity) return;
#endif
    xiaotai_hal_identity(identity);
}

void at_board_report(const xiaotai_reply_t *reply)
{
#ifdef XIAOTAI_AT_HOST_PARTIAL
    if (!xiaotai_hal_report) return;
#endif
    xiaotai_hal_report(reply->write, reply->ctx);
}

static size_t read_platform(void *self, uint8_t *data, size_t capacity, uint32_t timeout_ms)
{
    (void)self;
    return xiaotai_console_read((char *)data, capacity, timeout_ms);
}

static xiaotai_err_t write_platform(void *self, const char *data, size_t size)
{
    (void)self;
    return xiaotai_console_write(data, size);
}

static void bulk_platform(void *self, bool active)
{
    (void)self;
    xiaotai_console_bulk(active);
}

static const xiaotai_at_io_t s_platform = {
    .name = "platform", .read = read_platform, .write = write_platform, .bulk = bulk_platform,
};

const xiaotai_at_io_t *at_platform_source(void) { return &s_platform; }
xiaotai_err_t at_platform_open(void) { return xiaotai_console_open(NULL); }

void at_describe_source(const xiaotai_at_io_t *io, const xiaotai_reply_t *reply)
{
    if (io != &s_platform) {
        xiaotai_at_printf(reply, "+CONSOLE-SOURCE: name=%s", io->name);
        return;
    }
    xiaotai_console_info_t info;
    xiaotai_console_stats_t stats;
    xiaotai_console_info(&info);
    xiaotai_console_stats(&stats);
    xiaotai_at_printf(reply, "+CONSOLE-SOURCE: name=%s device=%s baud=%lu rx_bytes=%lu rx_overflows=%lu tx_bytes=%lu",
                      io->name, info.device, (unsigned long)info.baud_rate, (unsigned long)stats.rx_bytes,
                      (unsigned long)stats.rx_overflows, (unsigned long)stats.tx_bytes);
}
