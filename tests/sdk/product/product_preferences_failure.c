// SPDX-License-Identifier: MIT
// Replaces legacy: tests/lvgl/test_preferences_choice.c::preferences_choice_0
// Replaces legacy: tests/lvgl/test_preferences_choice.c::preferences_choice_1
// Replaces legacy: tests/lvgl/test_preferences_choice.c::preferences_choice_2
// Replaces legacy: tests/lvgl/test_preferences_choice.c::preferences_choice_3
// Replaces legacy: tests/regression/test_native_preferences.py
// Replaces legacy: tests/regression/test_environment_owners.py
// Replaces legacy: tests/regression/test_preferences_choice.py
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/apps/companion.h"
#ifndef PRODUCT_TEST_PREFERENCES_EMOTION
#define PRODUCT_TEST_PREFERENCES_EMOTION 0
#endif
#if PRODUCT_TEST_WT99
#include "xiaotai/apps/door.h"
#endif
/* A product with a typed snapshot saves all its snapshot-bound settings at
 * once; the count comes from the product's binding table (CMakeLists.txt). */
#if defined(PRODUCT_TEST_SNAPSHOT_SETTINGS) && PRODUCT_TEST_SNAPSHOT_SETTINGS > 0
#define WRITES_PER_SAVE ((unsigned)PRODUCT_TEST_SNAPSHOT_SETTINGS)
#else
#define WRITES_PER_SAVE 1U
#endif
#include "xiaotai/modules/environment.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai/preferences.h"
#include "xiaotai/view.h"
#include "xiaotai_kv_host.h"
#include "xiaotai_net_host.h"
#if PRODUCT_TEST_HISTORY_RECOGNITION
#include "xiaotai/modules/recognition.h"
#endif

#define BASE "http://fixture.invalid/environment"
#define WAIT_MS 5000

static atomic_uint stores;
static atomic_bool capture_store;

/* Stop at the real environment persistence boundary; no target is written. */
xiaotai_err_t __real_xiaotai_platform_environment_store(const char *base, xiaotai_platform_done_fn_t done, void *ctx);
xiaotai_err_t __wrap_xiaotai_platform_environment_store(const char *base, xiaotai_platform_done_fn_t done, void *ctx)
{
    if (!atomic_load(&capture_store)) return __real_xiaotai_platform_environment_store(base, done, ctx);
    assert(!strcmp(base, BASE) && done && ctx);
    ++stores;
    return XIAOTAI_ERR_NO_MEM;
}

typedef struct {
    const char *name;
    int32_t value;
    const char *text;
} setting_edit_t;

static void apply_setting(void *ctx)
{
    const setting_edit_t *edit = ctx;
    if (edit->text) {
        assert(xiaotai_setting_set_text(edit->name, edit->text) == XIAOTAI_OK);
    } else {
        assert(xiaotai_setting_set_int(edit->name, edit->value) == XIAOTAI_OK);
    }
}

static void edit_and_wait(setting_edit_t edit, bool failed)
{
    xiaotai_preferences_stats_t before, after;
    xiaotai_preferences_stats(&before);
    product_on_owner(apply_setting, &edit);
    const int64_t until = xiaotai_time_us() + WAIT_MS * INT64_C(1000);
    do {
        xiaotai_preferences_stats(&after);
        if (!xiaotai_preferences_pending() && after.writes > before.writes) break;
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < until);
    assert(!xiaotai_preferences_pending());
    /* Wait past the worker completion without consuming a preferences error. */
    assert(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    xiaotai_preferences_stats(&after);
    assert(!xiaotai_preferences_pending() && after.writes == before.writes + WRITES_PER_SAVE);
    assert(after.write_failures == before.write_failures + failed);
    if (edit.text) {
        char displayed[XIAOTAI_SETTING_TEXT_MAX];
        assert(xiaotai_setting_get_text(edit.name, displayed, sizeof(displayed)) == XIAOTAI_OK);
        assert(!strcmp(displayed, edit.text));
    } else {
        int32_t displayed;
        assert(xiaotai_setting_get_int(edit.name, &displayed) == XIAOTAI_OK && displayed == edit.value);
    }
}

static int64_t raw_setting(const char *name)
{
    int64_t value;
    assert(xiaotai_kv_get_int("product", name, XIAOTAI_KV_U8, &value, WAIT_MS) == XIAOTAI_OK);
    return value;
}

void product_preferences_choices_prepare(void)
{
    assert(xiaotai_kv_set_int("product", "ack_voice", XIAOTAI_KV_U8, 0, WAIT_MS) == XIAOTAI_OK);
    assert(xiaotai_kv_set_int("product", "sleep", XIAOTAI_KV_U8, 1, WAIT_MS) == XIAOTAI_OK);
    assert(xiaotai_kv_set_int("product", XIAOTAI_SETTING_KEY_APP_COMPANION_EMOTION, XIAOTAI_KV_U8, 0, WAIT_MS) ==
           XIAOTAI_OK);
    product_platform_prepare(true);
}

typedef struct {
    const char *name, *key, *intent;
    int32_t value, option;
    int64_t original;
    uint32_t revision;
} choice_edit_t;

static void record_choice_revision(void *ctx)
{
    choice_edit_t *edit = ctx;
    edit->revision = xiaotai_setting_store_revision(edit->key);
}

static void post_choice(const choice_edit_t *edit, const char *marker)
{
    xiaotai_args_t request = {.value = edit->option};
    if (marker) strcpy(request.text, marker);
    assert(xiaotai_intent_post(edit->intent, &request) == XIAOTAI_OK);
    product_on_owner(NULL, NULL);
}

static xiaotai_preferences_stats_t choice_save_settled(void)
{
    const int64_t until = xiaotai_time_us() + WAIT_MS * INT64_C(1000);
    while (xiaotai_preferences_pending() && xiaotai_time_us() < until) xiaotai_delay_ms(5);
    assert(!xiaotai_preferences_pending() && xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    product_on_owner(NULL, NULL);
    xiaotai_preferences_stats_t stats;
    xiaotai_preferences_stats(&stats);
    return stats;
}

static void read_companion_status(void *ctx) { xiaotai_app_companion_status(ctx); }

static void begin_choice_maintenance(void *ctx)
{
    assert(xiaotai_quiesce_begin(XIAOTAI_QUIESCE_ENVIRONMENT, ctx) == XIAOTAI_OK);
}

static void end_choice_maintenance(void *ctx) { xiaotai_quiesce_end(*(const uint32_t *)ctx); }

static void open_choices(void)
{
    const xiaotai_args_t page = {.text = XIAOTAI_APP_COMPANION_PAGE_SETTINGS};
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_SHOW, &page) == XIAOTAI_OK);
    xiaotai_app_companion_status_t app;
    product_on_owner(read_companion_status, &app);
    assert(app.foreground && !app.setup && !strcmp(app.page, XIAOTAI_APP_COMPANION_PAGE_SETTINGS));
}

static void check_invalid_choices(const choice_edit_t *choices, size_t count)
{
    const xiaotai_preferences_stats_t before = choice_save_settled();
    xiaotai_app_companion_status_t prior, after;
    product_on_owner(read_companion_status, &prior);
    const xiaotai_args_t invalid = {.value = 99};
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_SETTING_PREFIX "not-a-setting", &invalid) ==
           XIAOTAI_ERR_NOT_FOUND);
    for (size_t i = 0; i < count; ++i) {
        choice_edit_t edit = choices[i];
        int32_t live;
        assert(xiaotai_setting_get_int(edit.name, &live) == XIAOTAI_OK);
        const int32_t previous = live;
        edit.option = invalid.value;
        post_choice(&edit, NULL);
        assert(xiaotai_setting_get_int(edit.name, &live) == XIAOTAI_OK && live == previous);
        assert(raw_setting(edit.key) == edit.original);
    }
    const xiaotai_preferences_stats_t saved = choice_save_settled();
    product_on_owner(read_companion_status, &after);
    printf("invalid_choices controls=%zu refusals=%u saves=%u writes=%u\n",
           count, after.refused - prior.refused, saved.stores - before.stores, saved.writes - before.writes);
    fflush(stdout);
    assert(after.refused == prior.refused && saved.stores == before.stores && saved.writes == before.writes);
}

static void check_choice_maintenance(const choice_edit_t *field, const int32_t *values, size_t count)
{
    unsigned confirmations = 0;
    for (size_t old = 0; old < count; ++old) {
        for (size_t next = 0; next < count; ++next) {
            setting_edit_t prior_value = {.name = field->name, .value = values[old]};
            product_on_owner(apply_setting, &prior_value);
            const xiaotai_preferences_stats_t before = choice_save_settled();
            assert(raw_setting(field->key) == (int64_t)old);
            open_choices();
            choice_edit_t edit = *field;
            edit.option = (int32_t)next;
            const char *const marker = old == next ? XIAOTAI_ARG_CHOICE_RECONFIRM : NULL;

            uint32_t ticket = 0;
            product_on_owner(begin_choice_maintenance, &ticket);
            xiaotai_app_companion_status_t prior, result;
            product_on_owner(read_companion_status, &prior);
            assert(ticket && xiaotai_quiesced() && prior.setup &&
                   !strcmp(prior.page, XIAOTAI_APP_COMPANION_PAGE_MAINTENANCE));
            post_choice(&edit, marker);
            product_on_owner(read_companion_status, &result);
            assert(result.refused == prior.refused + 1U && result.stale == prior.stale &&
                   !strcmp(result.page, prior.page) && xiaotai_quiesced());
            const xiaotai_preferences_stats_t frozen = choice_save_settled();
            int32_t live;
            assert(xiaotai_setting_get_int(edit.name, &live) == XIAOTAI_OK && live == values[old]);
            assert(raw_setting(edit.key) == (int64_t)old && frozen.stores == before.stores &&
                   frozen.writes == before.writes && frozen.write_failures == before.write_failures);

            product_on_owner(end_choice_maintenance, &ticket);
            assert(!xiaotai_quiesced());
            open_choices();
            xiaotai_kv_host_gate(false);
            post_choice(&edit, marker);
            assert(xiaotai_kv_host_wait_held(WAIT_MS) && xiaotai_preferences_pending());
            xiaotai_kv_host_gate(true);
            const xiaotai_preferences_stats_t saved = choice_save_settled();
            product_on_owner(read_companion_status, &result);
            assert(result.refused == prior.refused + 1U && result.stale == prior.stale);
            assert(xiaotai_setting_get_int(edit.name, &live) == XIAOTAI_OK && live == values[next]);
            assert(raw_setting(edit.key) == (int64_t)next && saved.stores == before.stores + 1U &&
                   saved.writes == before.writes + WRITES_PER_SAVE && saved.write_failures == before.write_failures);
            confirmations += (unsigned)(marker != NULL);
        }
    }
    printf("choice_maintenance key=%s pairs=%zu frozen_writes=0 refusals=%zu "
           "thawed_saves=%zu same_value=%u old_keys_per_save=%u\n",
           field->key, count * count, count * count, count * count, confirmations, WRITES_PER_SAVE);
}

static void preview_emotion(const xiaotai_args_t *choice)
{
    const xiaotai_args_t page = {.text = XIAOTAI_APP_COMPANION_PAGE_EMOTIONS};
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_SHOW, &page) == XIAOTAI_OK);
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_EMOTION, choice) == XIAOTAI_OK);
}

static void check_emotion_while_frozen(void)
{
    const xiaotai_args_t choice = {.value = 7};
    preview_emotion(&choice);
    xiaotai_app_companion_status_t prior, result;
    product_on_owner(read_companion_status, &prior);
    assert(!strcmp(prior.page, XIAOTAI_APP_COMPANION_PAGE_EMOTION));
    assert(raw_setting(XIAOTAI_SETTING_KEY_APP_COMPANION_EMOTION) == 0);
    const xiaotai_preferences_stats_t before = choice_save_settled();

    uint32_t ticket = 0;
    product_on_owner(begin_choice_maintenance, &ticket);
    assert(ticket && xiaotai_quiesced());
    product_on_owner(read_companion_status, &prior);
    assert(prior.setup && !strcmp(prior.page, XIAOTAI_APP_COMPANION_PAGE_MAINTENANCE));
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_EMOTION_APPLY, &choice) == XIAOTAI_OK);
    product_on_owner(read_companion_status, &result);
    assert(result.setup && !strcmp(result.page, prior.page) && result.refused == prior.refused &&
           result.stale == prior.stale + 1U);
    const xiaotai_preferences_stats_t blocked = choice_save_settled();
    int32_t live;
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_APP_COMPANION_EMOTION, &live) == XIAOTAI_OK && live == 0);
    assert(raw_setting(XIAOTAI_SETTING_KEY_APP_COMPANION_EMOTION) == 0);
    assert(blocked.stores == before.stores && blocked.writes == before.writes &&
           blocked.write_failures == before.write_failures);

    product_on_owner(end_choice_maintenance, &ticket);
    assert(!xiaotai_quiesced());
    preview_emotion(&choice);
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_EMOTION_APPLY, &choice) == XIAOTAI_OK);
    product_on_owner(read_companion_status, &result);
    assert(!result.setup && !strcmp(result.page, XIAOTAI_APP_COMPANION_PAGE_HOME) &&
           result.refused == prior.refused && result.stale == prior.stale + 1U);
    const xiaotai_preferences_stats_t saved = choice_save_settled();
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_APP_COMPANION_EMOTION, &live) == XIAOTAI_OK && live == choice.value);
    assert(raw_setting(XIAOTAI_SETTING_KEY_APP_COMPANION_EMOTION) == choice.value);
    assert(saved.stores == before.stores + 1U && saved.writes == before.writes + WRITES_PER_SAVE &&
           saved.write_failures == before.write_failures);
    printf("emotion_apply maintenance_dropped_stale=1 frozen_stores=0 frozen_writes=0 thawed_writes=%u raw=%ld\n",
           saved.writes - before.writes, (long)live);
}

static void check_emotion_reapply(void)
{
    const char *const key = XIAOTAI_SETTING_KEY_APP_COMPANION_EMOTION;
    setting_edit_t setting = {.name = XIAOTAI_SETTING_APP_COMPANION_EMOTION, .value = 8};
    const xiaotai_args_t choice = {.value = setting.value};
    const int64_t original = raw_setting(key);
    assert(original != choice.value);
    const xiaotai_preferences_stats_t before = choice_save_settled();
    xiaotai_kv_host_fail_key(key, XIAOTAI_FAIL);
    preview_emotion(&choice);
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_EMOTION_APPLY, &choice) == XIAOTAI_OK);
    product_on_owner(NULL, NULL);
    const xiaotai_preferences_stats_t failed = choice_save_settled();
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    int32_t live;
    assert(xiaotai_setting_get_int(setting.name, &live) == XIAOTAI_OK && live == choice.value);
    assert(raw_setting(key) == original);
    assert(failed.stores == before.stores + 1U && failed.writes == before.writes + WRITES_PER_SAVE &&
           failed.write_failures == before.write_failures + 1U);
    choice_edit_t edit = {.key = key};
    product_on_owner(record_choice_revision, &edit);
    const uint32_t revision = edit.revision;
    assert(revision);

    product_on_owner(apply_setting, &setting);
    preview_emotion(&choice);
    const xiaotai_args_t stale = {.value = (int32_t)original};
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_EMOTION_APPLY, &stale) == XIAOTAI_OK);
    xiaotai_app_companion_status_t app;
    product_on_owner(read_companion_status, &app);
    assert(!strcmp(app.page, XIAOTAI_APP_COMPANION_PAGE_EMOTION));
    const xiaotai_preferences_stats_t unchanged = choice_save_settled();
    assert(unchanged.stores == failed.stores && unchanged.writes == failed.writes && raw_setting(key) == original);

    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_EMOTION_APPLY, &choice) == XIAOTAI_OK);
    product_on_owner(read_companion_status, &app);
    assert(!strcmp(app.page, XIAOTAI_APP_COMPANION_PAGE_HOME));
    const xiaotai_preferences_stats_t saved = choice_save_settled();
    product_on_owner(record_choice_revision, &edit);
    const int64_t raw = raw_setting(key);
    printf("emotion_reapply live=%ld old_raw=%lld raw=%lld same_writes=0 stale_writes=0 reapply_writes=%u "
           "revision_unchanged=%u\n", (long)live, (long long)original, (long long)raw,
           saved.writes - failed.writes, (unsigned)(revision == edit.revision));
    fflush(stdout);
    assert(saved.stores == failed.stores + 1U && saved.writes == failed.writes + WRITES_PER_SAVE &&
           saved.write_failures == failed.write_failures && raw == choice.value && revision == edit.revision);
}

int product_preferences_choices_run(bool invalid)
{
    choice_edit_t choices[] = {
        {XIAOTAI_SETTING_APP_COMPANION_ACK_VOICE, XIAOTAI_SETTING_KEY_APP_COMPANION_ACK_VOICE,
         XIAOTAI_INTENT_APP_COMPANION_SETTING_PREFIX XIAOTAI_SETTING_APP_COMPANION_ACK_VOICE, 1, 1, 0, 0},
        {XIAOTAI_SETTING_SCREEN_SLEEP, XIAOTAI_SETTING_KEY_SCREEN_SLEEP,
         XIAOTAI_INTENT_APP_COMPANION_SETTING_PREFIX XIAOTAI_SETTING_SCREEN_SLEEP, 600, 2, 1, 0},
    };
    product_platform_online();
    open_choices();
    if (invalid) {
        check_invalid_choices(choices, sizeof(choices) / sizeof(choices[0]));
        return 0;
    }
    for (size_t i = 0; i < sizeof(choices) / sizeof(*choices); ++i) {
        choice_edit_t *edit = &choices[i];
        assert(raw_setting(edit->key) == edit->original);
        xiaotai_kv_host_fail_key(edit->key, XIAOTAI_FAIL);
        edit_and_wait((setting_edit_t){.name = edit->name, .value = edit->value}, true);
        xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
        product_on_owner(record_choice_revision, edit);
        const uint32_t revision = edit->revision;
        assert(revision && raw_setting(edit->key) == edit->original);
        const xiaotai_preferences_stats_t failed = choice_save_settled();
        post_choice(edit, NULL);
        assert(choice_save_settled().writes == failed.writes && raw_setting(edit->key) == edit->original);
        post_choice(edit, XIAOTAI_ARG_CHOICE_RECONFIRM);
        const xiaotai_preferences_stats_t saved = choice_save_settled();
        product_on_owner(record_choice_revision, edit);
        const int64_t raw = raw_setting(edit->key);
        printf("choice_reselect key=%s old=%lld raw=%lld writes=%u revision_unchanged=%u\n",
               edit->key, (long long)edit->original, (long long)raw, saved.writes - failed.writes,
               (unsigned)(edit->revision == revision));
        fflush(stdout);
        assert(saved.writes == failed.writes + WRITES_PER_SAVE && saved.write_failures == failed.write_failures);
        assert(raw == edit->option && edit->revision == revision);
    }
    if (PRODUCT_TEST_PREFERENCES_EMOTION) {
        check_emotion_while_frozen();
        check_emotion_reapply();
    }
    static const int32_t voice[] = {0, 1};
    static const int32_t sleep[] = {60, 300, 600, 1800, 0};
    check_choice_maintenance(&choices[0], voice, sizeof(voice) / sizeof(*voice));
    check_choice_maintenance(&choices[1], sleep, sizeof(sleep) / sizeof(*sleep));
    return 0;
}

#if PRODUCT_TEST_WT99
static void check_door_peer(const char *expected)
{
    char peer[XIAOTAI_SETTING_TEXT_MAX];
    assert(xiaotai_kv_get_str_strict("product", "door_peer", peer, sizeof(peer), WAIT_MS) == XIAOTAI_OK);
    assert(!strcmp(peer, expected));
}

static void check_wt99_peer_round_trip(void)
{
    char peer[XIAOTAI_SETTING_TEXT_MAX];
    memset(peer, 'p', sizeof(peer) - 1U);
    peer[sizeof(peer) - 1U] = '\0';
    edit_and_wait((setting_edit_t){.name = XIAOTAI_SETTING_APP_DOOR_PEER, .text = peer}, false);
    check_door_peer(peer);
    edit_and_wait((setting_edit_t){.name = XIAOTAI_SETTING_APP_DOOR_PEER, .text = ""}, false);
    check_door_peer("");
    assert(raw_setting("volume") == 6 && raw_setting("mic_mute") == 1);
}
#endif

static void start_environment(void *ctx)
{
    bool *const started = ctx;
#if PRODUCT_TEST_HISTORY_RECOGNITION
    /* The cold fixture needs an idle recognition owner, not a retry of the
     * switch. Check and submit in one owner job, before catch-up can run. */
    if (started && xiaotai_recognition_maintenance_check() != XIAOTAI_OK) return;
#endif
    assert(!xiaotai_quiesced() && !xiaotai_restart_pending());
    assert(xiaotai_environment_set(BASE) == XIAOTAI_OK);
    xiaotai_environment_status_t now;
    xiaotai_environment_status(&now);
    assert(now.phase == XIAOTAI_ENVIRONMENT_WAITING && !now.write_attempted);
    if (started) *started = true;
}

static xiaotai_environment_status_t environment_attempt_with_readiness(bool wait_for_recognition)
{
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    if (wait_for_recognition) {
        const int64_t deadline = xiaotai_time_us() + WAIT_MS * INT64_C(1000);
        bool started = false;
        do {
            product_on_owner(start_environment, &started);
            if (!started) xiaotai_delay_ms(1);
        } while (!started && xiaotai_time_us() < deadline);
        assert(started);
    } else {
        product_on_owner(start_environment, NULL);
    }
    const int64_t until = xiaotai_time_us() + WAIT_MS * INT64_C(1000);
    xiaotai_environment_status_t now;
    do {
        xiaotai_environment_status(&now);
        if (now.phase == XIAOTAI_ENVIRONMENT_IDLE) break;
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < until);
    assert(now.phase == XIAOTAI_ENVIRONMENT_IDLE && !xiaotai_restart_pending());
    return now;
}

static xiaotai_environment_status_t environment_attempt(void)
{
    return environment_attempt_with_readiness(false);
}

#if PRODUCT_TEST_WT99
void product_wt99_tap_fixture_peer(bool already_selected);
void product_wt99_tap_named_fixture_peer(const char *label, bool already_selected);
void product_preferences_reselect_prepare(void);
int product_preferences_reselect_run(bool enabled);
void product_preferences_reselect_soak_prepare(void);
int product_preferences_reselect_soak(void);

void product_preferences_reselect_prepare(void)
{
    assert(xiaotai_kv_set_str("product", "door_peer", "before", WAIT_MS) == XIAOTAI_OK);
    product_platform_device_contacts_body("{\"code\":200,\"data\":{\"contacts\":["
        "{\"type\":\"device\",\"device_id\":\"peer\",\"remark\":\"Peer\",\"online\":true}]}}");
    product_platform_prepare(true);
}

static xiaotai_preferences_stats_t peer_save_settled(void)
{
    const int64_t until = xiaotai_time_us() + WAIT_MS * INT64_C(1000);
    while (xiaotai_preferences_pending() && xiaotai_time_us() < until) xiaotai_delay_ms(5);
    assert(!xiaotai_preferences_pending());
    assert(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    product_on_owner(NULL, NULL);
    xiaotai_preferences_stats_t stats;
    xiaotai_preferences_stats(&stats);
    return stats;
}

static void check_live_peer(const char *expected)
{
    char selected[XIAOTAI_SETTING_TEXT_MAX];
    assert(xiaotai_setting_get_text(XIAOTAI_SETTING_APP_DOOR_PEER, selected, sizeof(selected)) == XIAOTAI_OK);
    assert(!strcmp(selected, expected));
}

static void record_peer_revision(void *ctx)
{
    *(uint32_t *)ctx = xiaotai_setting_store_revision(XIAOTAI_SETTING_KEY_APP_DOOR_PEER);
}

static void check_reselect_busy(void *ctx)
{
    const uint32_t revision = *(const uint32_t *)ctx;
    uint32_t ticket;
    assert(xiaotai_quiesce_begin("fixture-door-reselect", &ticket) == XIAOTAI_OK);
    assert(xiaotai_settings_confirm_text(XIAOTAI_SETTING_APP_DOOR_PEER, "peer") == XIAOTAI_ERR_BUSY);
    assert(xiaotai_setting_store_revision(XIAOTAI_SETTING_KEY_APP_DOOR_PEER) == revision);
    xiaotai_quiesce_end(ticket);
    assert(!xiaotai_quiesced());
}

int product_preferences_reselect_run(bool enabled)
{
    atomic_store(&capture_store, true);
    product_platform_online();
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    check_door_peer("before");
    check_live_peer("before");
    const xiaotai_preferences_stats_t before = peer_save_settled();

    xiaotai_kv_host_fail_key("door_peer", XIAOTAI_FAIL);
    product_wt99_tap_fixture_peer(false);
    check_live_peer("peer");
    const xiaotai_preferences_stats_t failed = peer_save_settled();
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    check_door_peer("before");
    assert(failed.writes == before.writes + WRITES_PER_SAVE && failed.write_failures == before.write_failures + 1U);
    const xiaotai_environment_status_t first = environment_attempt();
    assert(first.error == XIAOTAI_FAIL && !first.write_attempted && atomic_load(&stores) == 0U);
    printf("first_selection live=peer stored=before writes=%u failures=%u env=%d attempted=%u\n",
           failed.writes - before.writes, failed.write_failures - before.write_failures,
           first.error, (unsigned)first.write_attempted);

    uint32_t before_revision;
    product_on_owner(record_peer_revision, &before_revision);
    assert(before_revision != 0);
    setting_edit_t same = {.name = XIAOTAI_SETTING_APP_DOOR_PEER, .text = "peer"};
    product_on_owner(apply_setting, &same);
    product_on_owner(check_reselect_busy, &before_revision);
    assert(xiaotai_settings_confirm_text(XIAOTAI_SETTING_APP_DOOR_PEER, "peer") == XIAOTAI_ERR_INVALID_STATE);
    const xiaotai_preferences_stats_t unchanged = peer_save_settled();
    assert(unchanged.stores == failed.stores && unchanged.writes == failed.writes);
    check_door_peer("before");
    printf("idempotent_set_and_busy writes=0 stored=before\n");

    product_wt99_tap_fixture_peer(true);
    check_live_peer("peer");
    const xiaotai_preferences_stats_t retried = peer_save_settled();
    char stored[XIAOTAI_SETTING_TEXT_MAX];
    assert(xiaotai_kv_get_str_strict("product", "door_peer", stored, sizeof(stored), WAIT_MS) == XIAOTAI_OK);
    const xiaotai_environment_status_t second = environment_attempt();
    const unsigned attempts = atomic_load(&stores);
    printf("same_row_selection live=peer stored=%s writes=%u failures=%u env=%d attempted=%u stores=%u\n",
           stored, retried.writes - failed.writes, retried.write_failures - failed.write_failures,
           second.error, (unsigned)second.write_attempted, attempts);
    uint32_t after_revision;
    product_on_owner(record_peer_revision, &after_revision);
    assert(before_revision == after_revision && retried.write_failures == failed.write_failures);
    if (!enabled) {
        assert(!strcmp(stored, "before") && retried.writes == failed.writes && !second.write_attempted &&
               second.error == XIAOTAI_FAIL && attempts == 0U);
        return 0;
    }

    assert(!strcmp(stored, "peer") && retried.writes == failed.writes + WRITES_PER_SAVE &&
           second.write_attempted && second.error == XIAOTAI_ERR_NO_MEM && attempts == 1U);
    static const char *const raw_u8_keys[] = {"volume", "spk_mute", "mic_mute", "ack_voice", "sleep",
                                             "emoji", "door_sender"};
    for (size_t i = 0; i < sizeof(raw_u8_keys) / sizeof(raw_u8_keys[0]); ++i) (void)raw_setting(raw_u8_keys[i]);
    assert(xiaotai_kv_get_str_strict("xt_settings", "door_peer", stored, sizeof(stored), WAIT_MS) ==
           XIAOTAI_ERR_NOT_FOUND);

    product_wt99_tap_fixture_peer(true);
    const xiaotai_preferences_stats_t healthy = peer_save_settled();
    assert(healthy.writes == retried.writes + WRITES_PER_SAVE && healthy.write_failures == retried.write_failures);
    check_door_peer("peer");
    printf("healthy_same_row writes=%u stored=peer\n", healthy.writes - retried.writes);
    return 0;
}

void product_preferences_reselect_soak_prepare(void)
{
    assert(xiaotai_kv_set_str("product", "door_peer", "soak-a", WAIT_MS) == XIAOTAI_OK);
    product_platform_device_contacts_body("{\"code\":200,\"data\":{\"contacts\":["
        "{\"type\":\"device\",\"device_id\":\"soak-a\",\"remark\":\"Soak A\",\"online\":true},"
        "{\"type\":\"device\",\"device_id\":\"soak-b\",\"remark\":\"Soak B\",\"online\":true}]}}");
    product_platform_prepare(true);
}

static void wait_soak_online(void)
{
    const int64_t until = xiaotai_time_us() + WAIT_MS * INT64_C(1000);
    do {
        xiaotai_platform_status_t status;
        xiaotai_platform_status(&status);
        if (status.phase == XIAOTAI_PLATFORM_ONLINE && status.signaling) return;
        xiaotai_delay_ms(10);
    } while (xiaotai_time_us() < until);
    assert(false && "WT99 soak platform did not return online");
}

int product_preferences_reselect_soak(void)
{
    static const char *const peers[] = {"soak-a", "soak-b"};
    static const char *const labels[] = {"Soak A", "Soak B"};
    const int64_t run_us = INT64_C(300000000);
    unsigned rounds = 0, selected = 0;
    unsigned selections[2] = {0};
    int64_t shortest = INT64_MAX;
    atomic_store(&capture_store, true);
    product_platform_online();
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    check_live_peer(peers[selected]);
    check_door_peer(peers[selected]);
    const xiaotai_preferences_stats_t initial = peer_save_settled();
    xiaotai_preferences_stats_t settled = initial;
    puts("wt99_preferences_reselect_soak started minimum_s=300");
    fflush(stdout);
    const int64_t started = xiaotai_time_us();
    int64_t elapsed;
    do {
        const int64_t round_start = xiaotai_time_us();
        const unsigned next = 1U - selected;
        xiaotai_kv_host_fail_key("door_peer", XIAOTAI_FAIL);
        product_wt99_tap_named_fixture_peer(labels[next], false);
        check_live_peer(peers[next]);
        const xiaotai_preferences_stats_t failed = peer_save_settled();
        xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
        check_door_peer(peers[selected]);
        assert(failed.writes == settled.writes + WRITES_PER_SAVE &&
               failed.write_failures == settled.write_failures + 1U);
        const xiaotai_environment_status_t blocked = environment_attempt();
        assert(blocked.error == XIAOTAI_FAIL && !blocked.write_attempted && atomic_load(&stores) == rounds);
        wait_soak_online();

        product_wt99_tap_named_fixture_peer(labels[next], true);
        check_live_peer(peers[next]);
        settled = peer_save_settled();
        check_door_peer(peers[next]);
        assert(settled.writes == failed.writes + WRITES_PER_SAVE &&
               settled.write_failures == failed.write_failures && !xiaotai_preferences_pending());
        const xiaotai_environment_status_t released = environment_attempt();
        assert(released.error == XIAOTAI_ERR_NO_MEM && released.write_attempted &&
               atomic_load(&stores) == rounds + 1U);
        wait_soak_online();
        assert(!xiaotai_preferences_pending());

        selected = next;
        ++selections[selected];
        ++rounds;
        const int64_t finished = xiaotai_time_us();
        const int64_t round_us = finished - round_start;
        if (round_us < shortest) shortest = round_us;
        elapsed = finished - started;
    } while (elapsed < run_us || rounds < 2U);

    printf("wt99_preferences_reselect_soak elapsed_ms=%lld rounds=%u contact_a=%u contact_b=%u "
           "min_round_ms=%lld rounds_per_min=%lld write_ops=%u failures=%u env_blocked=%u env_intercepted=%u pending=%u\n",
           (long long)(elapsed / 1000), rounds, selections[0], selections[1], (long long)(shortest / 1000),
           (long long)((int64_t)rounds * INT64_C(60000000) / elapsed), settled.writes - initial.writes,
           settled.write_failures - initial.write_failures, rounds, atomic_load(&stores),
           (unsigned)xiaotai_preferences_pending());
    return 0;
}
#endif

int product_preferences_failure(bool unrelated_edit)
{
    atomic_store(&capture_store, true);
    product_platform_online();
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    assert(raw_setting("volume") == 5 && raw_setting("mic_mute") == 0);

    xiaotai_kv_host_fail_key("volume", XIAOTAI_FAIL);
    edit_and_wait((setting_edit_t){.name = XIAOTAI_SETTING_AUDIO_VOLUME, .value = 60}, true);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    assert(raw_setting("volume") == 5);

    const xiaotai_environment_status_t first = environment_attempt();
    assert(first.error == XIAOTAI_FAIL && !first.write_attempted && atomic_load(&stores) == 0U);
    printf("first_env error=%d write_attempted=%u volume_raw=%lld\n", first.error,
           (unsigned)first.write_attempted, (long long)raw_setting("volume"));

    /* Old P4 snapshots rewrite failed volume on the next valid edit, even
     * though it is an edit of a different setting. */
    if (unrelated_edit) {
        edit_and_wait((setting_edit_t){.name = XIAOTAI_SETTING_AUDIO_MIC_MUTED, .value = 1}, false);
        assert(raw_setting("mic_mute") == 1);
        assert(raw_setting("spk_mute") <= 1 && raw_setting("ack_voice") <= 1);
        (void)raw_setting("sleep");
        (void)raw_setting("emoji");
#if PRODUCT_TEST_WT99
        assert(raw_setting("door_sender") <= 1);
        check_door_peer("");
#endif
    }
    const int64_t durable_volume = raw_setting("volume");
    const xiaotai_environment_status_t retry = environment_attempt();
    printf("retry_env unrelated_edit=%u error=%d write_attempted=%u stores=%u volume_raw=%lld\n",
           (unsigned)unrelated_edit, retry.error, (unsigned)retry.write_attempted, atomic_load(&stores),
           (long long)durable_volume);
    if (unrelated_edit) {
        assert(retry.write_attempted && retry.error == XIAOTAI_ERR_NO_MEM && atomic_load(&stores) == 1U);
        assert(durable_volume == 6);
#if PRODUCT_TEST_WT99
        check_wt99_peer_round_trip();
#endif
    } else {
        assert(!retry.write_attempted && retry.error == XIAOTAI_FAIL && atomic_load(&stores) == 0U);
        assert(durable_volume == 5 && raw_setting("mic_mute") == 0);
        edit_and_wait((setting_edit_t){.name = XIAOTAI_SETTING_AUDIO_VOLUME, .value = 80}, false);
        assert(raw_setting("volume") == 8);
        const xiaotai_environment_status_t recovered = environment_attempt();
        printf("recovered_env error=%d write_attempted=%u stores=%u volume_raw=%lld\n", recovered.error,
               (unsigned)recovered.write_attempted, atomic_load(&stores), (long long)raw_setting("volume"));
        assert(recovered.write_attempted && recovered.error == XIAOTAI_ERR_NO_MEM && atomic_load(&stores) == 1U);
    }
    return 0;
}

typedef enum {
    COLD_MISSING, COLD_VALID, COLD_RANGE, COLD_TYPE, COLD_TIMEOUT, COLD_PRIORITY,
    COLD_NAMESPACE_TIMEOUT, COLD_VOLUME_TIMEOUT,
#if PRODUCT_TEST_WT99
    COLD_PEER_EMPTY, COLD_PEER_OVERSIZE,
#endif
} cold_case_t;

static const struct {
    const char *name;
    xiaotai_err_t error;
} cold_cases[] = {
    [COLD_MISSING] = {"preferences-cold-missing", XIAOTAI_OK},
    [COLD_VALID] = {"preferences-cold-valid", XIAOTAI_OK},
    [COLD_RANGE] = {"preferences-cold-range", XIAOTAI_ERR_INVALID_RESPONSE},
    [COLD_TYPE] = {"preferences-cold-type", XIAOTAI_OK},
    [COLD_TIMEOUT] = {"preferences-cold-timeout", XIAOTAI_ERR_TIMEOUT},
    [COLD_PRIORITY] = {"preferences-cold-priority", XIAOTAI_ERR_TIMEOUT},
    [COLD_NAMESPACE_TIMEOUT] = {"preferences-cold-namespace-timeout", XIAOTAI_ERR_TIMEOUT},
    [COLD_VOLUME_TIMEOUT] = {"preferences-cold-volume-timeout", XIAOTAI_OK},
#if PRODUCT_TEST_WT99
    [COLD_PEER_EMPTY] = {"preferences-cold-peer-empty", XIAOTAI_OK},
    [COLD_PEER_OVERSIZE] = {"preferences-cold-peer-oversize", XIAOTAI_ERR_INVALID_SIZE},
#endif
};
static cold_case_t cold_case;
static const char wrong_volume[] = "wrong-type";
#if PRODUCT_TEST_WT99
static char oversize_peer[XIAOTAI_SETTING_TEXT_MAX + 1U];
#endif

bool product_preferences_cold_prepare(const char *scenario)
{
    size_t i = 0;
    while (i < sizeof(cold_cases) / sizeof(cold_cases[0]) && strcmp(cold_cases[i].name, scenario)) ++i;
    if (i == sizeof(cold_cases) / sizeof(cold_cases[0])) return false;
    cold_case = (cold_case_t)i;

    assert(xiaotai_kv_set_int("product", "mic_mute", XIAOTAI_KV_U8, 0, WAIT_MS) == XIAOTAI_OK);
    if (cold_case == COLD_TYPE) {
        /* A pinned NVS integer lookup sees a key of another type as absent. */
        assert(xiaotai_kv_set_str("product", "volume", wrong_volume, WAIT_MS) == XIAOTAI_OK);
    } else if (cold_case != COLD_MISSING) {
        const int64_t seed = cold_case == COLD_RANGE ? 11 : 5;
        assert(xiaotai_kv_set_int("product", "volume", XIAOTAI_KV_U8, seed, WAIT_MS) == XIAOTAI_OK);
    }
    if (cold_case == COLD_PRIORITY) {
        assert(xiaotai_kv_set_int("product", "ack_voice", XIAOTAI_KV_U8, 1, WAIT_MS) == XIAOTAI_OK);
        assert(xiaotai_kv_set_int("product", "sleep", XIAOTAI_KV_U8, UINT8_MAX, WAIT_MS) == XIAOTAI_OK);
    }
#if PRODUCT_TEST_WT99
    if (cold_case == COLD_PEER_EMPTY) {
        assert(xiaotai_kv_set_str("product", "door_peer", "", WAIT_MS) == XIAOTAI_OK);
    } else if (cold_case == COLD_PEER_OVERSIZE) {
        memset(oversize_peer, 'p', sizeof(oversize_peer) - 1U);
        assert(xiaotai_kv_set_str("product", "door_peer", oversize_peer, WAIT_MS) == XIAOTAI_OK);
    }
#endif
    product_platform_prepare(true);
    if (cold_case == COLD_TIMEOUT) xiaotai_kv_host_fail_key("volume", XIAOTAI_ERR_TIMEOUT);
    /* The legacy products read ack_voice before sleep, unlike registration
     * in the new graph. Different errors make the precedence observable. */
    if (cold_case == COLD_PRIORITY) xiaotai_kv_host_fail_key("ack_voice", XIAOTAI_ERR_TIMEOUT);
    if (cold_case == COLD_NAMESPACE_TIMEOUT) xiaotai_kv_host_fail_open_space("product", XIAOTAI_ERR_TIMEOUT);
    if (cold_case == COLD_VOLUME_TIMEOUT) xiaotai_kv_host_fail_key("volume", XIAOTAI_ERR_TIMEOUT);
    return true;
}

static void check_cold_storage(void)
{
    if (cold_case == COLD_MISSING) {
        int64_t raw;
        assert(xiaotai_kv_get_int("product", "volume", XIAOTAI_KV_U8, &raw, WAIT_MS) == XIAOTAI_ERR_NOT_FOUND);
    } else if (cold_case == COLD_TYPE) {
        char stored[sizeof(wrong_volume)];
        assert(xiaotai_kv_get_str_strict("product", "volume", stored, sizeof(stored), WAIT_MS) == XIAOTAI_OK);
        assert(!strcmp(stored, wrong_volume));
    } else {
        assert(raw_setting("volume") == (cold_case == COLD_RANGE ? 11 : 5));
    }
    assert(raw_setting("mic_mute") == 0);
    if (cold_case == COLD_PRIORITY) {
        assert(raw_setting("ack_voice") == 1 && raw_setting("sleep") == UINT8_MAX);
    }
#if PRODUCT_TEST_WT99
    if (cold_case == COLD_PEER_EMPTY) {
        check_door_peer("");
    } else if (cold_case == COLD_PEER_OVERSIZE) {
        char stored[sizeof(oversize_peer)];
        assert(xiaotai_kv_get_str_strict("product", "door_peer", stored, sizeof(stored), WAIT_MS) == XIAOTAI_OK);
        assert(!strcmp(stored, oversize_peer));
    }
#endif
    xiaotai_preferences_stats_t stats;
    xiaotai_preferences_stats(&stats);
    assert(!xiaotai_preferences_pending() && stats.writes == 0U);
}

int product_preferences_cold_run(void)
{
    if (cold_case == COLD_TIMEOUT || cold_case == COLD_PRIORITY) xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    if (cold_case == COLD_NAMESPACE_TIMEOUT) xiaotai_kv_host_fail_open_space(NULL, XIAOTAI_OK);
    if (cold_case == COLD_VOLUME_TIMEOUT) xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    atomic_store(&capture_store, true);
    product_platform_online();
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);

    const xiaotai_err_t load_error = cold_cases[cold_case].error;
    bool valid_volume = cold_case == COLD_VALID || cold_case == COLD_PRIORITY;
#if PRODUCT_TEST_WT99
    valid_volume = valid_volume || cold_case == COLD_PEER_EMPTY || cold_case == COLD_PEER_OVERSIZE;
#endif
    /* Without a usable stored value the setting falls back to the product's default. */
    const int32_t expected_volume = valid_volume ? 50 : (int32_t)xiaotai_settings_volume_default();
    int32_t displayed;
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_VOLUME, &displayed) == XIAOTAI_OK);
    assert(displayed == expected_volume);
    if (cold_case == COLD_PRIORITY) {
        int32_t ack, sleep;
        assert(xiaotai_setting_get_int(XIAOTAI_SETTING_APP_COMPANION_ACK_VOICE, &ack) == XIAOTAI_OK && ack == 0);
        assert(xiaotai_setting_get_int(XIAOTAI_SETTING_SCREEN_SLEEP, &sleep) == XIAOTAI_OK && sleep == 300);
    }
#if PRODUCT_TEST_WT99
    char peer[XIAOTAI_SETTING_TEXT_MAX];
    assert(xiaotai_setting_get_text(XIAOTAI_SETTING_APP_DOOR_PEER, peer, sizeof(peer)) == XIAOTAI_OK);
    assert(!peer[0]);
#endif
    check_cold_storage();
    assert(xiaotai_preferences_flush(WAIT_MS) == XIAOTAI_OK);
    const xiaotai_environment_status_t first = environment_attempt_with_readiness(true);
    printf("%s loaded_volume=%d error=%d attempted=%u stores=%u\n", cold_cases[cold_case].name,
           (int)displayed, first.error, (unsigned)first.write_attempted, atomic_load(&stores));
    check_cold_storage();
    if (load_error == XIAOTAI_OK) {
        assert(first.write_attempted && first.error == XIAOTAI_ERR_NO_MEM && atomic_load(&stores) == 1U);
        return 0;
    }
    assert(!first.write_attempted && first.error == load_error && atomic_load(&stores) == 0U);

    /* Boot defaults are usable; a later group save repairs storage but does
     * not erase the boot-time maintenance error. */
    edit_and_wait((setting_edit_t){.name = XIAOTAI_SETTING_AUDIO_MIC_MUTED, .value = 1}, false);
    assert(raw_setting("mic_mute") == 1);
    if (cold_case == COLD_NAMESPACE_TIMEOUT) {
        /* The scoped gate does not replace ordinary per-key persistence. */
        assert(raw_setting("volume") == 5);
    } else {
        assert(raw_setting("volume") == expected_volume / 10);
    }
    if (cold_case == COLD_PRIORITY) {
        assert(raw_setting("ack_voice") == 0 && raw_setting("sleep") == 1);
    }
#if PRODUCT_TEST_WT99
    check_door_peer("");
#endif
    assert(xiaotai_preferences_flush(WAIT_MS) == XIAOTAI_OK);
    const xiaotai_environment_status_t retry = environment_attempt();
    printf("%s after_repair error=%d attempted=%u stores=%u\n", cold_cases[cold_case].name, retry.error,
           (unsigned)retry.write_attempted, atomic_load(&stores));
    assert(!retry.write_attempted && retry.error == load_error && atomic_load(&stores) == 0U);
    return 0;
}
