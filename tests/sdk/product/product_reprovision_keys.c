// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_runtime_wifi.c::runtime_wifi
#include "product_test.h"
#include "product_platform.h"
#include "platform_internal.h"
#include "audio_io_fake.h"
#include "xiaotai/apps/companion.h"
#include "xiaotai/interaction.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/environment.h"
#include "xiaotai/modules/provisioning.h"
#include "xiaotai/modules/settings.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai/preferences.h"
#include "xiaotai/ui.h"
#include "xiaotai_kv_host.h"
#include "xiaotai_net_host.h"
#include "xiaotai_os_host.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static atomic_bool s_observe;
static atomic_uint s_requests;
static xiaotai_signal_t http_entered, http_release, http_done;
static const char wait_path[] = "/fixture/setup-wait";

static void block_http(void *unused, const xiaotai_http_host_request_t *request, xiaotai_http_host_reply_t *reply)
{
    (void)unused;
    (void)reply;
    if (!strstr(request->url, wait_path)) return;
    xiaotai_signal_give(&http_entered);
    assert(xiaotai_signal_take(&http_release, 8000));
}

static void request_done(const xiaotai_platform_reply_t *reply, void *unused)
{
    (void)reply;
    (void)unused;
    xiaotai_signal_give(&http_done);
}

static void hold_platform_request(void)
{
    const xiaotai_platform_request_t request = {
        .service = XIAOTAI_PLATFORM_SERVICE_CALL, .path = wait_path, .timeout_ms = 8000,
    };
    assert(xiaotai_platform_request(&request, request_done, NULL) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&http_entered, 5000));
}

static void release_platform_request(void)
{
    xiaotai_signal_give(&http_release);
    assert(xiaotai_signal_take(&http_done, 5000));
}

void product_reprovision_keys_prepare(void)
{
    product_platform_prepare(true);
    product_platform_http_responder(block_http, NULL);
    assert(xiaotai_signal_init(&http_entered) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&http_release) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&http_done) == XIAOTAI_OK);
}

xiaotai_err_t __real_xiaotai_provisioning_begin(uint32_t flags);
xiaotai_err_t __wrap_xiaotai_provisioning_begin(uint32_t flags)
{
    if (!atomic_load(&s_observe)) return __real_xiaotai_provisioning_begin(flags);
    assert(xiaotai_on_owner() && flags == 0U);
    atomic_fetch_add(&s_requests, 1U);
    /* The checked UI must reach the service only after explicit consent.
     * Stop at this boundary even if the UI regresses. */
    return XIAOTAI_FAIL;
}

static xiaotai_ui_info_t wait_view(bool overlay, unsigned focus, uint32_t posted)
{
    xiaotai_ui_info_t ui = {0};
    const int64_t deadline = xiaotai_time_us() + INT64_C(5000000);
    do {
        assert(xiaotai_ui_inspect(0, &ui) == XIAOTAI_OK);
        if (ui.ready && !strcmp(ui.page, XIAOTAI_APP_COMPANION_PAGE_SETTINGS) && ui.posted >= posted &&
            (overlay ? !strncmp(ui.overlay, "confirm.", 8) : !ui.overlay[0]) &&
            (!focus || ui.focus == focus)) {
            assert(!strcmp(ui.theme, "mono128") && ui.width == 128U && ui.height == 64U);
            assert(ui.frames && !ui.present_fault);
            return ui;
        }
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < deadline);
    fprintf(stderr, "expected settings overlay=%u focus=%u posted>=%u; got page=%s overlay=%s focus=%u posted=%u\n",
            (unsigned)overlay, focus, (unsigned)posted, ui.page, ui.overlay,
            (unsigned)ui.focus, (unsigned)ui.posted);
    assert(false);
    return ui;
}

static unsigned network_leaves(void)
{
    xiaotai_net_host_record_t records[128];
    const size_t count = xiaotai_net_host_records(records, sizeof(records) / sizeof(records[0]));
    assert(count < sizeof(records) / sizeof(records[0]));
    unsigned leaves = 0;
    for (size_t i = 0; i < count; ++i) if (records[i].call == XIAOTAI_NET_HOST_LEAVE) ++leaves;
    return leaves;
}

static xiaotai_ui_info_t open_confirmation(void)
{
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_REPROVISION, NULL) == XIAOTAI_OK);
    return wait_view(true, 0, 0);
}

static void check_waiting_owner(void *unused)
{
    (void)unused;
    xiaotai_platform_status_t platform;
    xiaotai_platform_status(&platform);
    assert(platform.gated && xiaotai_quiesced_for(XIAOTAI_QUIESCE_PROVISIONING) &&
           xiaotai_provisioning_begin_cancellable());
}

static void wait_maintenance(xiaotai_err_t error)
{
    const int64_t deadline = xiaotai_time_us() + INT64_C(5000000);
    xiaotai_provisioning_status_t provision = {0};
    xiaotai_ui_info_t ui = {0};
    do {
        xiaotai_provisioning_status(&provision);
        assert(xiaotai_ui_inspect(0, &ui) == XIAOTAI_OK);
        if (provision.phase == XIAOTAI_PROVISIONING_STARTING && provision.error == error &&
            ui.ready && !strcmp(ui.page, "maintenance") && !ui.overlay[0]) {
            assert(!provision.restart_pending);
            product_on_owner(check_waiting_owner, NULL);
            return;
        }
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < deadline);
    fprintf(stderr, "reprovision wait: phase=%d error=%d page=%s overlay=%s posted=%u\n",
            provision.phase, provision.error, ui.page, ui.overlay, (unsigned)ui.posted);
    assert(false);
}

static void edit_audio(void *unused)
{
    (void)unused;
    int32_t old;
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_VOLUME, &old) == XIAOTAI_OK);
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_VOLUME, old == 65 ? 75 : 65) == XIAOTAI_OK);
    assert(xiaotai_preferences_pending());
}

static void refuse_volume_while_held(void *unused)
{
    (void)unused;
    uint32_t ticket;
    assert(xiaotai_quiesce_begin(XIAOTAI_QUIESCE_PROVISIONING, &ticket) == XIAOTAI_OK);
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_VOLUME, 65) == XIAOTAI_ERR_BUSY);
    int32_t volume;
    assert(xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_VOLUME, &volume) == XIAOTAI_OK && volume == 40);
    assert(!xiaotai_preferences_pending());
    xiaotai_quiesce_end(ticket);
}

static void fail_volume_apply(void *unused)
{
    (void)unused;
    xiaotai_settings_status_t before, after;
    xiaotai_settings_status(&before);
    assert(before.volume == 65 && before.apply_error == XIAOTAI_OK);
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_VOLUME, 65) == XIAOTAI_OK);
    xiaotai_settings_status(&after);
    assert(before.revision == after.revision);

    /* The old input fails inside the live device; do not simulate a refusal
     * from the media API, which has a different persistence contract. */
    xt_audio_fake_fail(FAKE_VOLUME, XIAOTAI_FAIL, false);
    assert(xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_VOLUME, 75) == XIAOTAI_OK);
    assert(xiaotai_preferences_pending());
}

static void expect_audio_preference(uint8_t wanted, uint8_t applied, xiaotai_err_t error, int64_t deadline)
{
    xiaotai_media_status_t media;
    xiaotai_settings_status_t settings;
    do {
        xiaotai_media_status(&media);
        xiaotai_settings_status(&settings);
        if (media.ready && media.playing && media.volume_applied == applied && settings.volume == wanted &&
            media.output_error == error && settings.apply_error == error && !xiaotai_preferences_pending()) {
            const uint8_t expected[] = {2, wanted, 24, 1};
            uint8_t record[sizeof(expected)];
            size_t size = sizeof(record);
            assert(xiaotai_kv_flush(2000) == XIAOTAI_OK);
            assert(xiaotai_kv_get_blob_strict("xmini_ui", "audio", record, &size, 2000) == XIAOTAI_OK);
            assert(size == sizeof(expected) && !memcmp(record, expected, size));
            return;
        }
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < deadline);
    fprintf(stderr, "volume preference=%u applied=%u media_error=%d setting_error=%d pending=%u ready=%u playing=%u\n",
            settings.volume, media.volume_applied, media.output_error, settings.apply_error,
            (unsigned)xiaotai_preferences_pending(), (unsigned)media.ready, (unsigned)media.playing);
    assert(false);
}

typedef struct {
    const xiaotai_media_play_t *request;
    uint32_t id, previous;
} fixture_cue_t;

static void start_idle_cue(void *ctx)
{
    fixture_cue_t *cue = ctx;
    xiaotai_media_status_t media;
    xiaotai_media_status(&media);
    if (media.playing) cue->previous = media.playing;
    if (!media.ready || media.playing) return;
    // Check and submit on the owner so another application cue cannot interleave.
    assert(xiaotai_media_play_clips(cue->request, &cue->id) == XIAOTAI_OK && cue->id);
}

void product_runtime_wifi_audio(void)
{
    product_platform_online();
    product_on_owner(refuse_volume_while_held, NULL);
    /* This product closes the idle codec. Keep it open so the injected error
     * comes from the live HAL, as in the old fixture. */
    static const int16_t pcm[160] = {0};
    static const xiaotai_sound_t sound = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 8000, .channels = 1,
        .data = (const uint8_t *)pcm, .size = sizeof(pcm)};
    const xiaotai_media_play_t play = {.clips = {{.sound = &sound}}, .clip_count = 1, .priority = XIAOTAI_PLAY_CUE,
        .repeat = XIAOTAI_PLAY_FOREVER, .gap_ms = 20};
    fixture_cue_t cue = {.request = &play};
    const int64_t deadline = xiaotai_time_us() + INT64_C(5000000);
    do {
        product_on_owner(start_idle_cue, &cue);
        if (cue.id) break;
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < deadline);
    assert(cue.id && "startup audio did not finish within the fixture deadline");
    if (cue.previous) printf("runtime Wi-Fi audio: waited for startup cue %lu\n", (unsigned long)cue.previous);
    product_on_owner(edit_audio, NULL);
    expect_audio_preference(65, 65, XIAOTAI_OK, deadline);
    product_on_owner(fail_volume_apply, NULL);
    expect_audio_preference(75, 65, XIAOTAI_FAIL, xiaotai_time_us() + INT64_C(5000000));
    xiaotai_media_stop(cue.id);
    puts("runtime Wi-Fi audio: held rejects edit, duplicate keeps revision, live HAL failure retains packed preference 75 and applied 65");
}

static void confirm_maintenance(bool fail_write)
{
    if (fail_write) {
        xiaotai_kv_host_gate(false);
        product_on_owner(edit_audio, NULL);
        /* Let the setting's redraw precede the new confirmation gestures. */
        product_on_owner(NULL, NULL);
    }
    (void)open_confirmation();
    assert(xiaotai_interaction_inject("main", 1, 0) == XIAOTAI_OK);
    (void)wait_view(true, 2, 0);
    assert(xiaotai_interaction_inject("main", 2, 0) == XIAOTAI_OK);
    wait_maintenance(XIAOTAI_OK);
}

static void check_cancel_released(void)
{
    (void)wait_view(false, 0, 0);
    xiaotai_platform_status_t platform;
    xiaotai_provisioning_status_t provision;
    xiaotai_platform_status(&platform);
    xiaotai_provisioning_status(&provision);
    assert(!platform.gated && !xiaotai_quiesced() && !product_restarted());
    assert(provision.phase == XIAOTAI_PROVISIONING_NONE && !provision.restart_pending);
}

static void check_real_cancellation(void)
{
    xiaotai_provisioning_stats_t before, after;
    xiaotai_provisioning_stats(&before);
    const unsigned leaves = network_leaves();
    uint8_t wifi[98], identity[322], saved[sizeof(identity)], marker;
    size_t size = sizeof(wifi);
    assert(xiaotai_kv_get_blob_strict("xmini", "wifi", wifi, &size, 2000) == XIAOTAI_OK && size == sizeof(wifi));
    size = sizeof(identity);
    assert(xiaotai_kv_get_blob_strict("xmini", "identity", identity, &size, 2000) == XIAOTAI_OK && size == sizeof(identity));

    hold_platform_request();
    confirm_maintenance(false);
    assert(xiaotai_interaction_inject("main", 2, 0) == XIAOTAI_OK);
    check_cancel_released();
    release_platform_request();

    hold_platform_request();
    confirm_maintenance(true);
    assert(xiaotai_kv_host_wait_held(3000));
    xiaotai_kv_host_fail_key("audio", XIAOTAI_FAIL);
    xiaotai_kv_host_gate(true);
    wait_maintenance(XIAOTAI_FAIL);
    xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    release_platform_request();
    wait_maintenance(XIAOTAI_FAIL);
    assert(xiaotai_interaction_inject("main", 0, 1300) == XIAOTAI_OK);
    check_cancel_released();

    xiaotai_provisioning_stats(&after);
    assert(after.begins == before.begins + 2 && after.cancels == before.cancels + 2 &&
           after.flush_failures == before.flush_failures + 1 && network_leaves() == leaves);
    assert(xiaotai_kv_get_u8("xmini", "wifi_provision", &marker, 2000) == XIAOTAI_ERR_NOT_FOUND);
    size = sizeof(saved);
    assert(xiaotai_kv_get_blob_strict("xmini", "wifi", saved, &size, 2000) == XIAOTAI_OK &&
           size == sizeof(wifi) && !memcmp(wifi, saved, size));
    size = sizeof(saved);
    assert(xiaotai_kv_get_blob_strict("xmini", "identity", saved, &size, 2000) == XIAOTAI_OK &&
           size == sizeof(identity) && !memcmp(identity, saved, size));
    puts("reprovision keys: actual double-click and long hold canceled WAIT and HELD, no marker, restart or Wi-Fi leave");
}

static void provision_excludes_environment(void *unused)
{
    (void)unused;
    assert(xiaotai_provisioning_begin(0) == XIAOTAI_OK);
    check_waiting_owner(NULL);
    assert(xiaotai_environment_set("http://fixture.test/pre") == XIAOTAI_ERR_BUSY);
    assert(xiaotai_environment_cancel() == XIAOTAI_ERR_INVALID_STATE);
    check_waiting_owner(NULL);
    assert(xiaotai_provisioning_cancel() == XIAOTAI_OK && !xiaotai_quiesced());
}

static void environment_excludes_provision(void *unused)
{
    (void)unused;
    assert(xiaotai_environment_set("http://fixture.test/pre") == XIAOTAI_OK);
    xiaotai_environment_status_t status;
    xiaotai_environment_status(&status);
    assert(status.phase == XIAOTAI_ENVIRONMENT_WAITING && !status.write_attempted);
    assert(xiaotai_provisioning_cancel() == XIAOTAI_ERR_INVALID_STATE);
    assert(xiaotai_quiesced_for(XIAOTAI_QUIESCE_ENVIRONMENT));
    assert(xiaotai_environment_cancel() == XIAOTAI_OK);
}

void product_runtime_wifi_cancel_scope(void)
{
    product_platform_online();
    hold_platform_request();
    product_on_owner(provision_excludes_environment, NULL);
    const int64_t drain_deadline = xiaotai_time_us() + INT64_C(5000000);
    release_platform_request();

    /* The callback notifies us before the worker returns its request slot. */
    xiaotai_platform_status_t platform;
    do {
        xiaotai_platform_status(&platform);
        if (!platform.exchanges) break;
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < drain_deadline);
    assert(!platform.exchanges && !platform.gated && !xiaotai_quiesced());

    hold_platform_request();
    product_on_owner(environment_excludes_provision, NULL);
    release_platform_request();
    xiaotai_environment_status_t environment;
    const int64_t deadline = xiaotai_time_us() + INT64_C(5000000);
    do {
        xiaotai_environment_status(&environment);
        xiaotai_platform_status(&platform);
        if (environment.phase == XIAOTAI_ENVIRONMENT_IDLE && !platform.gated && !xiaotai_quiesced()) break;
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < deadline);
    assert(environment.phase == XIAOTAI_ENVIRONMENT_IDLE && !platform.gated && !xiaotai_quiesced());
    assert(!environment.write_attempted && !product_restarted());
    uint8_t marker;
    char base[64];
    assert(xiaotai_kv_get_u8("xmini", "wifi_provision", &marker, 2000) == XIAOTAI_ERR_NOT_FOUND);
    assert(xiaotai_kv_get_str("service_env", "ep_base", base, sizeof(base), 2000) == XIAOTAI_ERR_NOT_FOUND);
    puts("runtime Wi-Fi scope: cancel only releases its own operation during an in-flight HTTP request; no marker or reboot");
    xiaotai_signal_deinit(&http_entered);
    xiaotai_signal_deinit(&http_release);
    xiaotai_signal_deinit(&http_done);
}

void product_reprovision_keys(void)
{
    product_platform_online();
    const xiaotai_args_t settings = {.text = XIAOTAI_APP_COMPANION_PAGE_SETTINGS};
    assert(xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_SHOW, &settings) == XIAOTAI_OK);
    (void)wait_view(false, 0, 0);
    atomic_store(&s_observe, true);
    xiaotai_kv_host_counts_t before, after;
    xiaotai_kv_host_counts(&before);
    const unsigned leaves = network_leaves();

    const xiaotai_ui_info_t first = open_confirmation();
    assert(xiaotai_interaction_inject("main", 2, 0) == XIAOTAI_OK);
    (void)wait_view(false, 0, first.posted + 1U);
    product_on_owner(NULL, NULL);
    const unsigned accidental = atomic_load(&s_requests);
    if (accidental) fprintf(stderr, "default double-click dispatched %u provisioning request(s) without moving focus\n",
                            accidental);
    assert(!accidental && first.focus == 1U);

    const xiaotai_ui_info_t next = open_confirmation();
    assert(next.focus == 1U && strcmp(first.overlay, next.overlay));
    assert(xiaotai_interaction_inject("main", 1, 0) == XIAOTAI_OK);
    (void)wait_view(true, 2, 0);
    assert(!atomic_load(&s_requests));
    assert(xiaotai_interaction_inject("main", 2, 0) == XIAOTAI_OK);
    (void)wait_view(false, 0, next.posted + 1U);
    product_on_owner(NULL, NULL);
    assert(atomic_load(&s_requests) == 1U);

    const xiaotai_ui_info_t again = open_confirmation();
    assert(again.focus == 1U);
    xiaotai_kv_host_counts(&after);
    assert(after.writes == before.writes && after.commits == before.commits &&
           after.erase_alls == before.erase_alls && network_leaves() == leaves);
    puts("reprovision keys: default double-click canceled; explicit confirm requested setup without forgetting Wi-Fi");
    assert(xiaotai_interaction_inject("main", 2, 0) == XIAOTAI_OK);
    (void)wait_view(false, 0, again.posted + 1U);
    atomic_store(&s_observe, false);
    check_real_cancellation();
    xiaotai_signal_deinit(&http_entered);
    xiaotai_signal_deinit(&http_release);
    xiaotai_signal_deinit(&http_done);
}

static void begin_reprovision_after_edit(void *unused)
{
    edit_audio(unused);
    assert(xiaotai_provisioning_begin(0) == XIAOTAI_OK);
    assert(xiaotai_provisioning_begin_cancellable());
}

static void wait_for_platform_drain(void)
{
    const int64_t deadline = xiaotai_time_us() + INT64_C(5000000);
    do {
        platform_lock();
        const uint32_t ticket = platform.gate;
        platform_unlock();
        if (xiaotai_platform_gate_drained(ticket)) return;
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < deadline);
    assert(false);
}

typedef struct { xiaotai_signal_t entered, release; } owner_barrier_t;

static void hold_owner(void *ctx)
{
    owner_barrier_t *barrier = ctx;
    xiaotai_signal_give(&barrier->entered);
    assert(xiaotai_signal_take(&barrier->release, 5000));
}

static void cancel_reprovision(void *unused)
{
    (void)unused;
    assert(xiaotai_provisioning_begin_cancellable());
    assert(xiaotai_provisioning_cancel() == XIAOTAI_OK);
}

static void finish_preparation(bool failed)
{
    const int64_t deadline = xiaotai_time_us() + INT64_C(5000000);
    xiaotai_provisioning_status_t status;
    do {
        xiaotai_provisioning_status(&status);
        if (status.error != XIAOTAI_OK || status.provision.restart_required) break;
        xiaotai_delay_ms(5);
    } while (xiaotai_time_us() < deadline);
    uint8_t marker = 0;
    const xiaotai_err_t read = xiaotai_kv_get_u8("xmini", "wifi_provision", &marker, 2000);
    printf("preparation result: failed_write=%u error=%d phase=%d handed=%u marker_read=%d marker=%u restart=%u\n",
           (unsigned)failed, status.error, status.phase, (unsigned)status.provision.restart_required,
           read, (unsigned)marker, (unsigned)product_restarted());
    fflush(stdout);
    assert(!product_restarted());
    if (failed) {
        assert(status.phase == XIAOTAI_PROVISIONING_STARTING && status.error != XIAOTAI_OK &&
               !status.restart_pending && !status.provision.restart_required && read == XIAOTAI_ERR_NOT_FOUND);
        product_on_owner(cancel_reprovision, NULL);
        assert(!xiaotai_quiesced() && !xiaotai_restart_pending());
    } else {
        assert(status.provision.restart_required && read == XIAOTAI_OK && marker == 1);
        uint8_t saved[4];
        size_t size = sizeof(saved);
        assert(xiaotai_kv_get_blob_strict("xmini_ui", "audio", saved, &size, 2000) == XIAOTAI_OK &&
               size == sizeof(saved) && saved[0] == 2 && (saved[1] == 65 || saved[1] == 75));
    }
}

void product_reprovision_barrier(const char *test)
{
    const bool failed = !strcmp(test, "failed");
    const bool repaired = !strcmp(test, "repaired");
    assert(failed || repaired || !strcmp(test, "saved"));
    product_platform_online();
    const unsigned leaves = network_leaves();
    xiaotai_restart_hold(); /* Observe a wrong handoff without letting the host reboot. */
    xiaotai_preferences_stats_t before, after;
    xiaotai_provisioning_stats_t prep_before, prep_after;
    xiaotai_preferences_stats(&before);
    xiaotai_provisioning_stats(&prep_before);
    if (repaired) {
        xiaotai_kv_host_gate(false);
        product_on_owner(edit_audio, NULL);
        xiaotai_os_host_advance_us(1100000);
        assert(xiaotai_kv_host_wait_held(3000));
        xiaotai_kv_host_fail_key("audio", XIAOTAI_FAIL);
        xiaotai_kv_host_gate(true);
        assert(xiaotai_kv_flush(2000) == XIAOTAI_OK);
        assert(!xiaotai_preferences_pending());
        product_on_owner(NULL, NULL);
        xiaotai_kv_host_fail_key(NULL, XIAOTAI_OK);
    }

    xiaotai_kv_host_gate(false);
    product_on_owner(begin_reprovision_after_edit, NULL);
    xiaotai_os_host_advance_us(1100000);
    assert(xiaotai_kv_host_wait_held(3000));
    wait_for_platform_drain();
    owner_barrier_t barrier;
    assert(xiaotai_signal_init(&barrier.entered) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&barrier.release) == XIAOTAI_OK);
    assert(xiaotai_run(hold_owner, &barrier) == XIAOTAI_OK);
    assert(xiaotai_signal_take(&barrier.entered, 3000));
    if (failed) xiaotai_kv_host_fail_key("audio", XIAOTAI_FAIL);
    xiaotai_kv_host_gate(true);
    assert(xiaotai_kv_flush(2000) == XIAOTAI_OK);
    assert(!xiaotai_preferences_pending());
    xiaotai_preferences_stats(&after);
    assert(after.write_failures == before.write_failures + (failed || repaired ? 1U : 0U));
    xiaotai_os_host_advance_us(250000);
    puts("storage callback completed with the owner held; settle timer is now due");
    xiaotai_signal_give(&barrier.release);
    product_on_owner(NULL, NULL);
    xiaotai_signal_deinit(&barrier.release);
    xiaotai_signal_deinit(&barrier.entered);
    finish_preparation(failed);
    xiaotai_provisioning_stats(&prep_after);
    assert(prep_after.flush_failures == prep_before.flush_failures + (failed ? 1U : 0U));
    assert(network_leaves() == leaves);
}
