// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_environment_owners.py
#include "product_test.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "xiaotai/modules/environment.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/net.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai_net_host.h"

#if PRODUCT_TEST_HISTORY_RECOGNITION
#include <inttypes.h>
#include "xiaotai/apps/companion.h"
#include "xiaotai/modules/recognition.h"
#include "xiaotai/preferences.h"
#include "xiaotai_kv_host.h"
#endif

enum {WAIT_MS = 3000, HISTORY_SIZE = 394, SSID_OFFSET = 2, PASSWORD_OFFSET = 35};
static const char space[] = "wifi_cfg";
static xiaotai_net_host_record_t records[XIAOTAI_NET_HOST_RECORDS_MAX];
static atomic_uint history_writes;

xiaotai_err_t __real_xiaotai_kv_set_async(const char *ns, const char *key, const void *data, size_t size);
xiaotai_err_t __wrap_xiaotai_kv_set_async(const char *ns, const char *key, const void *data, size_t size)
{
    if (!strcmp(ns, space) && !strcmp(key, "known")) {
        assert(!xiaotai_on_owner());
        atomic_fetch_add(&history_writes, 1U);
    }
    return __real_xiaotai_kv_set_async(ns, key, data, size);
}

static xiaotai_net_host_record_t initial_join(void)
{
    assert(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_JOIN, 1, WAIT_MS));
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    const size_t count = xiaotai_net_host_records(records, XIAOTAI_NET_HOST_RECORDS_MAX);
    for (size_t i = 0; i < count; ++i) {
        if (records[i].call != XIAOTAI_NET_HOST_JOIN) continue;
        assert(records[i].token && records[i].on_worker && records[i].has_credentials);
        assert(!strcmp(records[i].credentials.ssid, PRODUCT_TEST_SSID));
        assert(!strcmp(records[i].credentials.password, PRODUCT_TEST_PASSWORD));
        assert(xiaotai_net_credentials_valid(&records[i].credentials));
        return records[i];
    }
    assert(!"the generated product did not connect with stored credentials");
    return (xiaotai_net_host_record_t){0};
}

static void publish_address(const xiaotai_net_host_record_t *join)
{
    xiaotai_net_driver_event_t joined = {.kind = XIAOTAI_NET_DRIVER_JOINED, .token = join->token};
    memcpy(joined.ssid, join->credentials.ssid, sizeof(joined.ssid));
    const uint8_t ip[4] = {192, 0, 2, 7};
    xiaotai_net_driver_event_t address = {.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = join->token};
    memcpy(&address.ipv4, ip, sizeof(ip));
    assert(xiaotai_net_host_emit(&joined) == XIAOTAI_OK);
    assert(xiaotai_net_host_emit(&address) == XIAOTAI_OK);
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    assert(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    xiaotai_net_status_t status;
    xiaotai_net_status(&status);
    assert(status.state == XIAOTAI_NET_UP && status.ipv4 == address.ipv4);
}

static void forget(void *unused)
{
    (void)unused;
    assert(xiaotai_net_forget_credentials() == XIAOTAI_OK);
}

static void expect_missing(const char *key)
{
    uint8_t bytes[492];
    size_t size = sizeof(bytes);
    assert(xiaotai_kv_get_blob_strict(space, key, bytes, &size, WAIT_MS) == XIAOTAI_ERR_NOT_FOUND);
}

void product_history(bool clear)
{
    xiaotai_net_config_t configured, effective;
    xiaotai_net_host_config(&configured, &effective);
    assert(configured.history_capacity == 4 && effective.history_capacity == 4);
    assert(effective.layout == XIAOTAI_NET_STORE_RECORD && !strcmp(effective.space, space));
    assert(xiaotai_net_has_saved_credentials());
    const xiaotai_net_host_record_t join = initial_join();
    expect_missing("known");

    if (clear) {
        /* Boot has already submitted a join. Clear a real old history while
         * that attempt is awaiting an address, then deliver its late success. */
        const uint8_t old[HISTORY_SIZE] = {1, 1, 'o', 'l', 'd'};
        assert(xiaotai_kv_set(space, "known", old, sizeof(old), WAIT_MS) == XIAOTAI_OK);
        product_on_owner(forget, NULL);
        assert(!xiaotai_net_has_saved_credentials());
        expect_missing("credentials");
        expect_missing("known");
        publish_address(&join);
        expect_missing("known");
        puts("P4 history: real erase; late address did not recreate history in the same boot");
        return;
    }

    publish_address(&join);
    uint8_t raw[492], expected[HISTORY_SIZE] = {1, 1};
    size_t size = sizeof(raw);
    assert(xiaotai_kv_get_blob_strict(space, "known", raw, &size, WAIT_MS) == XIAOTAI_OK);
    assert(size == HISTORY_SIZE);
    memcpy(expected + SSID_OFFSET, join.credentials.ssid, strlen(join.credentials.ssid));
    memcpy(expected + PASSWORD_OFFSET, join.credentials.password, strlen(join.credentials.password));
    assert(!memcmp(raw, expected, sizeof(expected)));
    puts("P4 history: first network after an erased history is a byte-exact 394-byte legacy record");
}

typedef struct { xiaotai_signal_t acquired, cancel; } history_gate_t;

static void begin_environment(void)
{
    assert(xiaotai_on_owner());
    assert(xiaotai_environment_set("http://fixture.invalid/ep") == XIAOTAI_OK);
    assert(xiaotai_quiesced_for(XIAOTAI_QUIESCE_ENVIRONMENT));
}

static void hold_environment(void *ctx)
{
    history_gate_t *gate = ctx;
    const xiaotai_net_credentials_t other = {.ssid = "other-ap", .password = PRODUCT_TEST_PASSWORD};
    assert(xiaotai_on_owner());
#if !PRODUCT_TEST_HISTORY_RECOGNITION
    begin_environment();
#endif
    assert(xiaotai_quiesced_for(XIAOTAI_QUIESCE_ENVIRONMENT));
    int32_t before, after;
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_VOLUME, &before) == XIAOTAI_OK);
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_VOLUME, before ? before - 1 : before + 1) == XIAOTAI_ERR_BUSY);
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_VOLUME, &after) == XIAOTAI_OK && after == before);
    assert(xiaotai_net_set_credentials(&other) == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_net_forget_credentials() == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_net_disconnect() == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_net_has_saved_credentials());
    xiaotai_signal_give(&gate->acquired);
    /* Hold only this owner job so the real timer cannot commit the switch
     * before the network worker has processed the address notification. */
    assert(xiaotai_signal_take(&gate->cancel, WAIT_MS));
    assert(xiaotai_environment_cancel() == XIAOTAI_OK);
}

#if PRODUCT_TEST_HISTORY_RECOGNITION
static void recognition_ready(void *ctx)
{
    *(bool *)ctx = xiaotai_recognition_maintenance_check() == XIAOTAI_OK;
}

enum {VOLUME, SPEAKER, MICROPHONE, SLEEP, ACK, EMOTION, SETTINGS};
static const char *const setting_names[SETTINGS] = {
    [VOLUME] = XIAOTAI_SETTING_AUDIO_VOLUME, [SPEAKER] = XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED,
    [MICROPHONE] = XIAOTAI_SETTING_AUDIO_MIC_MUTED, [SLEEP] = XIAOTAI_SETTING_SCREEN_SLEEP,
    [ACK] = XIAOTAI_SETTING_APP_COMPANION_ACK_VOICE, [EMOTION] = XIAOTAI_SETTING_APP_COMPANION_EMOTION,
};

typedef struct {
    int32_t values[SETTINGS];
    xiaotai_preferences_stats_t prefs;
    xiaotai_settings_stats_t settings;
    uint32_t app_refused, app_stale, registry_refused;
} action_state_t;

static void count_registry_refusals(void *ctx, const char *line)
{
    static const char key[] = " settings_refused=";
    const char *field = strstr(line, key);
    if (field) assert(sscanf(field + sizeof(key) - 1U, "%" SCNu32, (uint32_t *)ctx) == 1);
}

static void read_action_state(void *ctx)
{
    action_state_t *state = ctx;
    xiaotai_environment_status_t env;
    xiaotai_environment_status(&env);
    assert(xiaotai_quiesced_for(XIAOTAI_QUIESCE_ENVIRONMENT) && !xiaotai_activity_current(NULL));
    assert(env.phase == XIAOTAI_ENVIRONMENT_WAITING && !env.write_attempted && xiaotai_preferences_pending());
    xiaotai_app_companion_status_t app;
    xiaotai_app_companion_status(&app);
    assert(app.foreground && app.setup && !strcmp(app.page, XIAOTAI_APP_COMPANION_PAGE_MAINTENANCE));
    state->app_refused = app.refused;
    state->app_stale = app.stale;
    for (size_t i = 0; i < SETTINGS; ++i)
        assert(xiaotai_setting_get_int(setting_names[i], &state->values[i]) == XIAOTAI_OK);
    xiaotai_preferences_stats(&state->prefs);
    xiaotai_settings_stats(&state->settings);
    state->registry_refused = UINT32_MAX;
    const xiaotai_reply_t report = {.write = count_registry_refusals, .ctx = &state->registry_refused};
    xiaotai_diag_report("framework", &report);
    assert(state->registry_refused != UINT32_MAX);
}

static void begin_action_window(void *unused)
{
    (void)unused;
    /* Keep the real environment's preferences barrier waiting: unrelated
     * fixture data, not a user setting or an environment write. */
    assert(!xiaotai_preferences_pending());
    assert(xiaotai_preferences_store_int("history_hold", 1) == XIAOTAI_OK);
    begin_environment();
}

static action_state_t exercise_frozen_actions(void)
{
    action_state_t before;
    product_on_owner(read_action_state, &before);
    const int32_t volume = before.values[VOLUME];
    const int32_t step = xiaotai_settings_volume_step_percent();
    assert(volume > 0 && volume < 100 && step > 0);

    enum {APP_REFUSED, STALE, REGISTRY_REFUSED};
    const struct {const char *name; int32_t value; unsigned expected;} actions[] = {
        /* Values produced by the real touch form and by the now-replaced
         * emotion/call pages; call generation 1 is stale with no active call. */
        {XIAOTAI_INTENT_APP_COMPANION_SETTING_PREFIX XIAOTAI_SETTING_AUDIO_VOLUME,
         volume > step ? volume - step : 0, APP_REFUSED},
        {XIAOTAI_INTENT_APP_COMPANION_SETTING_PREFIX XIAOTAI_SETTING_AUDIO_VOLUME,
         volume + step < 100 ? volume + step : 100, APP_REFUSED},
        {XIAOTAI_INTENT_APP_COMPANION_SETTING_PREFIX XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED,
         before.values[SPEAKER], APP_REFUSED},
        {XIAOTAI_INTENT_APP_COMPANION_SETTING_PREFIX XIAOTAI_SETTING_AUDIO_MIC_MUTED,
         before.values[MICROPHONE], APP_REFUSED},
        {XIAOTAI_INTENT_APP_COMPANION_SETTING_PREFIX XIAOTAI_SETTING_SCREEN_SLEEP,
         before.values[SLEEP] == 300 ? 2 : 1, APP_REFUSED},
        {XIAOTAI_INTENT_APP_COMPANION_SETTING_PREFIX XIAOTAI_SETTING_APP_COMPANION_ACK_VOICE,
         !before.values[ACK], APP_REFUSED},
        {XIAOTAI_INTENT_APP_COMPANION_EMOTION_APPLY, before.values[EMOTION] == 7 ? 8 : 7, STALE},
        {"companion.call.mute", 1, APP_REFUSED},
        /* The old voice producer emitted WAKE_ONLY. These are the four
         * setting commands at the consumer boundary, not recognition tests. */
        {XIAOTAI_INTENT_AUDIO_VOLUME_UP, 0, REGISTRY_REFUSED},
        {XIAOTAI_INTENT_AUDIO_VOLUME_DOWN, 0, REGISTRY_REFUSED},
        {XIAOTAI_INTENT_AUDIO_MIC_MUTE, 1, REGISTRY_REFUSED},
        {XIAOTAI_INTENT_AUDIO_MIC_MUTE, 0, REGISTRY_REFUSED},
    };
    action_state_t previous = before;
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
        const xiaotai_args_t args = {.value = actions[i].value};
        assert(xiaotai_intent_post(actions[i].name, &args) == XIAOTAI_OK);
        action_state_t current;
        /* This owner callback runs after the posted intent, not after thaw. */
        product_on_owner(read_action_state, &current);
        assert(!memcmp(current.values, before.values, sizeof(before.values)));
        assert(current.prefs.stores == before.prefs.stores && current.prefs.writes == before.prefs.writes &&
               current.prefs.write_failures == before.prefs.write_failures);
        assert(current.settings.changes == before.settings.changes && current.settings.applied == before.settings.applied);
        assert(current.app_refused == previous.app_refused + (actions[i].expected == APP_REFUSED));
        assert(current.app_stale == previous.app_stale + (actions[i].expected == STALE));
        assert(current.registry_refused == previous.registry_refused + (actions[i].expected == REGISTRY_REFUSED));
        previous = current;
    }
    puts("environment settings: eight touch intents, four voice consumer intents; no settings change or write");
    return before;
}
#endif

void product_history_maintenance(void)
{
    const xiaotai_net_host_record_t join = initial_join();
    xiaotai_net_config_t config;
    xiaotai_net_host_config(NULL, &config);
    assert(config.layout == XIAOTAI_NET_STORE_RECORD && !strcmp(config.space, space));
    assert(config.history_capacity == 4 || config.history_capacity == 5);
    expect_missing("known");
#if PRODUCT_TEST_HISTORY_RECOGNITION
    /* The S3 recognition module must finish reading its journal before the
     * maintenance gate admits a switch, even without a bound identity. */
    const int64_t deadline = xiaotai_time_us() + (int64_t)WAIT_MS * 1000;
    bool ready = false;
    do {
        product_on_owner(recognition_ready, &ready);
        if (!ready) xiaotai_delay_ms(1);
    } while (!ready && xiaotai_time_us() < deadline);
    assert(ready);
    assert(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    xiaotai_kv_host_gate(false);
    product_on_owner(begin_action_window, NULL);
    assert(xiaotai_preferences_flush(20) == XIAOTAI_ERR_TIMEOUT);
    assert(xiaotai_kv_host_wait_held(WAIT_MS));
    const action_state_t frozen = exercise_frozen_actions();
#endif
    history_gate_t gate;
    assert(xiaotai_signal_init(&gate.acquired) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&gate.cancel) == XIAOTAI_OK);
    assert(xiaotai_run(hold_environment, &gate) == XIAOTAI_OK && xiaotai_signal_take(&gate.acquired, WAIT_MS));
#if PRODUCT_TEST_HISTORY_RECOGNITION
    xiaotai_kv_host_gate(true);
#endif
    publish_address(&join);
    expect_missing("known");
    xiaotai_net_diag_t diag;
    xiaotai_net_diag(&diag);
    assert(atomic_load(&history_writes) == 0 && diag.history_deferred == 1);
    xiaotai_signal_give(&gate.cancel);
    product_on_owner(NULL, NULL);
    xiaotai_signal_deinit(&gate.cancel);
    xiaotai_signal_deinit(&gate.acquired);
#if PRODUCT_TEST_HISTORY_RECOGNITION
    assert(xiaotai_preferences_flush(WAIT_MS) == XIAOTAI_OK);
    xiaotai_preferences_stats_t after;
    xiaotai_preferences_stats(&after);
    assert(after.stores == frozen.prefs.stores && after.writes == frozen.prefs.writes &&
           after.write_failures == frozen.prefs.write_failures);
#endif
    assert(!xiaotai_quiesced() && !xiaotai_restart_pending());
    xiaotai_environment_status_t status;
    xiaotai_environment_status(&status);
    assert(status.phase == XIAOTAI_ENVIRONMENT_IDLE && !status.write_attempted && !status.environment.saved[0]);
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK && xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    uint8_t raw[492], expected[492] = {1, 1};
    size_t size = sizeof(raw);
    assert(xiaotai_kv_get_blob_strict(space, "known", raw, &size, WAIT_MS) == XIAOTAI_OK);
    assert(size == 2U + config.history_capacity * sizeof(xiaotai_net_credentials_t));
    memcpy(expected + SSID_OFFSET, join.credentials.ssid, strlen(join.credentials.ssid));
    memcpy(expected + PASSWORD_OFFSET, join.credentials.password, strlen(join.credentials.password));
    assert(!memcmp(raw, expected, size));
    /* Later worker passes cannot flush the same deferred history twice. */
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK && xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    xiaotai_net_diag(&diag);
    assert(atomic_load(&history_writes) == 1 && diag.history_deferred == 1);
    printf("environment history: no write while held; one %zu-byte background write after cancel\n", size);
}
