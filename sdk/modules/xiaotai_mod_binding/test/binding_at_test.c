// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_platform_control.c::platform_control
// Replaces legacy: tests/regression/test_binding_lifecycle.py
#include "binding_internal.h"
#include "fake_platform.h"
#include "internal.h" /* real AT verb table, also used by HELP */
#include "manifest_commands.h"
#include "xiaotai/at.h"
#include "xiaotai/modules/binding_config.h"
#include "xiaotai/port/os.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static unsigned quiet_on, quiet_off, forgot, starting, halts, wipes;
static atomic_uint restarted;
static xiaotai_err_t result, outcome;
static uint32_t ticket;
static bool voice_active, allow_stream, timeout, direct_path;
static unsigned reset_completions, revoked;
static xiaotai_err_t reset_result;
static int stream_cookie;
static xiaotai_at_stream_t handed;
static xiaotai_err_t stream_error;
static const char *stream_reason;
static char output[1800];

static void restart(void) { atomic_fetch_add(&restarted, 1); }

static void observe_event(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (strcmp(event->name, XIAOTAI_EVENT_QUIESCE) == 0) {
        if (event->args.value) {
            assert(strcmp(event->args.text, XIAOTAI_QUIESCE_BINDING) == 0 ||
                   strcmp(event->args.text, "maintenance") == 0);
            ++quiet_on;
        } else {
            ++quiet_off;
        }
    } else if (strcmp(event->name, XIAOTAI_EVENT_BINDING_FORGOTTEN) == 0) {
        assert(!event->data && !event->size && !event->args.text[0]);
        outcome = event->args.value;
        ++forgot;
    } else {
        assert(strcmp(event->args.text, XIAOTAI_QUIESCE_BINDING) == 0 && xiaotai_quiesced() != direct_path);
        ++starting;
    }
}

static void halt_voice(void)
{
    if (!voice_active) return;
    assert(!g_identity.erases && xiaotai_quiesced());
    voice_active = false;
    ++halts;
}
static bool voice_is_active(void) { return voice_active; }

static xiaotai_err_t observe(void)
{
    static const binding_voice_t voice = {.active = voice_is_active, .halt = halt_voice};
    binding_voice_attach(&voice);
    assert(xiaotai_event_subscribe(XIAOTAI_EVENT_QUIESCE, observe_event, NULL) == XIAOTAI_OK);
    assert(xiaotai_event_subscribe(XIAOTAI_EVENT_BINDING_FORGOTTEN, observe_event, NULL) == XIAOTAI_OK);
    return xiaotai_event_subscribe(XIAOTAI_EVENT_RESTARTING, observe_event, NULL);
}

static const xiaotai_module_t module = {
    .name = "binding", .init = xiaotai_module_binding_init, .start = xiaotai_module_binding_start,
};
static const char *const depends[] = {"binding", NULL};
static const xiaotai_module_t observer = {.name = "observer", .depends = depends, .init = observe};
static const xiaotai_module_t *const modules[] = {&module, &observer};

static void boot(void)
{
    fake_reset();
    fake_clock_start();
    fake_identity_set("device-old", "secret-never-output");
    quiet_on = quiet_off = forgot = starting = halts = wipes = 0;
    atomic_store(&restarted, 0);
    voice_active = timeout = direct_path = false;
    reset_completions = revoked = 0;
    allow_stream = true;
    const xiaotai_framework_config_t config = {
        .modules = modules, .module_count = 2, .available_needs = UINT32_MAX, .restart_delay_ms = 500, .restart = restart,
    };
    assert(xiaotai_framework_start(&config) == XIAOTAI_OK);
    fake_drain_events();
}

static void shutdown(void)
{
    assert(!handed.next && !g_identity.erase_pending && !g_identity.save_pending && !voice_active);
    xiaotai_framework_reset();
    xiaotai_binding_reset();
    xiaotai_at_reset();
}

static void write_line(void *ctx, const char *line)
{
    (void)ctx;
    assert(line && !strchr(line, '\r') && !strchr(line, '\n'));
    const size_t used = strlen(output), size = strlen(line);
    assert(used + size + 2 < sizeof(output));
    memcpy(output + used, line, size);
    output[used + size] = '\n';
    output[used + size + 1] = '\0';
}

static const xiaotai_reply_t reply = {.write = write_line, .ctx = &stream_cookie};

xiaotai_err_t __wrap_xiaotai_at_stream(const xiaotai_reply_t *out, const xiaotai_at_stream_t *stream);
xiaotai_err_t __wrap_xiaotai_at_stream(const xiaotai_reply_t *out, const xiaotai_at_stream_t *stream)
{
    if (!allow_stream || at_reply_original(out)->ctx != &stream_cookie) return XIAOTAI_ERR_NOT_SUPPORTED;
    assert(!handed.next);
    handed = *stream;
    return XIAOTAI_OK;
}

xiaotai_err_t __wrap_xiaotai_at_fail(const xiaotai_reply_t *out, xiaotai_err_t error, const char *reason)
{
    (void)out;
    stream_error = error;
    stream_reason = reason;
    return error;
}

void __real_xiaotai_identity_wipe(void *p, size_t size);
void __wrap_xiaotai_identity_wipe(void *p, size_t size)
{
    const bool watch = size == sizeof(xiaotai_identity_t) && ((xiaotai_identity_t *)p)->device_key[0] == '~';
    __real_xiaotai_identity_wipe(p, size);
    if (watch) {
        const uint8_t *bytes = p;
        for (size_t i = 0; i < size; ++i) assert(!bytes[i]);
        ++wipes;
    }
}

bool __real_xiaotai_signal_take(xiaotai_signal_t *signal, uint32_t timeout_ms);
bool __wrap_xiaotai_signal_take(xiaotai_signal_t *signal, uint32_t timeout_ms)
{
    if (timeout && timeout_ms == 10000) return false;
    return __real_xiaotai_signal_take(signal, timeout_ms);
}

static xiaotai_err_t command(const char *line)
{
    assert(!handed.next);
    output[0] = '\0';
    const xiaotai_err_t err = xiaotai_command_run(line, &reply, 1000);
    if (err != XIAOTAI_OK && handed.next) {
        handed.release(handed.ctx);
        handed = (xiaotai_at_stream_t){0};
    }
    fake_drain_events();
    return err;
}

static xiaotai_err_t stream_end(void)
{
    assert(handed.next && handed.release);
    stream_error = XIAOTAI_OK;
    stream_reason = NULL;
    assert(!handed.next(handed.ctx, &reply));
    handed.release(handed.ctx);
    handed = (xiaotai_at_stream_t){0};
    return stream_error;
}

static void diag(void *ctx) { (void)ctx; xiaotai_diag_report("binding", &reply); }
static void do_forget(void *ctx) { (void)ctx; result = xiaotai_binding_forget(); }
static void do_quiet(void *ctx) { (void)ctx; assert(xiaotai_quiesce_begin("maintenance", &ticket) == XIAOTAI_OK); }
static void end_quiet(void *ctx) { (void)ctx; xiaotai_quiesce_end(ticket); }
static void unused(void *ctx) { (void)ctx; }

static void fill_timers(void *ctx)
{
    (void)ctx;
    uint32_t id;
    while (xiaotai_timer_start(60000, unused, NULL, &id) == XIAOTAI_OK) {}
}

static void check_verb(const xiaotai_at_verb_t *verb, void *ctx)
{
    bool found = false;
    for (size_t i = 0; i < sizeof(manifest_commands) / sizeof(manifest_commands[0]); ++i) {
        const bool enabled = !manifest_commands[i].option[0] ||
            (XIAOTAI_BINDING_DEV_COMMANDS && strcmp(manifest_commands[i].option, "XIAOTAI_BINDING_DEV_COMMANDS") == 0);
        if (enabled && strcmp(manifest_commands[i].command, "AT+BIND") == 0 &&
            strcmp(manifest_commands[i].verb, verb->word ? verb->word : "") == 0 &&
            strcmp(manifest_commands[i].usage, verb->usage ? verb->usage : "") == 0) found = true;
    }
    assert(found);
    ++*(unsigned *)ctx;
}

static void base_at(void)
{
    unsigned count = 0;
    assert(at_verbs_visit("AT+BIND", check_verb, &count));
    assert(count == (XIAOTAI_BINDING_DEV_COMMANDS ? 5U : 3U));
    assert(command("AT+BIND") == XIAOTAI_OK);
    assert(strstr(output, "+BIND: phase=") && strstr(output, "verification=-") &&
           strstr(output, "device=device-old client=test-aabbccddee01 gen=1"));
    assert(!strstr(output, "secret-never-output"));
    strcpy(g_identity.identity.client_id, "literal.v3");
    assert(command("AT+BIND") == XIAOTAI_OK && strstr(output, "client=literal.v3"));
    strcpy(g_identity.identity.client_id, "untrusted\r\nOK");
    assert(command("AT+BIND") == XIAOTAI_OK && strstr(output, "client=-") && !strstr(output, "\nOK"));
    output[0] = '\0';
    fake_on_owner(diag, NULL);
    assert(strstr(output, "phase=") && strstr(output, "announcements=0") && strstr(output, "forget_failures=0"));
    assert(!strstr(output, "secret-never-output") && !strstr(output, "device-old") && !strstr(output, "untrusted"));
    assert(command("AT+BIND RETRY") == XIAOTAI_OK && g_platform_retries == 1);
    assert(command("AT+BIND REPLAY") == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(command("AT+BIND RETRY EXTRA") == XIAOTAI_ERR_INVALID_ARG && g_platform_retries == 1);
}

static void waiting(void)
{
    assert(xiaotai_event_emit(XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED, NULL, NULL, 0) == XIAOTAI_OK);
    fake_drain_events();
    fake_reply(200, 200, fake_report_body("123456", "temp-not-output", "guest-client"));
    fake_guest_ready();
    assert(g_guest.open);
}

static void test_forget(void)
{
    boot();
    waiting();
    voice_active = true;
    fake_on_owner(do_forget, NULL);
    assert(result == XIAOTAI_OK && xiaotai_quiesced() && g_identity.erase_pending);
    assert(!g_guest.open && halts == 1 && strstr(g_trace, "close erase"));
    assert(!forgot && !xiaotai_restart_pending());
    fake_on_owner(do_forget, NULL);
    assert(result == XIAOTAI_ERR_BUSY);
    fake_identity_finish_erase(XIAOTAI_FAIL);
    assert(forgot == 1 && outcome == XIAOTAI_FAIL && quiet_on == 1 && quiet_off == 1);
    assert(!xiaotai_quiesced() && !xiaotai_restart_pending() && g_identity.state == XIAOTAI_IDENTITY_VALID);
    fake_on_owner(do_forget, NULL);
    assert(result == XIAOTAI_OK);
    g_run_fail_once = true; /* overload the first storage-completion hand-off */
    fake_identity_finish_erase(XIAOTAI_OK);
    assert(xiaotai_quiesced() && !xiaotai_restart_pending() && forgot == 1 && g_run_failures == 1);
    fake_clock_advance_to(200001);
    assert(xiaotai_quiesced() && xiaotai_restart_pending() && quiet_on == 2 && quiet_off == 1);
    assert(forgot == 2 && outcome == XIAOTAI_OK && starting == 1 && !atomic_load(&restarted));
    assert(g_identity.state == XIAOTAI_IDENTITY_ABSENT && g_net.state == XIAOTAI_NET_UP);
    xiaotai_binding_stats_t stats;
    xiaotai_binding_stats(&stats);
    assert(stats.forgets == 1 && stats.forget_failures == 1);
    shutdown();

    boot();
    fake_on_owner(fill_timers, NULL);
    fake_on_owner(do_forget, NULL);
    assert(result == XIAOTAI_ERR_NO_MEM && !xiaotai_quiesced() && !g_identity.erases && !quiet_on);
    shutdown();

    boot();
    waiting();
    fake_guest_message("auth_grant", "{\"type\":\"auth_grant\",\"payload\":{\"device_id\":\"device-old\",\"device_key\":\"secret-never-output\"}}");
    assert(g_guest.ack_pending); /* known identity, straight to CONFIRMING */
    fake_on_owner(do_forget, NULL);
    assert(result == XIAOTAI_ERR_INVALID_STATE && !xiaotai_quiesced() && !g_identity.erases);
    fake_guest_ack_result(XIAOTAI_OK);
    shutdown();

    boot();
    fake_identity_set(NULL, NULL);
    waiting();
    fake_guest_message("auth_grant", "{\"type\":\"auth_grant\",\"payload\":{\"device_id\":\"new\",\"device_key\":\"fixture\"}}");
    assert(g_identity.save_pending);
    fake_on_owner(do_forget, NULL);
    assert(result == XIAOTAI_ERR_INVALID_STATE && !xiaotai_quiesced() && !g_identity.erases);
    fake_identity_finish_save(XIAOTAI_OK);
    fake_guest_ack_result(XIAOTAI_OK);
    shutdown();
}

static void reset_complete(xiaotai_err_t error, void *ctx)
{
    assert(xiaotai_on_owner() && ctx == &reset_result);
    reset_result = error;
    ++reset_completions;
}

static void do_direct(void *ctx)
{
    (void)ctx;
    result = xiaotai_binding_forget_local(true, reset_complete, &reset_result);
}

static void try_quiet(void *ctx) { (void)ctx; result = xiaotai_quiesce_begin("maintenance", &ticket); }

static void revoke(uint32_t id, xiaotai_revoke_t reason, void *ctx)
{
    (void)id; (void)reason; (void)ctx;
    ++revoked;
}

static void start_activity(void *ctx)
{
    (void)ctx;
    uint32_t id = 0;
    const xiaotai_activity_t work = {.name = "fixture_active_call", .revoked = revoke};
    assert(xiaotai_activity_begin(&work, &id) == XIAOTAI_OK && id);
}

static void test_direct_reset(void)
{
    boot();
    direct_path = true;
    fake_on_owner(start_activity, NULL);
    const uint32_t generation = g_identity.generation;
    g_identity.erase_refuse = XIAOTAI_ERR_NO_MEM;
    fake_on_owner(do_direct, NULL);
    assert(result == XIAOTAI_ERR_NO_MEM && !reset_completions && !xiaotai_quiesced() && !xiaotai_restart_pending());
    assert(g_identity.erases == 0 && !revoked);
    g_identity.erase_refuse = XIAOTAI_OK;
    fake_on_owner(do_direct, NULL);
    assert(result == XIAOTAI_OK && g_identity.erase_pending && g_identity.restart_erase && !xiaotai_quiesced());
    fake_on_owner(try_quiet, NULL);
    assert(result == XIAOTAI_ERR_BUSY && !revoked && !quiet_on);
    fake_on_owner(do_forget, NULL);
    assert(result == XIAOTAI_ERR_BUSY && g_identity.erases == 1);
    const xiaotai_identity_done_fn_t stale = g_identity.done;
    void *const stale_ctx = g_identity.done_ctx;
    fake_identity_finish_erase(XIAOTAI_FAIL);
    assert(reset_completions == 1 && reset_result == XIAOTAI_FAIL && !xiaotai_restart_pending());
    assert(g_identity.generation == generation && !g_identity.restart_erase_committed && !revoked);

    fake_on_owner(do_direct, NULL);
    assert(result == XIAOTAI_OK && g_identity.erases == 2 && g_identity.done_ctx != stale_ctx);
    stale(XIAOTAI_OK, stale_ctx);
    fake_drain_events();
    assert(reset_completions == 1 && !xiaotai_restart_pending());
    g_run_fail_once = true;
    fake_identity_finish_erase(XIAOTAI_OK);
    assert(g_run_failures == 1 && reset_completions == 1 && !xiaotai_restart_pending());
    fake_clock_advance_to(200001);
    assert(reset_completions == 2 && reset_result == XIAOTAI_OK && xiaotai_restart_pending());
    assert(!xiaotai_quiesced() && !quiet_on && !revoked && starting == 1);
    assert(g_identity.restart_erase_committed && g_identity.state == XIAOTAI_IDENTITY_VALID &&
           g_identity.generation == generation && g_net.state == XIAOTAI_NET_UP);
    stale(XIAOTAI_FAIL, stale_ctx);
    fake_drain_events();
    assert(reset_completions == 2 && forgot == 3 && outcome == XIAOTAI_OK);
    shutdown();
}

static void test_intent(void)
{
    boot();
    assert(xiaotai_intent_post_external(XIAOTAI_INTENT_BINDING_FORGET, NULL) == XIAOTAI_ERR_NOT_ALLOWED);
    fake_drain_events();
    assert(!g_identity.erases);
    assert(xiaotai_intent_post(XIAOTAI_INTENT_BINDING_FORGET, NULL) == XIAOTAI_OK);
    fake_drain_events();
    assert(g_identity.erases == 1 && g_identity.erase_pending);
    fake_identity_finish_erase(XIAOTAI_OK);
    assert(xiaotai_restart_pending() && xiaotai_quiesced() && quiet_off == 0);
    shutdown();
}

static void test_at(void)
{
    boot();
    base_at();
    if (!XIAOTAI_BINDING_DEV_COMMANDS) {
        assert(command("AT+BIND CLEAR") == XIAOTAI_ERR_INVALID_ARG);
        assert(command("AT+BIND SET device 7e") == XIAOTAI_ERR_INVALID_ARG);
        assert(!quiet_on && !g_identity.erases && !g_identity.saves);
        shutdown();
        return;
    }

    char long_id[66], long_key[515], line[768];
    memset(long_id, 'D', sizeof(long_id) - 1);
    long_id[sizeof(long_id) - 1] = '\0';
    for (size_t i = 0; i < sizeof(long_key) - 1; ++i) long_key[i] = i % 2 == 0 ? '7' : 'e';
    long_key[sizeof(long_key) - 1] = '\0';
    const char *invalid[] = {"AT+BIND SET item 7", "AT+BIND SET item zz", "AT+BIND SET item 7e00",
                             "AT+BIND SET item 00", "AT+BIND SET item", "AT+BIND CLEAR EXTRA"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(command(invalid[i]) == XIAOTAI_ERR_INVALID_ARG);
    snprintf(line, sizeof(line), "AT+BIND SET %s 7e", long_id);
    assert(command(line) == XIAOTAI_ERR_INVALID_ARG);
    snprintf(line, sizeof(line), "AT+BIND SET valid 7e %s", long_id);
    assert(command(line) == XIAOTAI_ERR_INVALID_ARG);
    snprintf(line, sizeof(line), "AT+BIND SET valid %s", long_key);
    assert(command(line) == XIAOTAI_ERR_INVALID_ARG);
    assert(!quiet_on && !g_identity.saves && !g_identity.erases && !handed.next);
    const unsigned before = wipes;

    allow_stream = false;
    assert(command("AT+BIND SET valid 7e") == XIAOTAI_ERR_NOT_SUPPORTED && !g_identity.saves && !quiet_on);
    assert(wipes == before + 1);
    allow_stream = true;
    fake_on_owner(do_quiet, NULL);
    assert(command("AT+BIND CLEAR") == XIAOTAI_ERR_BUSY && !g_identity.erases && !handed.next);
    fake_on_owner(end_quiet, NULL);

    long_id[64] = '\0';
    long_key[512] = '\0';
    snprintf(line, sizeof(line), "AT+BIND SET %s %s %s", long_id, long_key, long_id);
    assert(command(line) == XIAOTAI_OK && g_identity.saves == 1 && g_identity.save_pending && xiaotai_quiesced());
    assert(strlen(g_identity.saving.device_key) == 256 && strcmp(g_identity.saving.client_id, long_id) == 0);
    assert(wipes == before + 2 && !output[0] && !xiaotai_restart_pending());
    fake_identity_finish_save(XIAOTAI_ERR_TIMEOUT);
    assert(stream_end() == XIAOTAI_ERR_TIMEOUT && !strcmp(stream_reason, "STORE") && !output[0]);
    assert(!xiaotai_restart_pending() && !xiaotai_quiesced() && !strcmp(g_identity.identity.device_id, "device-old"));

    assert(command("AT+BIND SET new_id 7e") == XIAOTAI_OK && !g_identity.saving.client_id[0] && wipes == before + 3);
    fake_identity_finish_save(XIAOTAI_OK);
    assert(xiaotai_restart_pending() && xiaotai_quiesced() && quiet_on == quiet_off + 1U);
    fake_clock_advance_to(1000001);
    assert(!starting && !atomic_load(&restarted)); /* console still holds restart */
    assert(stream_end() == XIAOTAI_OK && !output[0]);
    fake_clock_advance_to(2000001);
    assert(starting == 1 && xiaotai_quiesced());
    shutdown();

    boot();
    assert(command("AT+BIND CLEAR") == XIAOTAI_OK && g_identity.erase_pending);
    timeout = true;
    assert(stream_end() == XIAOTAI_ERR_TIMEOUT && !strcmp(stream_reason, "STORE_PENDING"));
    assert(xiaotai_quiesced() && !xiaotai_restart_pending());
    fake_identity_finish_erase(XIAOTAI_OK); /* console already released its reference */
    assert(xiaotai_restart_pending() && forgot == 1 && outcome == XIAOTAI_OK && quiet_off == 0);
    shutdown();
}

int main(void)
{
    enum { at, at_off, forget, intent_local, direct_reset };
    switch (BINDING_AT_GROUP) {
    case at: case at_off: test_at(); break;
    case forget: test_forget(); break;
    case direct_reset: test_direct_reset(); break;
    case intent_local: test_intent(); break;
    }
    return 0;
}
