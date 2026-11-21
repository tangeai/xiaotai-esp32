// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_settings.c::settings
// Replaces legacy: tests/xmini/test_product.c::product
// Replaces legacy: tests/xmini/test_settings.c::settings
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "product_platform.h"
#include "product_test.h"
#include "xiaotai/apps/companion.h"
#include "xiaotai/interaction.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/modules/provisioning_config.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/preferences.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai/ui.h"
#include "xiaotai_kv_host.h"
#include "xiaotai_net_host.h"
#include "xiaotai_os_host.h"

#define WAIT_MS 3000U
static const uint8_t v2[] = {2, 40, 24, 0};
static const uint8_t v1[] = {1, 40, 24, 0};
static const uint8_t *initial;
static int32_t guidance = 0;
static enum { REVERT, V1, ABSENT, RETRY, INFLIGHT, DEBOUNCE, PORTAL_FREEZE, PORTAL_RETRY,
              PORTAL_PERSISTENT, PORTAL_SLOW } test;
static atomic_int_fast64_t s_debounce_now;
static atomic_uint s_portal_restart_commits;

typedef struct {
    const char *name;
    uint8_t bytes[4];
    xiaotai_err_t read_result;
    bool guidance;
} record_case_t;

static const record_case_t records[] = {
    {"valid-v1",     {1, 67, 13, 0}, XIAOTAI_OK, true},
    {"valid-v2-off", {2, 70, 15, 0}, XIAOTAI_OK, false},
    {"valid-v2-on",  {2, 70, 15, 1}, XIAOTAI_OK, true},
    {"absent",      {0},            XIAOTAI_ERR_NOT_FOUND, true},
    {"bad-v1-flag", {1, 70, 15, 1}, XIAOTAI_ERR_INVALID_STATE, true},
    {"bad-v2-flag", {2, 70, 15, 2}, XIAOTAI_ERR_INVALID_STATE, true},
    {"bad-version", {3, 40, 24, 0}, XIAOTAI_ERR_INVALID_STATE, true},
};
static const record_case_t *record_case;
static atomic_bool s_record_page, s_record_failure;

void product_xmini_prefs_observe_view(const xiaotai_view_t *view)
{
    if (record_case && view && view->id && !strcmp(view->id, XIAOTAI_APP_COMPANION_PAGE_SETTINGS)) {
        atomic_store(&s_record_failure, view->status && !strcmp(view->status, "保存失败"));
        atomic_store(&s_record_page, true);
    }
}

static bool prepare_record_case(const char *name)
{
    for (size_t i = 0; i < sizeof(records) / sizeof(records[0]); ++i) {
        if (strcmp(name, records[i].name)) continue;
        record_case = &records[i];
        guidance = record_case->guidance;
        if (record_case->read_result != XIAOTAI_ERR_NOT_FOUND) {
            assert(xiaotai_kv_set("xmini_ui", "audio", record_case->bytes,
                                  sizeof(record_case->bytes), WAIT_MS) == XIAOTAI_OK);
        }
        product_platform_prepare(true);
        return true;
    }
    return false;
}

void product_xmini_prefs_observe_restart(void)
{
    if (test < PORTAL_FREEZE) return;
    xiaotai_kv_host_counts_t counts;
    xiaotai_kv_host_counts(&counts);
    atomic_store(&s_portal_restart_commits, counts.commits);
}

int64_t __real_xiaotai_time_us(void);
int64_t __wrap_xiaotai_time_us(void)
{
    const int64_t now = atomic_load(&s_debounce_now);
    return now ? now : __real_xiaotai_time_us();
}

bool product_xmini_prefs_prepare(const char *scenario)
{
    static const char record_prefix[] = "xmini-prefs-record-";
    if (!strncmp(scenario, record_prefix, sizeof(record_prefix) - 1U)) {
        return prepare_record_case(scenario + sizeof(record_prefix) - 1U);
    }
    if (!strcmp(scenario, "xmini-prefs-revert")) test = REVERT;
    else if (!strcmp(scenario, "xmini-prefs-v1")) test = V1;
    else if (!strcmp(scenario, "xmini-prefs-absent")) test = ABSENT;
    else if (!strcmp(scenario, "xmini-prefs-retry")) test = RETRY;
    else if (!strcmp(scenario, "xmini-prefs-inflight")) test = INFLIGHT;
    else if (!strcmp(scenario, "xmini-prefs-debounce")) test = DEBOUNCE;
    else if (!strcmp(scenario, "xmini-prefs-portal-freeze")) test = PORTAL_FREEZE;
    else if (!strcmp(scenario, "xmini-prefs-portal-retry")) test = PORTAL_RETRY;
    else if (!strcmp(scenario, "xmini-prefs-portal-persistent")) test = PORTAL_PERSISTENT;
    else if (!strcmp(scenario, "xmini-prefs-portal-slow")) test = PORTAL_SLOW;
    else return false;
    const bool absent = test == ABSENT || test >= PORTAL_FREEZE;
    initial = test == V1 ? v1 : absent ? NULL : v2;
    guidance = test == V1 || absent;
    if (initial) assert(xiaotai_kv_set("xmini_ui", "audio", initial, sizeof(v2), WAIT_MS) == XIAOTAI_OK);
    return true;
}

static void check_values(int32_t volume, int32_t gain)
{
    int32_t value = -1;
    const xiaotai_err_t read_volume = xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_VOLUME, &value);
    if (read_volume != XIAOTAI_OK || value != volume)
        fprintf(stderr, "preference scenario=%d volume: read=%d actual=%ld expected=%ld\n",
                test, read_volume, (long)value, (long)volume);
    assert(read_volume == XIAOTAI_OK && value == volume);
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_MIC_GAIN, &value) == XIAOTAI_OK && value == gain);
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_GUIDANCE_ENABLED, &value) == XIAOTAI_OK && value == guidance);
}

static void set_values(int32_t volume, int32_t gain)
{
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_VOLUME, volume) == XIAOTAI_OK);
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_MIC_GAIN, gain) == XIAOTAI_OK);
    check_values(volume, gain);
}

static void revert_inside_legacy_window(void *unused)
{
    (void)unused;
    set_values(100, 36);
    assert(xiaotai_preferences_pending());
    xiaotai_os_host_advance_us(500000);
    set_values(40, 24);
    xiaotai_os_host_advance_us(1600000);
}

static void edit_and_make_due(void *ctx)
{
    const bool revert = *(const bool *)ctx;
    set_values(revert ? 40 : 100, revert ? 24 : 36);
    assert(xiaotai_preferences_pending());
    xiaotai_os_host_advance_us(1100000);
}

static void settle(void)
{
    const int64_t until = xiaotai_time_us() + WAIT_MS * 1000;
    while (xiaotai_preferences_pending() && xiaotai_time_us() < until) xiaotai_delay_ms(5);
    assert(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK && !xiaotai_preferences_pending());
}

static void check_record(const uint8_t *expected)
{
    uint8_t actual[sizeof(v2)];
    size_t size = sizeof(actual);
    const xiaotai_err_t got = xiaotai_kv_get("xmini_ui", "audio", actual, &size, WAIT_MS);
    if (expected) assert(got == XIAOTAI_OK && size == sizeof(v2) && !memcmp(actual, expected, size));
    else assert(got == XIAOTAI_ERR_NOT_FOUND);
}

static void queue_confirmation_edit(void *unused)
{
    (void)unused;
    atomic_store(&s_debounce_now, __real_xiaotai_time_us());
    set_values(65, 24);
    assert(xiaotai_preferences_pending());
}

static void check_confirmation_frozen(void *unused)
{
    (void)unused;
    assert(xiaotai_quiesced() && xiaotai_restart_pending());
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_VOLUME, 90) == XIAOTAI_ERR_BUSY);
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_MIC_GAIN, 36) == XIAOTAI_ERR_BUSY);
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_GUIDANCE_ENABLED, 0) == XIAOTAI_ERR_BUSY);
    check_values(65, 24);
}

static void submit_and_verify(bool fail_store)
{
    const xiaotai_net_submission_t submission = {
        .credentials = {.ssid = PRODUCT_TEST_SSID, .password = PRODUCT_TEST_PASSWORD},
        .method = XIAOTAI_PROVISION_PORTAL,
    };
    const xiaotai_net_host_step_t join = {.result = XIAOTAI_OK};
    assert(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_AP_START, 1, WAIT_MS));
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    assert(xiaotai_net_host_step(XIAOTAI_NET_HOST_JOIN, &join) == XIAOTAI_OK);
    assert(xiaotai_net_provision_submit(&submission) == XIAOTAI_OK);
    /* Let the network's now+1 us deadline fire without reaching the
     * unrelated 1000 ms audio preference deadline. */
    atomic_fetch_add(&s_debounce_now, 10000);
    assert(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_JOIN, 1, WAIT_MS));
    xiaotai_net_host_record_t records[16];
    const size_t count = xiaotai_net_host_records(records, sizeof(records) / sizeof(records[0]));
    uint32_t token = 0;
    for (size_t i = 0; i < count; ++i) {
        if (records[i].call == XIAOTAI_NET_HOST_JOIN) token = records[i].token;
    }
    assert(token);
    const xiaotai_net_host_step_t leave = {
        .result = XIAOTAI_OK, .event_count = 1,
        .events = {{.kind = XIAOTAI_NET_DRIVER_LOST, .token = token, .loss = XIAOTAI_NET_LOSS_REQUESTED}},
    };
    assert(xiaotai_net_host_step(XIAOTAI_NET_HOST_LEAVE, &leave) == XIAOTAI_OK);
    assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){
        .kind = XIAOTAI_NET_DRIVER_JOINED, .token = token, .ssid = PRODUCT_TEST_SSID,
    }) == XIAOTAI_OK);
    if (fail_store) xiaotai_kv_host_fail_key("wifi", XIAOTAI_FAIL);
    assert(xiaotai_net_host_emit(&(xiaotai_net_driver_event_t){
        .kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = token, .ipv4 = 42,
    }) == XIAOTAI_OK);
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
}

static void explicit_confirmation_retry(void *out)
{
    *(xiaotai_err_t *)out = xiaotai_provisioning_retry();
}

static void user_retry(void)
{
    const int64_t deadline = __real_xiaotai_time_us() + WAIT_MS * 1000;
    xiaotai_ui_info_t ui = {0};
    do {
        assert(xiaotai_ui_inspect(0, &ui) == XIAOTAI_OK);
        if (ui.ready && !ui.present_fault && !strcmp(ui.page, XIAOTAI_APP_COMPANION_PAGE_SETUP)) break;
        xiaotai_delay_ms(5);
    } while (__real_xiaotai_time_us() < deadline);
    fprintf(stderr, "held settings UI: page=%s ready=%u focus=%u fault=%u\n", ui.page, (unsigned)ui.ready,
            (unsigned)ui.focus, (unsigned)ui.present_fault);
    assert(ui.ready && !ui.present_fault && !strcmp(ui.page, XIAOTAI_APP_COMPANION_PAGE_SETUP));
    assert(xiaotai_interaction_inject("main", 2, 0) == XIAOTAI_OK);
}

static void wait_wifi_store_state(const char *stage, xiaotai_net_provision_state_t expected,
                                  xiaotai_err_t error, unsigned writes)
{
    const int64_t deadline = __real_xiaotai_time_us() + WAIT_MS * 1000;
    xiaotai_net_provision_status_t net = {0};
    xiaotai_kv_host_counts_t counts = {0};
    do {
        assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
        xiaotai_net_provision_status(&net);
        xiaotai_kv_host_counts(&counts);
        if (net.state == expected && counts.writes >= writes) break;
        if (net.state == XIAOTAI_NET_PROVISION_STORING) atomic_fetch_add(&s_debounce_now, 1);
    } while (__real_xiaotai_time_us() < deadline);
    fprintf(stderr, "credential store %s: state=%d error=%d writes=%u restart_required=%u\n", stage,
            net.state, net.error, counts.writes, (unsigned)net.restart_required);
    assert(net.state == expected && net.error == error && counts.writes == writes);
    assert(net.restart_required == (expected == XIAOTAI_NET_PROVISION_DONE) && !product_restarted());
    if (expected == XIAOTAI_NET_PROVISION_STORE_FAILED) assert(!xiaotai_restart_pending());
}

static void retry_wifi_store(const xiaotai_kv_host_counts_t *before)
{
    assert(XIAOTAI_PROVISIONING_STORE_RETRY_MS == 0);
    wait_wifi_store_state("initial failure", XIAOTAI_NET_PROVISION_STORE_FAILED, XIAOTAI_FAIL, before->writes + 1U);
    xiaotai_kv_host_counts_t after;
    xiaotai_kv_host_counts(&after);
    assert(after.writes == before->writes + 1U && after.commits == before->commits);

    /* A later status read must not make another write or start a restart. */
    atomic_fetch_add(&s_debounce_now, 500000);
    product_on_owner(NULL, NULL);
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    xiaotai_kv_host_counts(&after);
    assert(after.writes == before->writes + 1U);
    wait_wifi_store_state("no automatic retry", XIAOTAI_NET_PROVISION_STORE_FAILED, XIAOTAI_FAIL, before->writes + 1U);
    xiaotai_err_t admitted = XIAOTAI_FAIL;
    product_on_owner(explicit_confirmation_retry, &admitted);
    assert(admitted == XIAOTAI_OK);
    atomic_fetch_add(&s_debounce_now, 10000);
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    wait_wifi_store_state("first explicit retry", XIAOTAI_NET_PROVISION_STORE_FAILED, XIAOTAI_FAIL, before->writes + 2U);
    xiaotai_kv_host_counts(&after);
    assert(after.writes == before->writes + 2U && after.commits == before->commits);

    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    xiaotai_net_credentials_t stored;
    size_t length = sizeof(stored);
    assert(xiaotai_kv_get("xmini", "wifi", &stored, &length, WAIT_MS) == XIAOTAI_ERR_NOT_FOUND);
    /* Dispatch the setup action directly while the clock is frozen. The
     * physical double-click is covered by the packed preference retry below. */
    assert(xiaotai_intent_post(XIAOTAI_INTENT_PROVISIONING_RETRY, NULL) == XIAOTAI_OK);
    product_on_owner(NULL, NULL);
    atomic_fetch_add(&s_debounce_now, 10000);
    assert(xiaotai_net_host_sync(WAIT_MS) == XIAOTAI_OK);
    wait_wifi_store_state("setup action retry", XIAOTAI_NET_PROVISION_DONE, XIAOTAI_OK, before->writes + 4U);
    length = sizeof(stored);
    assert(xiaotai_kv_get("xmini", "wifi", &stored, &length, WAIT_MS) == XIAOTAI_OK);
    assert(length == sizeof(stored) && !strcmp(stored.ssid, PRODUCT_TEST_SSID) &&
           !strcmp(stored.password, PRODUCT_TEST_PASSWORD));
    xiaotai_kv_host_counts(&after);
    assert(after.writes == before->writes + 4U && after.commits == before->commits + 1U);
    printf("verified credentials: two explicit retries, writes=%u commits=%u; restart waits for preferences\n",
           after.writes - before->writes, after.commits - before->commits);
}

static void wait_for_audio(void)
{
    // The settings UI admits audio edits only after the media worker opens its parts.
    xiaotai_media_status_t media;
    const int64_t audio_deadline = __real_xiaotai_time_us() + WAIT_MS * 1000;
    do {
        xiaotai_media_status(&media);
        if (media.ready || media.startup_complete) break;
        xiaotai_delay_ms(1);
    } while (__real_xiaotai_time_us() < audio_deadline);
    if (!media.ready) {
        fprintf(stderr, "preference audio not ready: startup_complete=%d error=%d\n",
                media.startup_complete, media.startup_error);
    }
    assert(media.ready);
}

static int check_confirmation(bool fail_audio, bool stays_failed)
{
    product_on_owner(queue_confirmation_edit, NULL);
    const bool fail_wifi = test == PORTAL_RETRY;
    xiaotai_kv_host_counts_t before_wifi;
    xiaotai_kv_host_counts(&before_wifi);
    submit_and_verify(fail_wifi);
    if (fail_wifi) retry_wifi_store(&before_wifi);
    xiaotai_provisioning_status_t confirmation;
    xiaotai_net_provision_status_t net;
    const int64_t wait = __real_xiaotai_time_us() + WAIT_MS * 1000;
    do {
        xiaotai_delay_ms(5);
        xiaotai_net_provision_status(&net);
        xiaotai_provisioning_status(&confirmation);
    } while ((!confirmation.holding_restart || !xiaotai_restart_pending() || !xiaotai_quiesced()) &&
             __real_xiaotai_time_us() < wait);
    assert(net.state == XIAOTAI_NET_PROVISION_DONE && net.restart_required);
    assert(confirmation.holding_restart && !product_restarted() && xiaotai_preferences_pending());
    product_on_owner(check_confirmation_frozen, NULL);
    check_record(NULL);

    xiaotai_kv_host_counts_t before, after;
    xiaotai_preferences_stats_t stats;
    xiaotai_kv_host_counts(&before);
    if (fail_audio) xiaotai_kv_host_fail_key("audio", XIAOTAI_FAIL);
    if (test == PORTAL_SLOW) xiaotai_kv_host_gate(false);
    product_expect_restart(); /* Capture rather than abort on an unexpected policy result. */
    atomic_store(&s_debounce_now, 0);
    xiaotai_os_host_advance_us(1100000);
    if (test == PORTAL_SLOW) {
        assert(xiaotai_kv_host_wait_held(WAIT_MS));
        const uint32_t hold_deadline_ms = XIAOTAI_PROVISIONING_SETTLE_WAIT_MS;
        const int64_t beyond_deadline_ms = hold_deadline_ms ? (int64_t)hold_deadline_ms + 1000 : 6000;
        xiaotai_os_host_advance_us(beyond_deadline_ms * 1000);
        xiaotai_delay_ms(120);
        product_on_owner(NULL, NULL);
        xiaotai_provisioning_status(&confirmation);
        xiaotai_provisioning_stats_t late;
        xiaotai_provisioning_stats(&late);
        printf("held packed write: deadline_ms=%lu timeouts=%lu restart=%u\n",
               (unsigned long)hold_deadline_ms, (unsigned long)late.flush_timeouts, (unsigned)product_restarted());
        assert(confirmation.holding_restart && !confirmation.packed_retry_required &&
               xiaotai_preferences_pending() && !product_restarted() &&
               late.flush_timeouts == (hold_deadline_ms ? 1U : 0U));
        xiaotai_kv_host_gate(true);
    }
    settle();
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);

    const int64_t after_write = __real_xiaotai_time_us() + INT64_C(2300000);
    while (!product_restarted() && __real_xiaotai_time_us() < after_write) xiaotai_delay_ms(5);
    xiaotai_provisioning_status(&confirmation);
    xiaotai_preferences_stats(&stats);
    xiaotai_kv_host_counts(&after);
    const unsigned commits_at_restart = product_restarted() ? atomic_load(&s_portal_restart_commits) - before.commits : 0U;
    printf("portal confirmation: fail_audio=%u writes=%u commits=%u failed=%u phase=%u hold=%u preference_pending=%u restarted=%u commits_at_restart=%u\n",
           (unsigned)fail_audio, after.writes - before.writes, after.commits - before.commits, stats.write_failures,
           (unsigned)confirmation.phase, (unsigned)confirmation.holding_restart,
           (unsigned)xiaotai_preferences_pending(), (unsigned)product_restarted(), commits_at_restart);
    const uint8_t saved[] = {2, 65, 24, 1};
    if (!fail_audio) {
        check_record(saved);
        return !(after.writes == before.writes + 1U && after.commits == before.commits + 1U &&
                 !stats.write_failures && product_restarted() && commits_at_restart == 1U);
    }

    check_record(NULL);
    assert(after.writes == before.writes + 1U && after.commits == before.commits && stats.write_failures == 1U);
    assert(!product_restarted() && confirmation.holding_restart && confirmation.packed_retry_required);
    if (stays_failed) xiaotai_kv_host_fail_key("audio", XIAOTAI_FAIL);
    xiaotai_kv_host_gate(false);
    user_retry();
    assert(xiaotai_kv_host_wait_held(WAIT_MS));
    assert(xiaotai_preferences_pending() && !product_restarted());
    product_on_owner(check_confirmation_frozen, NULL);
    xiaotai_err_t duplicate = XIAOTAI_OK;
    product_on_owner(explicit_confirmation_retry, &duplicate);
    assert(duplicate != XIAOTAI_OK);
    xiaotai_kv_host_gate(true);
    settle();
    const int64_t retry_done = __real_xiaotai_time_us() + WAIT_MS * 1000;
    do {
        xiaotai_delay_ms(5);
        xiaotai_provisioning_status(&confirmation);
    } while (!product_restarted() && (!stays_failed || !confirmation.packed_retry_required) &&
             __real_xiaotai_time_us() < retry_done);
    if (stays_failed) {
        /* Go beyond both the hold deadline and scheduled restart delay. */
        xiaotai_os_host_advance_us(7000000);
        product_on_owner(NULL, NULL);
        assert(!xiaotai_preferences_pending() && !product_restarted());
        /* The injector fails reads of this key too; clear it only after the
         * no-auto-retry observation to inspect the underlying record. */
        xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
        check_record(NULL);
        assert(xiaotai_quiesced() && confirmation.holding_restart && confirmation.packed_retry_required);
    } else {
        check_record(saved);
        assert(atomic_load(&s_portal_restart_commits) == before.commits + 1U);
    }
    xiaotai_kv_host_counts(&after);
    xiaotai_preferences_stats(&stats);
    assert(after.writes == before.writes + 2U && after.commits == before.commits + !stays_failed);
    assert(stats.write_failures == (stays_failed ? 2U : 1U));
    printf("manual retry: persistent=%u duplicate=%d writes=%u commits=%u restart=%u hold=%u\n",
           (unsigned)stays_failed, duplicate, after.writes - before.writes, after.commits - before.commits,
           (unsigned)product_restarted(), (unsigned)confirmation.holding_restart);
    return product_restarted() == stays_failed;
}

static void edit_staggered_audio_fields(void *unused)
{
    (void)unused;
    atomic_store(&s_debounce_now, __real_xiaotai_time_us());
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_VOLUME, 70) == XIAOTAI_OK);
    atomic_fetch_add(&s_debounce_now, 500000);
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_MIC_GAIN, 30) == XIAOTAI_OK);
    atomic_fetch_add(&s_debounce_now, 400000);
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_GUIDANCE_ENABLED, 1) == XIAOTAI_OK);
    atomic_fetch_add(&s_debounce_now, 100000);
}

static void advance(void *ctx) { atomic_fetch_add(&s_debounce_now, *(const int64_t *)ctx); }

static void check_silent_window(const xiaotai_kv_host_counts_t *before, const char *phase)
{
    product_on_owner(NULL, NULL); /* Dispatch timers without a preferences flush. */
    assert(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK);
    xiaotai_kv_host_counts_t now;
    xiaotai_kv_host_counts(&now);
    if (now.writes != before->writes || now.commits != before->commits)
        fprintf(stderr, "%s: audio record wrote early: writes=%u commits=%u\n",
                phase, now.writes - before->writes, now.commits - before->commits);
    assert(xiaotai_preferences_pending() && now.writes == before->writes && now.commits == before->commits);
    check_record(v2);
}

static int debounce(void)
{
    xiaotai_kv_host_counts_t before, after;
    xiaotai_kv_host_counts(&before);
    product_on_owner(edit_staggered_audio_fields, NULL);
    check_silent_window(&before, "volume deadline");
    int64_t delta = 500000;
    product_on_owner(advance, &delta);
    check_silent_window(&before, "gain deadline");
    delta = 399000;
    product_on_owner(advance, &delta);
    check_silent_window(&before, "1899 ms");
    delta = 1000;
    product_on_owner(advance, &delta);
    product_on_owner(NULL, NULL);
    assert(xiaotai_kv_flush(WAIT_MS) == XIAOTAI_OK && !xiaotai_preferences_pending());
    xiaotai_kv_host_counts(&after);
    assert(after.writes == before.writes + 1U && after.commits == before.commits + 1U);
    guidance = 1;
    check_values(70, 30);
    const uint8_t edited[] = {2, 70, 30, 1};
    check_record(edited);
    xiaotai_os_host_advance_us(atomic_load(&s_debounce_now) - __real_xiaotai_time_us());
    atomic_store(&s_debounce_now, 0);
    puts("staggered volume, gain and guidance: no writes at 1000/1500/1899 ms; one complete V2 record at 1900 ms");
    return 0;
}

static void save_bad_record(void *out)
{
    *(xiaotai_err_t *)out = xiaotai_preferences_store_int("volume", 65);
}

static int check_record_case(void)
{
    product_platform_online();
    const bool valid = record_case->read_result == XIAOTAI_OK;
    const bool malformed = record_case->read_result == XIAOTAI_ERR_INVALID_STATE;
    int32_t stored = -1;
    const xiaotai_err_t read = xiaotai_preferences_load_int("volume", 40, &stored);
    assert(xiaotai_storage_boot_error() == XIAOTAI_OK);
    check_values(valid ? record_case->bytes[1] : 40, valid ? record_case->bytes[2] : 24);

    const xiaotai_args_t settings = {.text = XIAOTAI_APP_COMPANION_PAGE_SETTINGS};
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_SHOW, &settings) == XIAOTAI_OK);
    xiaotai_ui_info_t ui;
    const int64_t deadline = xiaotai_time_us() + WAIT_MS * INT64_C(1000);
    do {
        assert(xiaotai_ui_inspect(0, &ui) == XIAOTAI_OK);
        if (atomic_load(&s_record_page) && ui.frames && !strcmp(ui.page, settings.text)) break;
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < deadline);
    assert(atomic_load(&s_record_page) && ui.frames && !strcmp(ui.page, settings.text));
    assert(ui.ready && !ui.present_fault && !strcmp(ui.theme, "mono128"));

    if (malformed) {
        xiaotai_err_t result;
        product_on_owner(save_bad_record, &result);
        assert(result == XIAOTAI_ERR_INVALID_STATE);
    }
    xiaotai_preferences_stats_t stats;
    xiaotai_preferences_stats(&stats);
    assert(stats.invalid == (unsigned)malformed && !stats.writes && !stats.stores &&
           !xiaotai_preferences_pending() && !xiaotai_restart_pending());
    check_record(valid || malformed ? record_case->bytes : NULL);
    const bool label = atomic_load(&s_record_failure);
    printf("packed record %s: read=%d stored=%ld failed_label=%u invalid=%u writes=%u unchanged=1\n",
           record_case->name, read, (long)stored, (unsigned)label, stats.invalid, stats.writes);
    fflush(stdout);
    assert(read == record_case->read_result && label == malformed);
    assert(stored == (valid ? record_case->bytes[1] : -1));
    return 0;
}

int product_xmini_prefs_revert(void)
{
    if (record_case) return check_record_case();
    wait_for_audio();
    check_values(40, 24);
    if (test >= PORTAL_FREEZE)
        return check_confirmation(test == PORTAL_RETRY || test == PORTAL_PERSISTENT, test == PORTAL_PERSISTENT);
    settle();
    check_record(initial);
    if (test == DEBOUNCE) return debounce();
    xiaotai_kv_host_counts_t before, after;
    xiaotai_preferences_stats_t before_stats, after_stats;
    xiaotai_kv_host_counts(&before);
    xiaotai_preferences_stats(&before_stats);
    bool reverting = false;
    unsigned expected_writes = 0, expected_commits = 0, expected_failures = 0;
    if (test == RETRY) {
        xiaotai_kv_host_fail_key("audio", XIAOTAI_FAIL);
        product_on_owner(edit_and_make_due, &reverting);
        settle();
        assert(xiaotai_preferences_flush(WAIT_MS) == XIAOTAI_FAIL);
        xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
        check_record(v2);
        reverting = true;
        product_on_owner(edit_and_make_due, &reverting);
        expected_writes = 2;
        expected_commits = 1;
        expected_failures = 2;
    } else if (test == INFLIGHT) {
        xiaotai_kv_host_gate(false);
        product_on_owner(edit_and_make_due, &reverting);
        assert(xiaotai_kv_host_wait_held(WAIT_MS));
        reverting = true;
        product_on_owner(edit_and_make_due, &reverting);
        xiaotai_kv_host_gate(true);
        expected_writes = expected_commits = 2;
    } else {
        product_on_owner(revert_inside_legacy_window, NULL);
    }
    settle();
    check_values(40, 24);
    xiaotai_kv_host_counts(&after);
    xiaotai_preferences_stats(&after_stats);
    const unsigned writes = after.writes - before.writes;
    const unsigned commits = after.commits - before.commits;
    const unsigned failures = after_stats.write_failures - before_stats.write_failures;
    printf("%s: scenario=%d initial=%s desired=40/24/%d pending=%u backend_writes=%u commits=%u prefs_writes=%u coalesced=%u failures=%u\n",
           XIAOTAI_PRODUCT_NAME, test, test == ABSENT ? "absent" : test == V1 ? "V1" : "V2", guidance,
           (unsigned)xiaotai_preferences_pending(), writes, commits,
           after_stats.writes - before_stats.writes, after_stats.coalesced - before_stats.coalesced, failures);
    if (writes != expected_writes || commits != expected_commits || failures != expected_failures ||
        after_stats.writes - before_stats.writes != expected_writes) return 1;
    check_record(initial);
    if (test == V1 || test == ABSENT) {
        reverting = false;
        product_on_owner(edit_and_make_due, &reverting);
        settle();
        const uint8_t edited[] = {2, 100, 36, 1};
        check_record(edited);
    }
    return 0;
}
