// SPDX-License-Identifier: MIT
#include "support.h"
#include "internal.h"
#if defined(XIAOTAI_TRANSPORT_TGTRP_MINI)
#define TGTRP_INTERNAL_DIAG_WEAK_IMPORT
#include "tgtrp_internal_diag.h"
#endif

static char s_output[8192];
static uint32_t s_generation;
static unsigned s_open, s_closed;
static xiaotai_link_end_t s_reason;

static void collect(void *ctx, const char *line)
{
    (void)ctx;
    CHECK(strlen(s_output) + strlen(line) + 2 < sizeof(s_output));
    strcat(s_output, line);
    strcat(s_output, "\n");
}

static xiaotai_err_t command(const char *line)
{
    s_output[0] = 0;
    const xiaotai_reply_t reply = {.write = collect};
    return xiaotai_command_run(line, &reply, 1000);
}

#if defined(XIAOTAI_TRANSPORT_TGTRP_MINI)
static void check_hook_before_init(void)
{
    /* The fixture borrows the hook weakly just like the SDK; the diagnostics
     * provider's strong reference must pull it from the adapter archive. */
    CHECK(tgtrp_internal_diag_hook != NULL);
    tgtrp_internal_diag_hook(NULL);
    tgtrp_internal_diag_t row = {.identity = 0x1234ABCD, .frames = 4, .stream_id = 7, .kind = 2,
        .version = TGTRP_INTERNAL_DIAG_VERSION, .stage = TGTRP_INTERNAL_DIAG_TX};
    tgtrp_internal_diag_hook(&row); /* no framework init yet */
}

static void feed_mini_records(void)
{
    tgtrp_internal_diag_t row = {.identity = 0x1234ABCD, .frames = 4, .stream_id = 7, .kind = 2,
        .version = TGTRP_INTERNAL_DIAG_VERSION, .stage = TGTRP_INTERNAL_DIAG_TX};
    tgtrp_internal_diag_hook(&row);
    row.stage = TGTRP_INTERNAL_DIAG_RX;
    row.payload_bytes = 160;
    row.rx_allocation_failures = 3;
    row.first_failed_frame_id = UINT32_MAX;
    tgtrp_internal_diag_hook(&row);
    row.stage = TGTRP_INTERNAL_DIAG_QUEUE;
    row.auxiliary_frames = 24;
    row.auxiliary_bytes = 3840;
    tgtrp_internal_diag_hook(&row);
    /* Overwrite only TX, and report even after the stack record is gone. */
    row.stage = TGTRP_INTERNAL_DIAG_TX;
    row.now_ms = UINT32_MAX;
    row.frame_id = 401;
    row.frames = 44;
    tgtrp_internal_diag_hook(&row);
    row.frames = 88;
    row.version = 1;
    tgtrp_internal_diag_hook(&row);
    row.version = TGTRP_INTERNAL_DIAG_VERSION;
    row.stage = 0;
    tgtrp_internal_diag_hook(&row);
    row.stage = 4;
    tgtrp_internal_diag_hook(&row);
}

static void check_mini_output(void)
{
    CHECK(strstr(s_output, "+LINK-TG: captures=4\n"));
    CHECK(strstr(s_output, "+LINK-TG-TX: seq=4 time_ms=4294967295 frame=401"));
    CHECK(strstr(s_output, "kind=2 stream=7 frames=44"));
    CHECK(strstr(s_output, "+LINK-TG-RX: seq=2") && strstr(s_output, "bytes=160"));
    CHECK(strstr(s_output, "rx_alloc_failures=3 rx_first_failed_frame=4294967295"));
    CHECK(strstr(s_output, "+LINK-TG-QUEUE: seq=3") && strstr(s_output, "aux_frames=24 aux_bytes=3840"));
    CHECK(!strstr(s_output, "1234abcd") && !strstr(s_output, "1234ABCD") && !strstr(s_output, "305441741"));
    CHECK(!strstr(s_output, "identity=") && !strstr(s_output, "frames=88"));
}
#endif

static void opened(void *ctx, uint32_t generation) { (void)ctx; CHECK(generation == s_generation); ++s_open; }
static void closed(void *ctx, uint32_t generation, xiaotai_link_end_t why, int detail)
{
    (void)ctx;
    (void)detail;
    CHECK(generation == s_generation);
    s_reason = why;
    ++s_closed;
}

static void connect_link(void *ctx)
{
    (void)ctx;
    const xiaotai_link_handlers_t handlers = {.opened = opened, .closed = closed};
    const xiaotai_link_request_t request = {.kind = XIAOTAI_LINK_WHIP, .peer = "unlogged-peer", .token = "unlogged-token"};
    CHECK(xiaotai_link_connect(&request, &handlers, &s_generation) == XIAOTAI_OK);
}

static void report(void *ctx)
{
    (void)ctx;
    const xiaotai_reply_t reply = {.write = collect};
    xiaotai_diag_report("link", &reply);
}

static bool link_event(const char *label)
{
    xiaotai_eventlog_entry_t entries[32];
    const size_t used = xiaotai_eventlog_read(0, entries, 32);
    for (size_t i = 0; i < used; ++i) {
        if (!strcmp(entries[i].source, "link") && !strcmp(entries[i].label, label) &&
            entries[i].generation == s_generation && entries[i].kind == XIAOTAI_EVENTLOG_LINK) return true;
    }
    return false;
}

int main(void)
{
#if defined(XIAOTAI_TRANSPORT_TGTRP_MINI)
    check_hook_before_init();
#endif
    const xiaotai_transport_config_t config = {.start_failure = XIAOTAI_TRANSPORT_RECOVER_ON_EVENT,
                                                .alert_ends_links = true};
    boot(&config);
#if defined(XIAOTAI_TRANSPORT_TGTRP_MINI)
    CHECK(command("AT+LINK") == XIAOTAI_OK && strstr(s_output, "+LINK-TG: captures=0\n"));
    feed_mini_records();
    CHECK(command("AT+LINK") == XIAOTAI_OK);
    check_mini_output();
#else
    CHECK(command("AT+LINK") == XIAOTAI_OK && !strstr(s_output, "+LINK-TG"));
#endif
    CHECK(command("AT+MEMTRACE") == XIAOTAI_ERR_NOT_FOUND);
    CHECK(command("AT+LINK BOGUS") == XIAOTAI_ERR_INVALID_ARG);
    CHECK(command("AT+LINK TRACE") == XIAOTAI_OK && !strcmp(s_output, "+LINK-TRACE: count=0\n"));
    unsigned registered = 0;
    for (size_t i = 0; i < xiaotai_command_count(); ++i) {
        const char *name = NULL;
        xiaotai_command_at(i, &name, NULL);
        if (name && !strcmp(name, "AT+LINK")) ++registered;
    }
    CHECK(registered == 1);
    xiaotai_transport_host_fail_start(XIAOTAI_FAIL, -91, 1);
    give_account();
    CHECK(command("AT+LINK") == XIAOTAI_OK && strstr(s_output, "error=-91 wait=start_failed"));
    xiaotai_transport_restart();
    settle();
    xiaotai_transport_host_started();
    settle();
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE);
    CHECK(xiaotai_run(connect_link, NULL) == XIAOTAI_OK);
    settle();
    xiaotai_transport_host_connected(s_generation, 0x512, XIAOTAI_LINK_END_CONNECT_FAILED, 0);
    settle();
    CHECK(s_open == 1 && link_event("link_request") && link_event("link_open"));

    const char payload[] = "unlogged-payload";
    CHECK(xiaotai_link_command(s_generation, 0x10001, payload, sizeof(payload) - 1U) == XIAOTAI_OK);
    for (uint32_t i = 0; i < 6; ++i) {
        xiaotai_transport_host_command(0x512, 0, 0x20000U + i, (const uint8_t *)payload, sizeof(payload) - 1U);
        settle();
    }
    CHECK(command("AT+LINK TRACE") == XIAOTAI_OK && strstr(s_output, "count=8"));
    CHECK(strstr(s_output, "dir=rx") && strstr(s_output, "dir=dispatch") && !strstr(s_output, "dir=tx"));
    CHECK(!strstr(s_output, "word=0x00020000") && strstr(s_output, "word=0x00020005"));
    CHECK(!strstr(s_output, "unlogged-"));

    xiaotai_transport_host_alert();
    settle();
    CHECK(s_closed == 1 && s_reason == XIAOTAI_LINK_END_ALERT && link_event("link_end"));
    CHECK(xiaotai_transport_state() == XIAOTAI_TRANSPORT_ONLINE && calls(XIAOTAI_TRANSPORT_HOST_STOP) == 0);
    s_output[0] = 0;
    CHECK(xiaotai_run(report, NULL) == XIAOTAI_OK);
    owner_sync();
    CHECK(strstr(s_output, "+LINK-SLOT:") && strstr(s_output, "commands_rx=6") && strstr(s_output, "alerts=1"));
    CHECK(strstr(s_output, "+LINK-MEDIA:") && strstr(s_output, "+LINK-TRACE:") && !strstr(s_output, "unlogged-"));
#if defined(XIAOTAI_TRANSPORT_TGTRP_MINI)
    check_mini_output();
#endif
    shutdown_all();
#if defined(XIAOTAI_TRANSPORT_TGTRP_MINI)
    feed_mini_records(); /* late SDK reports after shutdown are harmless */
    boot(&config);
    CHECK(command("AT+LINK") == XIAOTAI_OK && strstr(s_output, "+LINK-TG: captures=0\n"));
    shutdown_all();
#endif
    return 0;
}
