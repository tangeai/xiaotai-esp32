// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_c61_ota_image_admission.py
// Replaces legacy: tests/regression/test_companion_maintenance.py
// Replaces legacy: tests/host_crypto/run.py::c61_image_sha256
// Replaces legacy: tests/regression/test_p4_companion_identity.py
#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#include "internal.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/sys.h"
#include "xiaotai_http_host.h"
#include "xiaotai_net_host.h"

#define WAIT 3000U
static const uint8_t image[] = {0x12, 0xAB, 0xEF};
static const char rollback_line[] = "+COMPANION: rollback=requested\r\n";
static const char update_line[] = "+COMPANION-UPDATE: stage=done percent=100 error=0\r\n";
static atomic_uint s_commands, s_updates, s_downloads, s_restarts, s_quiesce_attempts;
static atomic_bool s_rollback_written, s_update_written, s_progress_published;
static atomic_bool s_fail_update_task;
static xiaotai_signal_t s_owner, s_restarted, s_allow_update_return;
static uint32_t s_activity, s_other_quiesce;
static bool s_external = true, s_hook_fail, s_fail_output, s_no_hooks;
static bool s_need_rollback_line, s_need_update_line, s_stress_notice;
static bool s_prepare_only, s_hold_retry, s_keep_fault, s_restrict_retry, s_fail_driver_start;
static bool s_maintenance_info;
static xiaotai_network_stage_t s_first_prepare = XIAOTAI_NETWORK_READY;
static xiaotai_err_t s_no_link_error = XIAOTAI_ERR_TIMEOUT;
static atomic_uint s_prepares, s_manual_prepares, s_identity_reads;
static xiaotai_signal_t s_retry_entered, s_retry_release;
static int64_t s_content_length = sizeof(image);
static size_t s_body_size = sizeof(image);
static bool s_fail_receive;
static const char *s_input;
static size_t s_input_left, s_output_used;
static char s_output[12000], update_command[300];

static const xiaotai_coprocessor_info_t c6_identity = {
    .project = "eh_cp_wifi_sta", .version = "3.0.7-xiaotai.5", .platform = "v5.5.4", .protocol = "3.0.7",
    .elf_sha256 = "dead000000000000000000000000000000000000000000000000000000000000",
};
static const char c6_identity_line[] =
    "+COMPANION: project=eh_cp_wifi_sta version=3.0.7-xiaotai.5 platform=v5.5.4 protocol=3.0.7 "
    "elf_sha256=dead000000000000000000000000000000000000000000000000000000000000\r\nOK\r\n";

bool __wrap_xiaotai_mem_has_external(void);
bool __wrap_xiaotai_mem_has_external(void) { return s_external; }

xiaotai_err_t __real_xiaotai_task_create(const xiaotai_task_config_t *, xiaotai_task_t *);
xiaotai_err_t __wrap_xiaotai_task_create(const xiaotai_task_config_t *config, xiaotai_task_t *task)
{
    if (!strcmp(config->name, "xiaotai_cp") && atomic_exchange(&s_fail_update_task, false)) {
        uint32_t ticket = 0;
        assert(xiaotai_quiesced());
        assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_ERR_INVALID_STATE && !ticket);
        return XIAOTAI_ERR_NO_MEM;
    }
    return __real_xiaotai_task_create(config, task);
}

static size_t read_source(void *self, uint8_t *data, size_t capacity, uint32_t timeout_ms)
{
    assert(!self);
    if (!s_input_left) {
        xiaotai_delay_ms(timeout_ms);
        return 0;
    }
    const size_t count = s_input_left < capacity ? s_input_left : capacity;
    memcpy(data, s_input, count);
    s_input += count;
    s_input_left -= count;
    return count;
}

static xiaotai_err_t write_source(void *self, const char *data, size_t size)
{
    assert(!self && size >= 2 && data[size - 2] == '\r' && data[size - 1] == '\n');
    const bool rollback = size == sizeof(rollback_line) - 1 && !memcmp(data, rollback_line, size);
    const bool update = size == sizeof(update_line) - 1 && !memcmp(data, update_line, size);
    if (rollback && s_fail_output) return XIAOTAI_FAIL;
    assert(s_output_used + size < sizeof(s_output));
    memcpy(s_output + s_output_used, data, size);
    s_output_used += size;
    s_output[s_output_used] = 0;
    if (rollback) atomic_store(&s_rollback_written, true);
    if (update) atomic_store(&s_update_written, true);
    return XIAOTAI_OK;
}

static const xiaotai_at_io_t source = {.name = "fixture", .read = read_source, .write = write_source};

static xiaotai_err_t prepare(void *self, bool manual, xiaotai_network_progress_fn_t report, void *ctx)
{
    assert(!self && !manual && net_on_worker());
    report(XIAOTAI_NETWORK_READY, 100, ctx);
    return XIAOTAI_OK;
}

static xiaotai_err_t info(void *self, bool live, xiaotai_coprocessor_info_t *out)
{
    assert(!self && net_on_worker());
    atomic_fetch_add(&s_identity_reads, 1);
    strcpy(out->project, "fixture");
    strcpy(out->version, live ? "live" : "cached");
    strcpy(out->platform, "sample");
    memset(out->elf_sha256, 'a', sizeof(out->elf_sha256) - 1);
    return XIAOTAI_OK;
}

static xiaotai_err_t failed_prepare(xiaotai_network_stage_t stage)
{
    switch (stage) {
    case XIAOTAI_NETWORK_NO_LINK: return s_no_link_error;
    case XIAOTAI_NETWORK_UNFINISHED: return XIAOTAI_ERR_INVALID_STATE;
    case XIAOTAI_NETWORK_BAD_IMAGE: return XIAOTAI_ERR_INVALID_CRC;
    case XIAOTAI_NETWORK_UNSUPPORTED: return XIAOTAI_ERR_NOT_SUPPORTED;
    case XIAOTAI_NETWORK_POWER_CYCLE: return XIAOTAI_ERR_INVALID_STATE;
    case XIAOTAI_NETWORK_CHECKING: return XIAOTAI_FAIL;
    default: assert(false); return XIAOTAI_FAIL;
    }
}

static xiaotai_err_t prepare_identity(void *self, bool manual, xiaotai_network_progress_fn_t report,
                                      xiaotai_network_identity_fn_t publish, void *ctx)
{
    assert(!self && net_on_worker() && publish);
    atomic_fetch_add(&s_prepares, 1);
    report(XIAOTAI_NETWORK_CHECKING, 0, ctx);
    if (manual) {
        atomic_fetch_add(&s_manual_prepares, 1);
        if (s_hold_retry) {
            xiaotai_signal_give(&s_retry_entered);
            assert(xiaotai_signal_take(&s_retry_release, WAIT));
        }
    }
    if (s_first_prepare != XIAOTAI_NETWORK_READY && (!manual || s_keep_fault)) {
        report(s_first_prepare, 0, ctx);
        return failed_prepare(s_first_prepare);
    }
    xiaotai_coprocessor_info_t snapshot = c6_identity;
    publish(&snapshot, ctx);
    memset(&snapshot, 0, sizeof(snapshot));
    report(XIAOTAI_NETWORK_READY, 100, ctx);
    return XIAOTAI_OK;
}

static xiaotai_err_t run_part_command(void *self, const char *command, char *reply, size_t capacity)
{
    assert(!self && net_on_worker());
    atomic_fetch_add(&s_commands, 1);
    if (!strcmp(command, "ROLLBACK")) {
        assert(xiaotai_quiesced() && !atomic_load(&s_rollback_written));
        return s_hook_fail ? XIAOTAI_FAIL : XIAOTAI_OK;
    }
    assert(!strcmp(command, "INFO"));
    (void)snprintf(reply, capacity, "a b");
    return XIAOTAI_OK;
}

static xiaotai_err_t part_update(void *self, const uint8_t *data, size_t size,
                                xiaotai_network_progress_fn_t report, void *ctx)
{
    assert(!self && net_on_worker() && xiaotai_quiesced());
    assert(size == sizeof(image) && !memcmp(data, image, size));
    atomic_fetch_add(&s_updates, 1);
    report(XIAOTAI_NETWORK_WRITING, 50, ctx);
    report(XIAOTAI_NETWORK_WRITING, 50, ctx);
    if (!s_hook_fail) report(XIAOTAI_NETWORK_RESTARTING, 100, ctx);
    atomic_store(&s_progress_published, true);
    if (s_stress_notice) assert(xiaotai_signal_take(&s_allow_update_return, WAIT));
    return s_hook_fail ? XIAOTAI_FAIL : XIAOTAI_OK;
}

const xiaotai_network_t *xiaotai_hal_network(size_t index)
{
    static const xiaotai_network_t part = {.desc = {.kind = XIAOTAI_NETWORK_WIFI_COPROCESSOR},
        .prepare = prepare, .coprocessor_info = info, .coprocessor_command = run_part_command, .coprocessor_update = part_update};
    static const xiaotai_network_t prepare_only = {.desc = {.kind = XIAOTAI_NETWORK_WIFI_COPROCESSOR},
        .prepare = prepare, .prepare_with_identity = prepare_identity, .coprocessor_info = info};
    static const xiaotai_network_t plain = {.desc = {.kind = XIAOTAI_NETWORK_WIFI}};
    return index ? NULL : s_no_hooks ? &plain : s_prepare_only ? &prepare_only : &part;
}

static void restart(void)
{
    assert(net_on_worker() && xiaotai_quiesced());
    if (s_need_rollback_line) assert(atomic_load(&s_rollback_written));
    if (s_need_update_line) assert(atomic_load(&s_update_written));
    atomic_fetch_add(&s_restarts, 1);
    xiaotai_signal_give(&s_restarted);
}

static void http_reply(void *ctx, const xiaotai_http_host_request_t *request, xiaotai_http_host_reply_t *reply)
{
    assert(!ctx && !net_on_worker() && !xiaotai_on_owner() && xiaotai_quiesced());
    assert(request->method == XIAOTAI_HTTP_GET && !strcmp(request->url, "http://fixture.invalid/update"));
    assert(net_gate_config_begin() == XIAOTAI_ERR_INVALID_STATE);
    atomic_fetch_add(&s_downloads, 1);
    *reply = (xiaotai_http_host_reply_t){.status = 200, .content_length = s_content_length,
        .body = image, .body_size = s_body_size, .read_chunk_bytes = 1};
    if (s_fail_receive) {
        reply->fail_stage = XIAOTAI_HTTP_STAGE_RECEIVE;
        reply->fail_after_bytes = 1;
        reply->error = XIAOTAI_ERR_TIMEOUT;
    }
}

static void give(void *ctx) { xiaotai_signal_give(ctx); }
static void owner_sync(void)
{
    assert(xiaotai_run(give, &s_owner) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_owner, WAIT));
}

static void settle(void)
{
    assert(xiaotai_net_host_sync(WAIT) == XIAOTAI_OK);
    assert(xiaotai_net_host_sync(WAIT) == XIAOTAI_OK);
    owner_sync();
}

static xiaotai_err_t watch_quiesce(const char *reason, void *ctx)
{
    assert(!ctx && xiaotai_on_owner());
    if (!strcmp(reason, XIAOTAI_QUIESCE_COPROCESSOR)) atomic_fetch_add(&s_quiesce_attempts, 1);
    return XIAOTAI_OK;
}

static xiaotai_err_t init_fixture(void)
{
    return xiaotai_quiesce_guard_register("cp_fixture", watch_quiesce, NULL);
}

static void boot(void)
{
    assert(xiaotai_net_configure(&(xiaotai_net_config_t){
        .block_retry_after_driver_start_failure = s_fail_driver_start,
        .restrict_blocked_retry_to_recoverable = s_restrict_retry,
        .allow_coprocessor_info_during_maintenance = s_maintenance_info,
    }) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_owner) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_restarted) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_allow_update_return) == XIAOTAI_OK);
    uint8_t record[99] = {1};
    memcpy(record + 1, "fixture", sizeof("fixture"));
    assert(xiaotai_kv_init() == XIAOTAI_OK);
    assert(xiaotai_kv_set("wifi_cfg", "credentials", record, sizeof(record), WAIT) == XIAOTAI_OK);
    assert(xiaotai_at_configure(&(xiaotai_at_config_t){
        .task_stack_bytes = XIAOTAI_AT_NO_TASK, .no_platform_console = true,
        .notify_bytes = s_stress_notice ? 258 : 0,
    }) == XIAOTAI_OK);
    assert(xiaotai_at_attach(&source) == XIAOTAI_OK && xiaotai_at_open() == XIAOTAI_OK);
    static const xiaotai_module_t log = {.name = "eventlog", .init = xiaotai_engine_eventlog_init};
    static const char *const at_depends[] = {"eventlog", NULL};
    static const xiaotai_module_t at = {.name = "at", .depends = at_depends,
        .init = xiaotai_engine_at_init, .start = xiaotai_engine_at_start};
    static const xiaotai_module_t net = {.name = "net", .init = xiaotai_engine_net_init, .start = xiaotai_engine_net_start};
    static const xiaotai_module_t watch = {.name = "watch", .init = init_fixture};
    static const xiaotai_module_t *const modules[] = {&log, &at, &net, &watch};
    if (s_fail_driver_start) {
        const xiaotai_net_host_step_t fail = {.result = XIAOTAI_FAIL};
        assert(xiaotai_net_host_step(XIAOTAI_NET_HOST_START, &fail) == XIAOTAI_OK);
    }
    xiaotai_net_host_set_restart(restart);
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = modules, .module_count = 4}) == XIAOTAI_OK);
    owner_sync();
    settle();
    if (s_first_prepare != XIAOTAI_NETWORK_READY || s_fail_driver_start) {
        assert(net_state_now() == XIAOTAI_NET_BLOCKED);
        return;
    }
    xiaotai_net_host_record_t records[16];
    const size_t count = xiaotai_net_host_records(records, 16);
    uint32_t token = 0;
    for (size_t i = 0; i < count; ++i) if (records[i].call == XIAOTAI_NET_HOST_JOIN) token = records[i].token;
    assert(token);
    assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){
        .kind = XIAOTAI_NET_DRIVER_JOINED, .token = token, .ssid = "fixture",
    }) == XIAOTAI_OK);
    assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){
        .kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = token, .ipv4 = 42,
    }) == XIAOTAI_OK);
    settle();
    assert(xiaotai_net_generation());
}

static void stop(void)
{
    xiaotai_http_host_set_responder(NULL, NULL, false);
    xiaotai_net_reset();
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_eventlog_reset();
    xiaotai_signal_deinit(&s_allow_update_return);
    xiaotai_signal_deinit(&s_restarted);
    xiaotai_signal_deinit(&s_owner);
}

static void serial(const char *command, const char *expected)
{
    s_output_used = 0;
    s_output[0] = 0;
    s_input = command;
    s_input_left = strlen(command);
    for (unsigned i = 0; i < 10 && s_input_left; ++i) assert(xiaotai_at_service(15) == XIAOTAI_OK);
    assert(!s_input_left);
    if (!strstr(s_output, expected)) fprintf(stderr, "Response [%s], expected [%s]\n", s_output, expected);
    assert(strstr(s_output, expected));
}

static void cached_reply(const char *expected)
{
    serial("AT+COMPANION\r", expected);
    assert(!atomic_load(&s_identity_reads));
    if (strncmp(expected, "ERROR:", 6) == 0)
        assert(!strstr(s_output, "+COMPANION:") && !strstr(s_output, "OK\r\n"));
}

static void p4_cached_identity(void)
{
    s_prepare_only = true;
    boot();
    cached_reply(c6_identity_line);
    cached_reply(c6_identity_line);
    serial("AT+CLI OFF\r", "OK\r\n");
    cached_reply("ERROR: INVALID_STATE CLI_DISABLED\r\n");
    serial("AT+CLI ON\r", "OK\r\n");
    cached_reply(c6_identity_line);

    xiaotai_coprocessor_info_t bad = c6_identity;
    strcpy(bad.platform, "v5.5.4\r\nOK");
    net_coprocessor_publish_prepare_identity(&bad, NULL);
    cached_reply("ERROR: INVALID_ARG IDENTITY\r\n");
    bad = c6_identity;
    memset(bad.platform, 'X', sizeof(bad.platform));
    net_coprocessor_publish_prepare_identity(&bad, NULL);
    cached_reply("ERROR: NOT_FOUND\r\n");
    net_coprocessor_publish_prepare_identity(NULL, NULL);
    cached_reply("ERROR: NOT_FOUND\r\n");

    serial("AT+COMPANION UPDATE unused unused\r", "ERROR: INVALID_ARG UNKNOWN_VERB\r\n");
    serial("AT+HELP\r", "+HELP: AT+COMPANION\r\n");
    assert(strstr(s_output, "+HELP: AT+COMPANION RETRY\r\n"));
    assert(!strstr(s_output, "+HELP: AT+COMPANION UPDATE") && !strstr(s_output, "CPOTA"));
    assert(atomic_load(&s_prepares) == 1 && !atomic_load(&s_manual_prepares));
    assert(!atomic_load(&s_updates) && !atomic_load(&s_commands) && !atomic_load(&s_downloads));
    stop();
}

static void retry_history(xiaotai_network_stage_t initial, xiaotai_net_fault_t expected, bool restrict_retry)
{
    s_prepare_only = s_hold_retry = true;
    s_restrict_retry = restrict_retry;
    s_first_prepare = initial;
    assert(xiaotai_signal_init(&s_retry_entered) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&s_retry_release) == XIAOTAI_OK);
    boot();
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    assert(status.prepare.fault == expected && status.prepare.error == failed_prepare(initial) &&
           !status.prepare.retry_blocked);
    assert(atomic_load(&s_prepares) == 1);
    cached_reply("ERROR: NOT_FOUND\r\n");
    serial("AT+COMPANION RETRY\r", "OK\r\n");
    assert(xiaotai_signal_take(&s_retry_entered, WAIT));
    serial("AT+COMPANION RETRY\r", "ERROR: INVALID_STATE COMPANION_RETRY_UNAVAILABLE\r\n");
    assert(atomic_load(&s_manual_prepares) == 1 && atomic_load(&s_prepares) == 2);
    xiaotai_signal_give(&s_retry_release);
    settle();
    cached_reply(c6_identity_line);
    serial("AT+COMPANION RETRY\r", "ERROR: INVALID_STATE COMPANION_RETRY_UNAVAILABLE\r\n");
    assert(xiaotai_net_retry() == XIAOTAI_OK);
    settle();
    assert(atomic_load(&s_manual_prepares) == 1 && atomic_load(&s_prepares) == 2);
    stop();
    xiaotai_signal_deinit(&s_retry_release);
    xiaotai_signal_deinit(&s_retry_entered);
}

static void retry_on_worker(const net_message_t *message)
{
    (void)message;
    net_bringup(true);
}

static void driver_start_failure(bool restrict_retry)
{
    s_prepare_only = s_fail_driver_start = true;
    s_restrict_retry = restrict_retry;
    boot();
    uint32_t ticket = 0;
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK && ticket);
    xiaotai_net_maintenance_end(ticket);
    serial("AT+COMPANION RETRY\r", "ERROR: INVALID_STATE\r\n");
    assert(!strstr(s_output, "OK\r\n"));
    settle();
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    assert(status.state == XIAOTAI_NET_BLOCKED && status.prepare.fault == XIAOTAI_NET_FAULT_START &&
           status.prepare.error == XIAOTAI_FAIL && status.prepare.retry_blocked && !status.control_pending);
    assert(atomic_load(&s_prepares) == 1 && !atomic_load(&s_manual_prepares));
    xiaotai_net_host_record_t calls[16];
    const size_t count = xiaotai_net_host_records(calls, 16);
    unsigned starts = 0;
    assert(count < 16);
    for (size_t i = 0; i < count; ++i) {
        assert(calls[i].call != XIAOTAI_NET_HOST_JOIN);
        starts += calls[i].call == XIAOTAI_NET_HOST_START;
    }
    assert(starts == 1);
    printf("driver start failure: restrict=%u, fault=%d, error=%d, starts=1, maintenance reusable; AT=%s",
           (unsigned)restrict_retry, status.prepare.fault, status.prepare.error, s_output);
    stop();
}

static void p4_retry_must_not_start(xiaotai_network_stage_t stage, xiaotai_net_fault_t fault)
{
    s_prepare_only = s_keep_fault = s_restrict_retry = true;
    s_first_prepare = stage;
    boot();
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    assert(status.prepare.fault == fault && status.prepare.error == failed_prepare(stage) &&
           status.prepare.retry_blocked);
    assert(atomic_load(&s_prepares) == 1 && !atomic_load(&s_manual_prepares));
    serial("AT+COMPANION RETRY\r", "ERROR: INVALID_STATE\r\n");
    assert(!strstr(s_output, "OK\r\n"));
    assert(xiaotai_net_retry() == XIAOTAI_ERR_INVALID_STATE);
    /* Exercise the worker guard for a request admitted before the result. */
    assert(net_post(retry_on_worker, 0, NULL, 0) == XIAOTAI_OK);
    settle();
    assert(atomic_load(&s_prepares) == 1 && !atomic_load(&s_manual_prepares));
    xiaotai_net_status(&status);
    assert(status.state == XIAOTAI_NET_BLOCKED && status.prepare.fault == fault &&
           status.prepare.error == failed_prepare(stage) && !status.control_pending);
    stop();
}

static void begin_activity(void *unused)
{
    (void)unused;
    const xiaotai_activity_t activity = {.name = "fixture", .resources = XIAOTAI_RESOURCE_AUDIO_OUT};
    assert(xiaotai_activity_begin(&activity, &s_activity) == XIAOTAI_OK);
}
static void end_activity(void *unused) { (void)unused; xiaotai_activity_end(s_activity); }
static void try_activity_during_update(void *unused)
{
    (void)unused;
    uint32_t id = 0;
    const xiaotai_activity_t activity = {.name = "fixture", .resources = XIAOTAI_RESOURCE_AUDIO_OUT};
    assert(xiaotai_activity_begin(&activity, &id) == XIAOTAI_ERR_BUSY && !id);
}
static void begin_quiesce(void *unused)
{
    (void)unused;
    assert(xiaotai_quiesce_begin("fixture", &s_other_quiesce) == XIAOTAI_OK);
}
static void end_quiesce(void *unused) { (void)unused; xiaotai_quiesce_end(s_other_quiesce); }

static void make_update_for(size_t digest_bytes, bool correct_digest)
{
    uint8_t digest[XIAOTAI_SHA256_BYTES];
    assert(digest_bytes <= sizeof(image) && xiaotai_sha256(image, digest_bytes, digest) == XIAOTAI_OK);
    if (!correct_digest) digest[sizeof(digest) - 1] ^= 1U;
    char hex[2 * XIAOTAI_SHA256_BYTES + 1];
    assert(xiaotai_at_hex_encode(digest, sizeof(digest), hex, sizeof(hex)) == sizeof(hex) - 1);
    (void)snprintf(update_command, sizeof(update_command), "AT+COMPANION UPDATE http://fixture.invalid/update %s\r", hex);
}

static void make_update(bool correct_digest) { make_update_for(sizeof(image), correct_digest); }

static void unexpected_reply(void *ctx, const char *text)
{
    (void)ctx;
    (void)text;
    assert(false);
}

static void basics(void)
{
    boot();
    serial("AT+COMPANION\r", "+COMPANION: project=fixture version=cached platform=sample protocol=-");
    serial("AT+COMPANION LIVE\r", "+COMPANION: project=fixture version=live platform=sample protocol=-");
    serial("AT+COMPANION SEND INFO\r", "+COMPANION-REPLY: text_hex=612062\r\nOK\r\n");
    serial("AT+COMPANION SEND ROLLBACK\r", "ERROR: NOT_ALLOWED\r\n");
    serial("AT+COMPANION RETRY\r", "ERROR: INVALID_STATE COMPANION_RETRY_UNAVAILABLE\r\n");
    serial("AT+COMPANION UPDATE https://fixture.invalid/image abc\r", "ERROR: INVALID_ARG\r\n");
    make_update(true);
    const xiaotai_reply_t other = {.write = unexpected_reply};
    update_command[strlen(update_command) - 1] = '\0';
    assert(xiaotai_command_run(update_command, &other, WAIT) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(xiaotai_command_run("AT+COMPANION ROLLBACK", &other, WAIT) == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(!xiaotai_quiesced() && !atomic_load(&s_quiesce_attempts) && !atomic_load(&s_downloads));
    make_update(true);
    assert(xiaotai_run(begin_activity, NULL) == XIAOTAI_OK);
    owner_sync();
    serial("AT+COMPANION ROLLBACK\r", "ERROR: BUSY ACTIVITY\r\n");
    serial(update_command, "ERROR: BUSY ACTIVITY\r\n");
    assert(xiaotai_activity_active() && atomic_load(&s_commands) == 1 && !atomic_load(&s_downloads));
    assert(xiaotai_run(end_activity, NULL) == XIAOTAI_OK && xiaotai_run(begin_quiesce, NULL) == XIAOTAI_OK);
    owner_sync();
    serial("AT+COMPANION ROLLBACK\r", "ERROR: BUSY ACTIVITY\r\n");
    serial(update_command, "ERROR: BUSY ACTIVITY\r\n");
    assert(xiaotai_quiesced());
    assert(xiaotai_run(end_quiesce, NULL) == XIAOTAI_OK);
    owner_sync();
    uint32_t ticket = 0;
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK);
    serial(update_command, "ERROR: INVALID_STATE MAINTENANCE\r\n");
    assert(!xiaotai_quiesced() && !atomic_load(&s_quiesce_attempts));
    xiaotai_net_maintenance_end(ticket);
    s_external = false;
    serial(update_command, "ERROR: NOT_SUPPORTED\r\n");
    s_external = true;
    stop();
}

static void maintenance_info_parity(bool enabled)
{
    s_maintenance_info = enabled;
    boot();
    make_update(true);
    uint32_t ticket = 0, denied = 0;
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK && ticket);
    serial(update_command, "ERROR: INVALID_STATE MAINTENANCE\r\n");
    serial("AT+COMPANION ROLLBACK\r", "ERROR: INVALID_STATE\r\n");
    serial("AT+COMPANION SEND RXRESET\r", "ERROR: INVALID_STATE\r\n");
    serial("AT+COMPANION SEND SINK1\r", "ERROR: INVALID_STATE\r\n");
    serial("AT+COMPANION LIVE\r", "ERROR: INVALID_STATE\r\n");
    settle();
    assert(!atomic_load(&s_commands) && !atomic_load(&s_downloads) &&
           !atomic_load(&s_updates) && !atomic_load(&s_restarts));
    char output[8] = {0};
    const xiaotai_err_t res = xiaotai_net_coprocessor_command("INFO", false, output, sizeof(output));
    printf("maintenance INFO option=%u result=%d peer_commands=%u; OTA=0 ROLLBACK=0\n",
           (unsigned)enabled, res, atomic_load(&s_commands));
    assert(net_gate_maintenance_held() && xiaotai_net_maintenance_begin(&denied) == XIAOTAI_ERR_INVALID_STATE && !denied);
    assert(res == (enabled ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE));
    assert(atomic_load(&s_commands) == (unsigned)enabled && !strcmp(output, enabled ? "a b" : ""));
    serial("AT+COMPANION SEND INFO\r", enabled ? "+COMPANION-REPLY: text_hex=612062\r\nOK\r\n" :
                                               "ERROR: INVALID_STATE\r\n");
    const unsigned held_commands = enabled ? 2U : 0U;
    assert(net_gate_maintenance_held() && atomic_load(&s_commands) == held_commands);
    xiaotai_net_maintenance_end(ticket);
    settle();
    serial("AT+COMPANION SEND INFO\r", "+COMPANION-REPLY: text_hex=612062\r\nOK\r\n");
    assert(!net_gate_maintenance_held() && atomic_load(&s_commands) == held_commands + 1U);

    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK);
    assert(xiaotai_net_maintenance_restart(ticket) == XIAOTAI_OK);
    serial("AT+COMPANION SEND INFO\r", "ERROR: INVALID_STATE\r\n");
    assert(net_gate_restart_pending() && atomic_load(&s_commands) == held_commands + 1U);
    stop();
}

static void roll(bool send_fail)
{
    boot();
    s_fail_output = send_fail;
    s_need_rollback_line = !send_fail;
    serial("AT+COMPANION ROLLBACK\r", send_fail ? "ERROR: FAIL OUTPUT\r\n" : rollback_line);
    if (send_fail) assert(atomic_load(&s_restarts) == 0 && !atomic_load(&s_rollback_written));
    assert(xiaotai_signal_take(&s_restarted, WAIT + XIAOTAI_NET_COPROCESSOR_REPLY_WAIT_MS));
    assert(atomic_load(&s_commands) == 1 && atomic_load(&s_restarts) == 1);
    stop();
}

static void wait_update_failure(void)
{
    for (unsigned i = 0; i < 100 && (!strstr(s_output, "stage=failed") || xiaotai_quiesced()); ++i)
        assert(xiaotai_at_service(10) == XIAOTAI_OK);
    assert(strstr(s_output, "stage=failed") && !xiaotai_quiesced() && !atomic_load(&s_restarts));
}

static void maintenance_info_during_transfer(void)
{
    s_maintenance_info = s_stress_notice = s_hook_fail = true;
    boot();
    make_update(true);
    xiaotai_http_host_set_responder(http_reply, NULL, false);
    atomic_store(&s_fail_update_task, true);
    serial(update_command, "ERROR: NO_MEM\r\n");
    settle();
    uint32_t ticket = 0;
    assert(!xiaotai_quiesced() && !atomic_load(&s_updates) && !atomic_load(&s_downloads));
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK && ticket);
    xiaotai_net_maintenance_end(ticket);
    settle();

    serial(update_command, "OK\r\n");
    const int64_t deadline = xiaotai_time_us() + WAIT * INT64_C(1000);
    while (!atomic_load(&s_progress_published) && xiaotai_time_us() < deadline) xiaotai_delay_ms(1);
    assert(atomic_load(&s_progress_published) && atomic_load(&s_updates) == 1);
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_ERR_INVALID_STATE && !ticket);
    assert(xiaotai_run(try_activity_during_update, NULL) == XIAOTAI_OK);
    owner_sync();
    serial("AT+COMPANION SEND INFO\r", "ERROR: INVALID_STATE\r\n");
    assert(!atomic_load(&s_commands) && xiaotai_quiesced());
    xiaotai_signal_give(&s_allow_update_return);
    wait_update_failure();
    settle();
    assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_OK && ticket);
    serial("AT+COMPANION SEND INFO\r", "+COMPANION-REPLY: text_hex=612062\r\nOK\r\n");
    assert(net_gate_maintenance_held() && atomic_load(&s_commands) == 1);
    xiaotai_net_maintenance_end(ticket);
    stop();
}

static void expect_update_failure(xiaotai_err_t error, bool verifying)
{
    wait_update_failure();
    char expected[96];
    (void)snprintf(expected, sizeof(expected), "+COMPANION-UPDATE: stage=failed percent=0 error=%d\r\n", error);
    const char *terminal = strstr(s_output, expected);
    if (!terminal) fprintf(stderr, "Response [%s], expected terminal [%s]\n", s_output, expected);
    assert(terminal && !strstr(terminal + strlen(expected), "stage=failed") && !strstr(s_output, update_line));
    assert((strstr(s_output, "stage=verifying") != NULL) == verifying);
}

static void failed(void)
{
    boot();
    s_hook_fail = true;
    serial("AT+COMPANION ROLLBACK\r", "ERROR: FAIL\r\n");
    settle();
    assert(!xiaotai_quiesced() && !atomic_load(&s_restarts));
    xiaotai_http_host_set_responder(http_reply, NULL, false);
    make_update(false);
    serial(update_command, "OK\r\n");
    _Static_assert(XIAOTAI_ERR_INVALID_CRC == 0x109, "legacy checksum error code");
    expect_update_failure(XIAOTAI_ERR_INVALID_CRC, true);
    assert(atomic_load(&s_downloads) == 1 && !atomic_load(&s_updates));
    make_update(true);
    s_fail_receive = true;
    serial(update_command, "OK\r\n");
    expect_update_failure(XIAOTAI_ERR_TIMEOUT, false);
    assert(atomic_load(&s_downloads) == 2 && !atomic_load(&s_updates));
    s_fail_receive = false;
    /* Even the correct digest of a short HTTP body cannot admit a partial image. */
    s_body_size = sizeof(image) - 1;
    make_update_for(s_body_size, true);
    serial(update_command, "OK\r\n");
    expect_update_failure(XIAOTAI_FAIL, false);
    assert(atomic_load(&s_downloads) == 3 && !atomic_load(&s_updates));
    /* The same prefix in a complete response instead fails verification of the full digest. */
    s_content_length = s_body_size;
    make_update(true);
    serial(update_command, "OK\r\n");
    expect_update_failure(XIAOTAI_ERR_INVALID_CRC, true);
    assert(atomic_load(&s_downloads) == 4 && !atomic_load(&s_updates));
    s_body_size = sizeof(image);
    s_content_length = XIAOTAI_NET_COPROCESSOR_IMAGE_MAX + 1;
    serial(update_command, "OK\r\n");
    expect_update_failure(XIAOTAI_ERR_INVALID_SIZE, false);
    assert(atomic_load(&s_downloads) == 5 && !atomic_load(&s_updates));
    s_content_length = sizeof(image);
    serial(update_command, "OK\r\n");
    expect_update_failure(XIAOTAI_FAIL, true);
    assert(atomic_load(&s_downloads) == 6 && atomic_load(&s_updates) == 1);
    stop();
}

static void fill_notice_ring(void)
{
    /* Prevent the port from draining this line until enqueueing the terminal
     * notice has hit the full-ring path at least once. */
    const int64_t deadline = xiaotai_time_us() + 500000;
    while (!atomic_load(&s_progress_published) && xiaotai_time_us() < deadline) xiaotai_delay_ms(1);
    assert(atomic_load(&s_progress_published));
    owner_sync();
    assert(xiaotai_at_service(15) == XIAOTAI_OK);
    char filler[257];
    memset(filler, 'x', sizeof(filler) - 1);
    memcpy(filler, "+FILL: ", sizeof("+FILL: ") - 1);
    filler[sizeof(filler) - 1] = '\0';
    assert(xiaotai_at_notify(filler) == XIAOTAI_OK);
    xiaotai_at_stats_t stats;
    xiaotai_at_stats(&stats);
    const uint32_t dropped_before = stats.notify_dropped;
    xiaotai_signal_give(&s_allow_update_return);
    const int64_t retry_deadline = xiaotai_time_us() + 500000;
    do {
        xiaotai_delay_ms(1);
        xiaotai_at_stats(&stats);
    } while (stats.notify_dropped == dropped_before && xiaotai_time_us() < retry_deadline);
    assert(stats.notify_dropped > dropped_before && !atomic_load(&s_restarts));
}

static void success(bool stress_notice)
{
    s_stress_notice = stress_notice;
    boot();
    make_update(true);
    s_need_update_line = true;
    xiaotai_http_host_set_responder(http_reply, NULL, false);
    serial(update_command, "OK\r\n");
    if (stress_notice) fill_notice_ring();
    for (unsigned i = 0; i < 100 && !atomic_load(&s_restarts); ++i)
        assert(xiaotai_at_service(10) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_restarted, WAIT));
    const char *accepted = strstr(s_output, "OK\r\n");
    const char *downloading = strstr(s_output, "stage=downloading");
    const char *verifying = strstr(s_output, "stage=verifying");
    const char *checking = strstr(s_output, "stage=checking");
    const char *writing = strstr(s_output, "stage=writing");
    const char *restarting = strstr(s_output, "stage=restarting");
    const char *terminal = strstr(s_output, update_line);
    assert(accepted && terminal && accepted < terminal && !strstr(terminal + 1, update_line));
    if (!stress_notice) {
        assert(downloading && verifying && checking && writing && restarting);
        assert(accepted < downloading && downloading < verifying && verifying < checking && checking < writing &&
               writing < restarting && restarting < terminal);
        assert(!strstr(writing + 1, "stage=writing"));
    }
    assert(atomic_load(&s_downloads) == 1 && atomic_load(&s_updates) == 1 && atomic_load(&s_restarts) == 1);
    stop();
}

static void failed_queue(void)
{
    s_stress_notice = s_hook_fail = true;
    boot();
    make_update(true);
    xiaotai_http_host_set_responder(http_reply, NULL, false);
    serial(update_command, "OK\r\n");
    fill_notice_ring();
    wait_update_failure();
    const char *failure = strstr(s_output, "stage=failed percent=0 error=-1\r\n");
    assert(failure && !strstr(failure + 1, "stage=failed") && !strstr(s_output, update_line));
    assert(atomic_load(&s_updates) == 1);
    stop();
}

int main(int argc, char **argv)
{
    assert(argc <= 2);
    if (argc == 1) basics();
    else if (!strcmp(argv[1], "maintenance_info_parity")) maintenance_info_parity(true);
    else if (!strcmp(argv[1], "maintenance_info_default")) maintenance_info_parity(false);
    else if (!strcmp(argv[1], "maintenance_info_during_transfer")) maintenance_info_during_transfer();
    else if (!strcmp(argv[1], "rollback")) roll(false);
    else if (!strcmp(argv[1], "offline")) roll(true);
    else if (!strcmp(argv[1], "failed")) failed();
    else if (!strcmp(argv[1], "success")) success(false);
    else if (!strcmp(argv[1], "queue")) success(true);
    else if (!strcmp(argv[1], "failed_queue")) failed_queue();
    else if (!strcmp(argv[1], "p4_cached_identity")) p4_cached_identity();
    else if (!strcmp(argv[1], "p4_driver_start_failure")) driver_start_failure(true);
    else if (!strcmp(argv[1], "wt99_driver_start_failure")) driver_start_failure(false);
    else if (!strcmp(argv[1], "p4_retry_no_link"))
        retry_history(XIAOTAI_NETWORK_NO_LINK, XIAOTAI_NET_FAULT_NO_LINK, true);
    else if (!strcmp(argv[1], "p4_retry_no_link_not_timeout")) {
        s_no_link_error = XIAOTAI_ERR_INVALID_RESPONSE;
        retry_history(XIAOTAI_NETWORK_NO_LINK, XIAOTAI_NET_FAULT_NO_LINK, true);
    } else if (!strcmp(argv[1], "p4_retry_unfinished"))
        retry_history(XIAOTAI_NETWORK_UNFINISHED, XIAOTAI_NET_FAULT_INTERRUPTED, true);
    else if (!strcmp(argv[1], "p4_retry_bad_image"))
        p4_retry_must_not_start(XIAOTAI_NETWORK_BAD_IMAGE, XIAOTAI_NET_FAULT_BAD_IMAGE);
    else if (!strcmp(argv[1], "p4_retry_unsupported"))
        p4_retry_must_not_start(XIAOTAI_NETWORK_UNSUPPORTED, XIAOTAI_NET_FAULT_UNSUPPORTED);
    else if (!strcmp(argv[1], "p4_retry_power_cycle"))
        p4_retry_must_not_start(XIAOTAI_NETWORK_POWER_CYCLE, XIAOTAI_NET_FAULT_POWER_CYCLE);
    else if (!strcmp(argv[1], "p4_retry_unknown"))
        p4_retry_must_not_start(XIAOTAI_NETWORK_CHECKING, XIAOTAI_NET_FAULT_ERROR);
    else if (!strcmp(argv[1], "retry_default_unsupported"))
        retry_history(XIAOTAI_NETWORK_UNSUPPORTED, XIAOTAI_NET_FAULT_UNSUPPORTED, false);
    else if (!strcmp(argv[1], "unsupported")) {
        s_no_hooks = true;
        boot();
        serial("AT+COMPANION\r", "ERROR: NOT_FOUND UNKNOWN_COMMAND\r\n");
        stop();
    } else assert(false);
    return 0;
}
