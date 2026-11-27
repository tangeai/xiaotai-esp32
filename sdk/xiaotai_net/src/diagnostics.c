// SPDX-License-Identifier: MIT
#include "internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/at_command.h"
#include "xiaotai/port/mem.h"

enum { REPORT_LINES = 4, REPORT_CHARS = (XIAOTAI_AT_OUT_LINE_MAX - 32U) / 2U };

typedef struct {
    size_t used;
    bool overflow;
    char lines[REPORT_LINES][REPORT_CHARS + 1];
} readback_t;

static const char *loss_name(xiaotai_net_loss_t loss)
{
    static const char *const names[] = {
        "unknown", "requested", "not_found", "credentials", "signal", "refused", "address", "security",
    };
    return (unsigned)loss < sizeof(names) / sizeof(names[0]) ? names[loss] : names[0];
}

static void loss_line(const char *record, unsigned index, const xiaotai_net_loss_record_t *loss,
                      const xiaotai_reply_t *reply)
{
    char dbm[8] = "-";
    if (loss->signal_valid) (void)snprintf(dbm, sizeof(dbm), "%d", (int)loss->signal_dbm);
    xiaotai_at_printf(reply, "+NET-DIAG: type=%s index=%u uptime_s=%" PRIu32 " loss=%s code=%" PRId32 " rssi=%s",
                     record, index, loss->uptime_s, loss_name(loss->loss), loss->code, dbm);
}

static void cached_diagnostics(const xiaotai_reply_t *reply)
{
    xiaotai_net_diag_t diag;
    const bool first = net_diag_snapshot(&diag);
    xiaotai_at_printf(reply, "+NET-DIAG: type=link attempts=%" PRIu32 " addresses=%" PRIu32 " losses=%" PRIu32
                     " signal_losses=%" PRIu32 " stale_events=%" PRIu32 " dropped_events=%" PRIu32
                     " backoff_ms=%" PRIu32 " join_ms=%" PRIu32 " address_ms=%" PRIu32,
                     diag.attempts, diag.addresses, diag.losses, diag.signal_losses, diag.stale_events,
                     diag.dropped_events, diag.backoff_ms, diag.join_ms, diag.address_ms);
    xiaotai_at_printf(reply, "+NET-DIAG: type=setup scans=%" PRIu32 " scan_timeouts=%" PRIu32
                     " submissions=%" PRIu32 " submissions_rejected=%" PRIu32 " store_failures=%" PRIu32
                     " history_deferred=%" PRIu32 " portal_requests=%" PRIu32 " portal_refused=%" PRIu32
                     " dns_queries=%" PRIu32,
                     diag.scans, diag.scan_timeouts, diag.submissions, diag.submissions_rejected, diag.store_failures,
                     diag.history_deferred, diag.portal_requests, diag.portal_refused, diag.dns_queries);
    if (first) loss_line("first", 0, &diag.first, reply);
    for (unsigned i = 0; i < diag.recent_count && i < XIAOTAI_NET_LOSS_HISTORY; ++i)
        loss_line("loss", i, &diag.recent[i], reply);
}

static void keep_readback(void *ctx, const char *text)
{
    readback_t *readback = ctx;
    while (text && *text) {
        const size_t len = strcspn(text, "\n");
        if (len && (len > REPORT_CHARS || readback->used == REPORT_LINES)) {
            readback->overflow = true;
            return;
        }
        if (len) {
            memcpy(readback->lines[readback->used], text, len);
            readback->lines[readback->used++][len] = '\0';
        }
        text += len;
        if (*text) ++text;
    }
}

static xiaotai_err_t read_driver(net_call_t *call)
{
    readback_t *readback;
    memcpy(&readback, call->data, sizeof(readback));
    const xiaotai_net_driver_t *driver = net_state->driver;
    if (!net_state->driver_running || !net_state->link.radio_ready || !driver || !driver->report)
        return XIAOTAI_ERR_NOT_SUPPORTED;
    const xiaotai_reply_t out = {.write = keep_readback, .ctx = readback};
    driver->report(driver->self, &out);
    return readback->overflow ? XIAOTAI_ERR_INVALID_SIZE : XIAOTAI_OK;
}

static bool stream_diagnostics(void *ctx, const xiaotai_reply_t *reply)
{
    (void)ctx;
    cached_diagnostics(reply);
    readback_t *readback = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*readback));
    if (!readback) {
        (void)xiaotai_at_fail(reply, XIAOTAI_ERR_NO_MEM, NULL);
        return false;
    }
    /* Like COMPANION LIVE, the console waits for the accepted worker call.
     * The capture buffer is borrowed only for that call, never for an event. */
    const xiaotai_err_t result = net_call(read_driver, 0, &readback, sizeof(readback), NULL, 0, NULL,
                                          XIAOTAI_WAIT_FOREVER);
    if (result == XIAOTAI_ERR_NOT_SUPPORTED || (result == XIAOTAI_OK && !readback->used)) {
        xiaotai_at_printf(reply, "+NET-DIAG: type=driver driver_hex=-");
    } else if (result != XIAOTAI_OK) {
        (void)xiaotai_at_fail(reply, result, "DRIVER_REPORT");
    } else {
        for (size_t i = 0; i < readback->used; ++i) {
            char hex[2 * REPORT_CHARS + 1];
            (void)xiaotai_at_text_encode(readback->lines[i], hex, sizeof(hex));
            xiaotai_at_printf(reply, "+NET-DIAG: type=driver driver_hex=%s", hex);
        }
    }
    xiaotai_mem_free(readback);
    return false;
}

xiaotai_err_t net_diagnostics_run(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc;
    (void)argv;
    return xiaotai_at_stream(reply, &(xiaotai_at_stream_t){.next = stream_diagnostics});
}

xiaotai_err_t net_diagnostics_register(void) { return xiaotai_diag_register("net", cached_diagnostics); }
