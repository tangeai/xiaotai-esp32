// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_c61_ota_image_admission.py
// Replaces legacy: tests/regression/test_companion_maintenance.py
// Replaces legacy: tests/regression/test_p4_companion_identity.py
// Replaces legacy: tests/regression/test_c6_ota_recovery.py
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "xiaotai/at.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/kv.h"
#include "xiaotai_net_host.h"

#define WAIT_MS 3000U

static _Atomic int64_t s_now_us, s_restarted_us;
static atomic_uint s_live_calls, s_cache_calls, s_command_calls, s_done, s_prepare_calls;
static atomic_uint s_update_calls;
static xiaotai_signal_t s_owner, s_restarted, s_finished, s_writing, s_release;
static enum { NORMAL, PARTIAL, NO_HOOKS, PREPARE_BLOCKED, PREPARE_RESTART } s_boot;
static enum { BAD_IMAGE, BEGIN_FAIL, WRITE_FAIL, WRITE_OK } s_write;
static bool s_command_fail, s_expect_done, s_hold_write;
static const uint8_t s_image[] = {0x12, 0xab, 0xef};

int64_t __wrap_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void) { return atomic_load(&s_now_us); }

static xiaotai_err_t prepare(void *self, bool manual, xiaotai_network_progress_fn_t progress, void *ctx)
{
    (void)self;
    assert(net_on_worker() && net_gate_bringup());
    assert(manual == (atomic_fetch_add(&s_prepare_calls, 1) > 0));
    assert(net_gate_coprocessor_begin() == XIAOTAI_ERR_INVALID_STATE);
    progress(XIAOTAI_NETWORK_CHECKING, 0, ctx);
    if (!manual && s_boot == PREPARE_BLOCKED) {
        progress(XIAOTAI_NETWORK_NO_LINK, 0, ctx);
        return XIAOTAI_ERR_TIMEOUT;
    }
    progress(s_boot == PREPARE_RESTART ? XIAOTAI_NETWORK_RESTARTING : XIAOTAI_NETWORK_READY, 100, ctx);
    return XIAOTAI_OK;
}

static xiaotai_err_t identity(void *self, bool live, xiaotai_coprocessor_info_t *info)
{
    (void)self;
    assert(net_on_worker());
    if (live) atomic_fetch_add(&s_live_calls, 1);
    else atomic_fetch_add(&s_cache_calls, 1);
    const char *version = live ? "live-tag" : "cached-tag";
    strcpy(info->project, "fixture-link");
    strcpy(info->version, version);
    strcpy(info->platform, "test-build");
    strcpy(info->protocol, "1.1.0");
    memset(info->elf_sha256, 'a', sizeof(info->elf_sha256) - 1);
    info->elf_sha256[sizeof(info->elf_sha256) - 1] = '\0';
    if (s_boot == PARTIAL) info->platform[0] = 0;
    return XIAOTAI_OK;
}

static xiaotai_err_t command(void *self, const char *text, char *reply, size_t capacity)
{
    (void)self;
    assert(net_on_worker() && !xiaotai_on_owner());
    assert(!strcmp(text, "VERSION") || !strcmp(text, "ROLLBACK") || !strcmp(text, "RXRESET") || !strcmp(text, "INFO"));
    atomic_fetch_add(&s_command_calls, 1);
    assert(net_gate_config_begin() == XIAOTAI_ERR_INVALID_STATE);
    if (s_command_fail) return XIAOTAI_FAIL;
    (void)snprintf(reply, capacity, "firmware-version");
    return XIAOTAI_OK;
}

static xiaotai_err_t update(void *self, const uint8_t *data, size_t size,
                            xiaotai_network_progress_fn_t progress, void *ctx)
{
    (void)self;
    assert(net_on_worker() && data == s_image && size == sizeof(s_image));
    assert(net_gate_config_begin() == XIAOTAI_ERR_INVALID_STATE);
    atomic_fetch_add(&s_update_calls, 1);
    progress(XIAOTAI_NETWORK_VERIFYING, 0, ctx);
    if (s_write == BAD_IMAGE) {
        if (s_hold_write) {
            xiaotai_signal_give(&s_writing);
            assert(xiaotai_signal_take(&s_release, WAIT_MS));
        }
        progress(XIAOTAI_NETWORK_BAD_IMAGE, 0, ctx);
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (s_write == BEGIN_FAIL) {
        progress(XIAOTAI_NETWORK_POWER_CYCLE, 0, ctx);
        return XIAOTAI_ERR_TIMEOUT;
    }
    progress(XIAOTAI_NETWORK_WRITING, 21, ctx);
    if (s_hold_write) {
        xiaotai_signal_give(&s_writing);
        assert(xiaotai_signal_take(&s_release, WAIT_MS));
    }
    if (s_write == WRITE_FAIL) {
        progress(XIAOTAI_NETWORK_POWER_CYCLE, 21, ctx);
        return XIAOTAI_ERR_TIMEOUT;
    }
    progress(XIAOTAI_NETWORK_ACTIVATING, 100, ctx);
    progress(XIAOTAI_NETWORK_RESTARTING, 100, ctx);
    return XIAOTAI_OK;
}

const xiaotai_network_t *xiaotai_hal_network(size_t index)
{
    static const xiaotai_network_t companion = {
        .desc = {.kind = XIAOTAI_NETWORK_WIFI_COPROCESSOR},
        .prepare = prepare, .coprocessor_info = identity, .coprocessor_update = update, .coprocessor_command = command,
    };
    static const xiaotai_network_t plain = {.desc = {.kind = XIAOTAI_NETWORK_WIFI}};
    return index ? NULL : s_boot == NO_HOOKS ? &plain : &companion;
}

static void restarted(void)
{
    assert(net_on_worker() && !xiaotai_on_owner() && !atomic_load(&s_restarted_us));
    if (s_expect_done) assert(atomic_load(&s_done) == 1);
    if (s_boot == PREPARE_RESTART) {
        uint32_t ticket = 0;
        assert(net_gate_bringup());
        assert(xiaotai_net_maintenance_begin(&ticket) == XIAOTAI_ERR_INVALID_STATE && !ticket);
        assert(xiaotai_net_retry() == XIAOTAI_ERR_INVALID_STATE);
        puts("prepare restart: bring-up gate still held; maintenance and retry refused before reboot");
    }
    atomic_store(&s_restarted_us, xiaotai_time_us());
    xiaotai_signal_give(&s_restarted);
}

static void finished(xiaotai_err_t result, void *ctx)
{
    assert(net_on_worker() && !atomic_load(&s_restarted_us));
    const xiaotai_err_t expected = *(const xiaotai_err_t *)ctx;
    assert(result == expected);
    if (result == XIAOTAI_OK) {
        assert(net_gate_restart_pending() && net_gate_coprocessor_begin() == XIAOTAI_ERR_INVALID_STATE);
    } else {
        /* Completion can hand the failure to a caller starting maintenance. */
        assert(!net_gate_restart_pending() && net_gate_config_begin() == XIAOTAI_OK);
        net_gate_config_end();
    }
    atomic_fetch_add(&s_done, 1);
    xiaotai_signal_give(&s_finished);
}

static void give(void *ctx) { xiaotai_signal_give(ctx); }

static void sync_worker(void)
{
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
}

static void boot(void)
{
    static const xiaotai_module_t net = {.name = "net", .init = xiaotai_engine_net_init,
                                        .start = xiaotai_engine_net_start};
    const xiaotai_module_t *const modules[] = {&net};
    xiaotai_net_host_set_restart(restarted);
    atomic_store(&s_now_us, 1000000);
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = modules, .module_count = 1}) == XIAOTAI_OK);
    assert(xiaotai_run(give, &s_owner) == XIAOTAI_OK && xiaotai_signal_take(&s_owner, WAIT_MS));
    if (s_boot != PREPARE_RESTART) sync_worker();
}

static xiaotai_net_status_t snapshot(void)
{
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    return status;
}

static uint32_t connect_fixture(void)
{
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
    sync_worker();
    assert(snapshot().state == XIAOTAI_NET_UP && xiaotai_net_generation());
    return token;
}

static void info(void)
{
    xiaotai_coprocessor_info_t out = {0};
    if (s_boot == PREPARE_BLOCKED) {
        assert(snapshot().state == XIAOTAI_NET_BLOCKED && snapshot().prepare.fault == XIAOTAI_NET_FAULT_NO_LINK);
        assert(xiaotai_net_retry() == XIAOTAI_OK);
        sync_worker();
        assert(snapshot().state == XIAOTAI_NET_UNCONFIGURED && atomic_load(&s_prepare_calls) == 2);
    }
    const unsigned cached = atomic_load(&s_cache_calls);
    assert(cached == (s_boot == PREPARE_BLOCKED ? 2U : 1U));
    const xiaotai_err_t expected = s_boot == PARTIAL ? XIAOTAI_ERR_NOT_FOUND : XIAOTAI_OK;
    assert(xiaotai_net_coprocessor_info(false, &out) == expected);
    assert(atomic_load(&s_cache_calls) == cached && !atomic_load(&s_live_calls));
    if (expected == XIAOTAI_OK) assert(!strcmp(out.version, "cached-tag"));
    assert(xiaotai_net_coprocessor_info(true, &out) == expected && atomic_load(&s_live_calls) == 1);
    if (expected == XIAOTAI_OK) assert(!strcmp(out.version, "live-tag") && strlen(out.elf_sha256) == 64);
}

static void rollback(bool release_early)
{
    char reply[5];
    assert(xiaotai_net_coprocessor_command("VERSION", false, reply, sizeof(reply)) == XIAOTAI_OK);
    assert(!strcmp(reply, "firm"));
    xiaotai_net_coprocessor_restart_now();
    sync_worker();
    assert(!atomic_load(&s_restarted_us));
    s_command_fail = true;
    assert(xiaotai_net_coprocessor_command("ROLLBACK", true, reply, sizeof(reply)) == XIAOTAI_FAIL);
    assert(!net_gate_restart_pending());
    s_command_fail = false;
    assert(xiaotai_net_coprocessor_command("ROLLBACK", true, reply, sizeof(reply)) == XIAOTAI_OK);
    assert(!strcmp(reply, "firm") && atomic_load(&s_command_calls) == 3);
    const int64_t handback = xiaotai_time_us();
    xiaotai_coprocessor_info_t live;
    uint32_t lease = 1;
    assert(net_gate_restart_pending());
    assert(xiaotai_net_maintenance_begin(&lease) == XIAOTAI_ERR_INVALID_STATE && !lease);
    assert(xiaotai_net_coprocessor_info(true, &live) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_net_coprocessor_command("VERSION", false, NULL, 0) == XIAOTAI_ERR_INVALID_STATE);
    if (release_early) {
        xiaotai_net_coprocessor_restart_now();
        assert(xiaotai_signal_take(&s_restarted, WAIT_MS) && atomic_load(&s_restarted_us) == handback);
    } else {
        const int64_t due = handback + (int64_t)XIAOTAI_NET_COPROCESSOR_REPLY_WAIT_MS * 1000;
        atomic_store(&s_now_us, due - 1000);
        sync_worker();
        assert(!atomic_load(&s_restarted_us));
        atomic_store(&s_now_us, due);
        assert(xiaotai_signal_take(&s_restarted, WAIT_MS) && atomic_load(&s_restarted_us) == due);
    }
}

static void write_case(void)
{
    const bool power_cycle = s_write == BEGIN_FAIL || s_write == WRITE_FAIL;
    const uint32_t retired = power_cycle ? connect_fixture() : 0;
    const xiaotai_err_t expected = s_write == WRITE_OK ? XIAOTAI_OK : s_write == BAD_IMAGE ?
                                   XIAOTAI_ERR_INVALID_ARG : XIAOTAI_ERR_TIMEOUT;
    xiaotai_coprocessor_info_t original;
    assert(xiaotai_net_coprocessor_info(false, &original) == XIAOTAI_OK);
    assert(xiaotai_net_coprocessor_update(NULL, 0, finished, (void *)&expected) == XIAOTAI_ERR_INVALID_ARG);
    uint32_t lease;
    assert(xiaotai_net_maintenance_begin(&lease) == XIAOTAI_OK);
    assert(xiaotai_net_coprocessor_update(s_image, sizeof(s_image), finished, (void *)&expected) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_net_maintenance_end(lease);
    s_expect_done = s_write == WRITE_OK;
    s_hold_write = s_write == WRITE_FAIL || s_write == WRITE_OK;
    assert(xiaotai_net_coprocessor_update(s_image, sizeof(s_image), finished, (void *)&expected) == XIAOTAI_OK);
    if (s_hold_write) {
        assert(xiaotai_signal_take(&s_writing, WAIT_MS));
        xiaotai_coprocessor_info_t identity;
        assert(snapshot().prepare.percent == 21 && snapshot().prepare.stage == XIAOTAI_NET_PREPARE_WRITING);
        assert(xiaotai_net_coprocessor_info(false, &identity) == XIAOTAI_ERR_NOT_FOUND);
        assert(xiaotai_net_coprocessor_command("VERSION", false, NULL, 0) == XIAOTAI_ERR_INVALID_STATE);
        xiaotai_signal_give(&s_release);
    }
    assert(xiaotai_signal_take(&s_finished, WAIT_MS) && atomic_load(&s_done) == 1);
    if (s_write == WRITE_OK) {
        assert(xiaotai_signal_take(&s_restarted, WAIT_MS));
    } else {
        assert(!atomic_load(&s_restarted_us));
        assert(snapshot().prepare.fault == (s_write == BAD_IMAGE ? XIAOTAI_NET_FAULT_BAD_IMAGE : XIAOTAI_NET_FAULT_POWER_CYCLE));
        assert(snapshot().state == (s_write == BAD_IMAGE ? XIAOTAI_NET_UNCONFIGURED : XIAOTAI_NET_BLOCKED));
        xiaotai_coprocessor_info_t identity;
        assert(xiaotai_net_coprocessor_info(false, &identity) == (power_cycle ? XIAOTAI_ERR_NOT_FOUND : XIAOTAI_OK));
        if (retired) {
            const xiaotai_err_t reset = xiaotai_net_coprocessor_command("RXRESET", false, NULL, 0);
            printf("power-cycle latch: RXRESET result=%d peer_commands=%u\n", reset,
                   atomic_load(&s_command_calls));
            fflush(stdout);
            assert(reset == XIAOTAI_ERR_INVALID_STATE && !atomic_load(&s_command_calls));
            assert(xiaotai_net_coprocessor_command("ROLLBACK", true, NULL, 0) == XIAOTAI_ERR_INVALID_STATE);
            assert(xiaotai_net_coprocessor_update(s_image, sizeof(s_image), finished, (void *)&expected) ==
                   XIAOTAI_ERR_INVALID_STATE && atomic_load(&s_update_calls) == 1);
            assert(xiaotai_net_retry() == XIAOTAI_ERR_INVALID_STATE);
            assert(xiaotai_net_coprocessor_command("INFO", false, NULL, 0) == XIAOTAI_OK &&
                   atomic_load(&s_command_calls) == 1);
            assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){
                .kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = retired, .ipv4 = 43,
            }) == XIAOTAI_OK);
            sync_worker();
            assert(snapshot().state == XIAOTAI_NET_BLOCKED && !xiaotai_net_generation());
            assert(snapshot().ipv4 == 0);
        }
    }
}

static void hold_worker(const net_message_t *message)
{
    (void)message;
    xiaotai_signal_give(&s_writing);
    assert(xiaotai_signal_take(&s_release, WAIT_MS));
}

static void no_op(const net_message_t *message) { (void)message; }

static void queue_drained(const net_message_t *message)
{
    (void)message;
    xiaotai_signal_give(&s_writing);
}

static void rejected_writes(const xiaotai_err_t *expected)
{
    assert(xiaotai_net_coprocessor_update(s_image, sizeof(s_image), finished, (void *)expected) ==
           XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_net_coprocessor_command("ROLLBACK", true, NULL, 0) == XIAOTAI_ERR_INVALID_STATE);
    assert(!atomic_load(&s_update_calls) && !atomic_load(&s_command_calls));
}

static void write_admission(bool blocked)
{
    const xiaotai_err_t expected = XIAOTAI_ERR_INVALID_ARG;
    if (blocked) {
        assert(snapshot().state == XIAOTAI_NET_BLOCKED);
        rejected_writes(&expected);
        info(); /* Read-only diagnostics and manual bring-up still work. */
        return;
    }

    /* A failed queue append must hand both kinds of reservation back. */
    assert(net_post(hold_worker, 0, NULL, 0) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_writing, WAIT_MS));
    for (unsigned i = 1; i < XIAOTAI_NET_QUEUE_DEPTH; ++i)
        assert(net_post(no_op, 0, NULL, 0) == XIAOTAI_OK);
    assert(net_post(queue_drained, 0, NULL, 0) == XIAOTAI_OK);
    assert(xiaotai_net_scan() == XIAOTAI_ERR_TIMEOUT);
    assert(xiaotai_net_coprocessor_update(s_image, sizeof(s_image), finished, (void *)&expected) ==
           XIAOTAI_ERR_TIMEOUT);
    xiaotai_signal_give(&s_release);
    assert(xiaotai_signal_take(&s_writing, WAIT_MS));

    assert(net_post(hold_worker, 0, NULL, 0) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_writing, WAIT_MS));
    assert(xiaotai_net_scan() == XIAOTAI_OK);
    assert(xiaotai_net_coprocessor_update(s_image, sizeof(s_image), finished, (void *)&expected) ==
           XIAOTAI_ERR_INVALID_STATE);
    xiaotai_signal_give(&s_release);
    sync_worker();

    xiaotai_net_host_record_t records[16];
    const size_t count = xiaotai_net_host_records(records, 16);
    uint32_t scan_token = 0;
    for (size_t i = 0; i < count; ++i) {
        if (records[i].call == XIAOTAI_NET_HOST_SCAN) scan_token = records[i].token;
    }
    assert(scan_token);
    rejected_writes(&expected);
    xiaotai_coprocessor_info_t identity;
    assert(xiaotai_net_coprocessor_info(true, &identity) == XIAOTAI_OK);
    assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){
        .kind = XIAOTAI_NET_DRIVER_SCAN_DONE, .token = scan_token + 1,
    }) == XIAOTAI_OK);
    sync_worker();
    rejected_writes(&expected);
    assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){
        .kind = XIAOTAI_NET_DRIVER_SCAN_DONE, .token = scan_token,
    }) == XIAOTAI_OK);
    sync_worker();

    atomic_fetch_add(&s_now_us, ((int64_t)XIAOTAI_NET_SCAN_INTERVAL_MS + 1) * 1000);
    s_hold_write = true;
    assert(xiaotai_net_coprocessor_update(s_image, sizeof(s_image), finished, (void *)&expected) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&s_writing, WAIT_MS));
    assert(xiaotai_net_scan() == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_signal_give(&s_release);
    assert(xiaotai_signal_take(&s_finished, WAIT_MS) && atomic_load(&s_update_calls) == 1);
    assert(xiaotai_net_scan() == XIAOTAI_OK);
    assert(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_SCAN, 2, WAIT_MS));
}

int main(int argc, char **argv)
{
    const char *scenario = argc == 2 ? argv[1] : "identity";
    xiaotai_signal_t *const signals[] = {&s_owner, &s_restarted, &s_finished, &s_writing, &s_release};
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) assert(xiaotai_signal_init(signals[i]) == XIAOTAI_OK);
    xiaotai_coprocessor_info_t out;
    assert(xiaotai_net_coprocessor_info(false, &out) == XIAOTAI_ERR_INVALID_STATE);
    if (!strcmp(scenario, "partial")) s_boot = PARTIAL;
    if (!strcmp(scenario, "unavailable")) s_boot = NO_HOOKS;
    if (!strcmp(scenario, "prepare_blocked") || !strcmp(scenario, "admission_blocked")) s_boot = PREPARE_BLOCKED;
    if (!strcmp(scenario, "prepare_restart")) s_boot = PREPARE_RESTART;
    if (!strcmp(scenario, "update_fault") || !strcmp(scenario, "update_begin_fault")) {
        uint8_t record[99] = {1};
        memcpy(record + 1, "fixture", sizeof("fixture"));
        assert(xiaotai_kv_init() == XIAOTAI_OK);
        assert(xiaotai_kv_set("wifi_cfg", "credentials", record, sizeof(record), WAIT_MS) == XIAOTAI_OK);
    }
    boot();
    if (s_boot == PREPARE_RESTART) {
        assert(xiaotai_signal_take(&s_restarted, WAIT_MS));
        assert(snapshot().prepare.stage == XIAOTAI_NET_PREPARE_RESTARTING);
    } else if (s_boot == NO_HOOKS) {
        assert(xiaotai_net_coprocessor_info(false, &out) == XIAOTAI_ERR_NOT_SUPPORTED);
        assert(xiaotai_net_coprocessor_command("VERSION", false, NULL, 0) == XIAOTAI_ERR_NOT_SUPPORTED);
    } else if (!strncmp(scenario, "rollback_", 9)) {
        rollback(!strcmp(scenario, "rollback_release"));
    } else if (!strncmp(scenario, "admission_", 10)) {
        write_admission(s_boot == PREPARE_BLOCKED);
    } else if (!strncmp(scenario, "update_", 7)) {
        s_write = !strcmp(scenario, "update_ok") ? WRITE_OK :
                  !strcmp(scenario, "update_fault") ? WRITE_FAIL :
                  !strcmp(scenario, "update_begin_fault") ? BEGIN_FAIL : BAD_IMAGE;
        write_case();
    } else {
        info();
    }
    xiaotai_net_reset();
    xiaotai_framework_reset();
    xiaotai_at_reset();
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); ++i) xiaotai_signal_deinit(signals[i]);
    printf("net_companion %s: ok\n", scenario);
    return 0;
}
