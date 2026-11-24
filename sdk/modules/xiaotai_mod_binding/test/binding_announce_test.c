// SPDX-License-Identifier: MIT
// Replaces legacy: tests/xmini/test_product.c::product
// Replaces legacy: tests/regression/test_binding_prompt_cancel.py
#include "binding_internal.h"
#include "fake_media.h"
#include "fake_platform.h"
#include "xiaotai/port/os.h"
#include "xiaotai/sounds.h"
#include <assert.h>
#include <string.h>

static unsigned events;
static xiaotai_err_t result, last_result;
static uint32_t activity, quiet;

static void on_announced(const xiaotai_event_t *event, void *ctx)
{
    assert(!ctx && event->data == NULL && event->size == 0);
    ++events;
    last_result = event->args.value;
}

static xiaotai_err_t observe(void)
{
    return xiaotai_event_subscribe(XIAOTAI_EVENT_BINDING_ANNOUNCED, on_announced, NULL);
}

static const xiaotai_module_t binding = {
    .name = "binding", .init = xiaotai_module_binding_init, .start = xiaotai_module_binding_start,
};
static const char *const deps[] = {"binding", NULL};
static const xiaotai_module_t observer = {.name = "observer", .depends = deps, .init = observe};
static const xiaotai_module_t *const modules[] = {&binding, &observer};
static const xiaotai_priority_t priorities[] = {{"call", 100}, {NULL, 0}};

static xiaotai_binding_status_t status(void)
{
    xiaotai_binding_status_t value;
    xiaotai_binding_status(&value);
    return value;
}

static xiaotai_binding_stats_t stats(void)
{
    xiaotai_binding_stats_t value;
    xiaotai_binding_stats(&value);
    return value;
}

static void emit(const char *event)
{
    assert(xiaotai_event_emit(event, NULL, NULL, 0) == XIAOTAI_OK);
    fake_drain_events();
}

static void waiting(const char *code)
{
    emit(XIAOTAI_EVENT_PLATFORM_BINDING_NEEDED);
    assert(status().phase == XIAOTAI_BINDING_REPORTING);
    fake_reply(200, 200, fake_report_body(code, "fixture-token", "fixture-client"));
    assert(status().phase == XIAOTAI_BINDING_CONNECTING);
    fake_guest_ready();
    assert(status().phase == XIAOTAI_BINDING_WAITING && !status().announcing);
}

static void boot(xiaotai_binding_voice_t voice, const char *code, uint8_t rounds, uint16_t wait_s)
{
    fake_reset();
    fake_clock_start();
    fake_audio_reset();
    xiaotai_sounds_reset();
    events = 0;
    activity = quiet = 0;
    const xiaotai_binding_config_t config = {.voice = voice, .announce_rounds = rounds, .wait_s = wait_s};
    const xiaotai_framework_config_t framework = {.modules = modules, .module_count = 2,
        .priorities = priorities, .available_needs = UINT32_MAX};
    assert(xiaotai_binding_configure(&config) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&framework) == XIAOTAI_OK);
    assert(fake_drain());
    if (code) waiting(code);
    assert(fake_audio.clips == 0 && fake_tts.submissions == 0 && events == 0);
}

static void do_announce(void *ctx) { (void)ctx; result = xiaotai_binding_announce(); }
static void do_stop(void *ctx) { (void)ctx; xiaotai_binding_announce_stop(); }
static void do_cancel(void *ctx) { (void)ctx; assert(xiaotai_binding_cancel() == XIAOTAI_OK); }
static void do_quiet(void *ctx) { (void)ctx; assert(xiaotai_quiesce_begin("maintenance", &quiet) == XIAOTAI_OK); }
static void do_activity(void *ctx)
{
    (void)ctx;
    const xiaotai_activity_t call = {.name = "call", .resources = XIAOTAI_RESOURCE_AUDIO_OUT};
    assert(xiaotai_activity_begin(&call, &activity) == XIAOTAI_OK);
}

static xiaotai_err_t announce(void)
{
    fake_on_owner(do_announce, NULL);
    return result;
}

static void shutdown(void)
{
    fake_on_owner(do_stop, NULL);
    if (fake_tts.pending) fake_tts_done(XIAOTAI_ERR_PLATFORM_CANCELLED, 0);
    xiaotai_framework_reset();
    xiaotai_binding_reset();
}

static const uint8_t sample[] = {0, 0};
#define CLIP(name_) {.name = name_, .data = sample, .size = sizeof(sample), .encoding = XIAOTAI_CODEC_PCM16, \
                      .container = XIAOTAI_SOUND_RAW, .rate_hz = 8000, .channels = 1}
static const xiaotai_sound_t clips[] = {CLIP("guide.binding"), CLIP("digit.0"), CLIP("digit.1"), CLIP("digit.2"),
    CLIP("digit.3"), CLIP("digit.4"), CLIP("digit.5"), CLIP("digit.6"), CLIP("digit.7"), CLIP("digit.8"), CLIP("digit.9")};
static const xiaotai_sound_pack_t full_pack = {"fixture", clips, sizeof(clips) / sizeof(clips[0])};
static const xiaotai_sound_pack_t short_pack = {"partial", clips, 7}; /* digit 6 missing */

static void local(void)
{
    boot(XIAOTAI_BINDING_VOICE_LOCAL_FIRST, NULL, 3, 0);
    assert(announce() == XIAOTAI_ERR_INVALID_STATE);
    waiting("09876543210");
    assert(xiaotai_sounds_register(&full_pack) == XIAOTAI_OK);
    fake_audio.playback = false;
    assert(announce() == XIAOTAI_ERR_NOT_SUPPORTED);
    fake_audio.playback = true;
    fake_audio.other_urgent = true;
    assert(announce() == XIAOTAI_ERR_BUSY && !fake_audio.clips && !fake_tts.submissions);
    fake_audio.other_urgent = false;

    assert(xiaotai_intent_post(XIAOTAI_INTENT_BINDING_ANNOUNCE, NULL) == XIAOTAI_OK);
    fake_drain_events();
    assert(status().announcing && fake_audio.clips == 1 && !fake_tts.submissions);
    assert(announce() == XIAOTAI_ERR_BUSY && fake_audio.clips == 1);
    const xiaotai_media_play_t *request = &fake_audio.clip;
    assert(request->priority == XIAOTAI_PLAY_URGENT && request->flags == XIAOTAI_PLAY_REFUSE_EQUAL &&
           request->generation == 0);
    assert(request->clip_count == 12 && request->repeat == 2 && request->gap_ms == 350);
    assert(!strcmp(request->clips[0].name, "guide.binding") && !strcmp(request->clips[1].name, "digit.0"));
    assert(!strcmp(request->clips[2].name, "digit.9") && !strcmp(request->clips[11].name, "digit.0"));
    fake_audio_done(fake_audio.play, XIAOTAI_OK);
    assert(!status().announcing && events == 1 && last_result == XIAOTAI_OK);
    assert(stats().announcements == 1 && stats().announcements_server == 0 && stats().announcements_busy == 2);
    shutdown();

    boot(XIAOTAI_BINDING_VOICE_LOCAL, "123456", 1, 0);
    assert(xiaotai_sounds_register(&short_pack) == XIAOTAI_OK);
    assert(announce() == XIAOTAI_ERR_NOT_SUPPORTED && fake_tts.submissions == 0);
    shutdown();

    boot(XIAOTAI_BINDING_VOICE_LOCAL_FIRST, "098765432109", 1, 0);
    assert(xiaotai_sounds_register(&full_pack) == XIAOTAI_OK);
    assert(announce() == XIAOTAI_ERR_NOT_SUPPORTED); /* more than eleven, not a six digit server code */
    shutdown();

    boot(XIAOTAI_BINDING_VOICE_NONE, "123456", 1, 0);
    assert(announce() == XIAOTAI_ERR_NOT_SUPPORTED);
    shutdown();

    boot(XIAOTAI_BINDING_VOICE_SERVER, "12345x", 1, 0);
    assert(announce() == XIAOTAI_ERR_NOT_SUPPORTED && fake_tts.submissions == 0);
    shutdown();
}

static void local_cancellation_and_deadline(void)
{
    boot(XIAOTAI_BINDING_VOICE_LOCAL, "012345", 1, 60);
    assert(xiaotai_sounds_register(&full_pack) == XIAOTAI_OK);
    assert(announce() == XIAOTAI_OK);
    const fake_play_t retired = fake_audio.play;
    assert(fake_audio.clip.clip_count == 7 && fake_audio.clip.repeat == 0);
    const char *const names[] = {"guide.binding", "digit.0", "digit.1", "digit.2", "digit.3", "digit.4", "digit.5"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); ++i)
        assert(!strcmp(fake_audio.clip.clips[i].name, names[i]));
    assert(announce() == XIAOTAI_ERR_BUSY && fake_audio.clips == 1);
    fake_on_owner(do_stop, NULL);
    assert(events == 1 && fake_audio.stopped_id == retired.id && !status().announcing);
    assert(announce() == XIAOTAI_OK && fake_audio.play.id != retired.id);
    const fake_play_t current = fake_audio.play;
    fake_audio_done(retired, XIAOTAI_ERR_INVALID_STATE);
    assert(events == 1 && status().announcing && fake_audio.play.id == current.id && !fake_tts.submissions);

    fake_clock_advance_to(59999999);
    assert(status().announcing && fake_audio.stops == 1);
    fake_clock_advance_to(60000001);
    assert(!status().announcing && events == 2 && last_result == XIAOTAI_ERR_INVALID_STATE);
    assert(fake_audio.stops == 2 && fake_audio.stopped_id == current.id);
    /* An unrelated diagnostic can now own the playback port. */
    fake_audio.other_urgent = true;
    fake_audio_done(current, XIAOTAI_OK);
    assert(events == 2 && fake_audio.stops == 2 && fake_audio.other_urgent);
    shutdown();
}

static const uint8_t pcm[] = {1, 2, 3, 4, 5, 6};

static void send_pcm(void)
{
    assert(fake_tts_head(200, "Audio/PCM;rate=8000", sizeof(pcm), false) == XIAOTAI_OK);
    assert(fake_tts_write(pcm, 1) == XIAOTAI_OK);
    assert(fake_tts_write(pcm + 1, 4) == XIAOTAI_OK);
    assert(fake_tts_write(pcm + 5, 1) == XIAOTAI_OK);
    fake_tts_done(XIAOTAI_OK, 200);
}

static void server_rounds(void)
{
    boot(XIAOTAI_BINDING_VOICE_LOCAL_FIRST, "123456", 3, 0);
    assert(xiaotai_sounds_register(&short_pack) == XIAOTAI_OK);
    fake_audio.other_urgent = true;
    assert(announce() == XIAOTAI_ERR_BUSY && !fake_audio.streams && !fake_tts.submissions);
    fake_audio.other_urgent = false;
    assert(announce() == XIAOTAI_OK && !fake_audio.clips && fake_audio.streams == 1);
    assert(fake_audio.stream.encoding == XIAOTAI_CODEC_PCM16 && fake_audio.stream.rate_hz == 8000);
    assert(fake_audio.stream.priority == XIAOTAI_PLAY_URGENT && fake_audio.stream.flags == XIAOTAI_PLAY_REFUSE_EQUAL);
    const xiaotai_platform_transfer_t *request = &fake_tts.req;
    assert(!strcmp(request->request.service, XIAOTAI_PLATFORM_SERVICE_DEVICE));
    assert(!strcmp(request->request.path, "/v1/device/tts?code=123456"));
    assert(!strcmp(request->request.bearer, "fixture-token") && request->request.auth == XIAOTAI_PLATFORM_AUTH_BEARER);
    assert(request->request.timeout_ms == 20000 && request->response_max == 160 * 1024);
    for (unsigned round = 1; round <= 3; ++round) {
        assert(fake_tts.submissions == round);
        send_pcm();
        assert(fake_audio.finishes == round && !events && status().announcing);
        assert(fake_audio.size == round * sizeof(pcm) && !memcmp(fake_audio.bytes + (round - 1) * sizeof(pcm), pcm, sizeof(pcm)));
        fake_audio_done(fake_audio.play, XIAOTAI_OK);
        if (round < 3) {
            const int64_t until = xiaotai_time_us() + 350000;
            fake_clock_advance_to(until - 1);
            assert(fake_tts.submissions == round);
            fake_clock_advance_to(until);
        }
    }
    assert(events == 1 && last_result == XIAOTAI_OK && !status().announcing);
    assert(stats().announcements_server == 1 && stats().announcements_failed == 0);
    shutdown();
}

static void grant_between_rounds(void)
{
    boot(XIAOTAI_BINDING_VOICE_SERVER, "123456", 3, 0);
    assert(announce() == XIAOTAI_OK);
    send_pcm();
    fake_audio_done(fake_audio.play, XIAOTAI_OK);
    const int64_t next_round = xiaotai_time_us() + 350000;
    fake_clock_advance_to(next_round - 1);
    fake_guest_ack_result(XIAOTAI_OK);
    assert(status().phase == XIAOTAI_BINDING_WAITING && status().announcing && !events);
    assert(!g_guest.acks && !g_identity.saves && fake_tts.submissions == 1);

    fake_guest_message("auth_grant",
        "{\"type\":\"auth_grant\",\"payload\":{\"device_id\":\"fixture\",\"device_key\":\"fixture-key\"}}");
    assert(status().phase == XIAOTAI_BINDING_STORING && g_identity.save_pending && !g_guest.acks);
    assert(!status().announcing && events == 1 && last_result == XIAOTAI_ERR_INVALID_STATE);
    assert(announce() == XIAOTAI_ERR_INVALID_STATE);
    assert(fake_audio.streams == 1);
    fake_clock_advance_to(next_round + 350000);
    assert(fake_tts.submissions == 1 && events == 1 && !xiaotai_binding_bound());

    fake_identity_finish_save(XIAOTAI_OK);
    assert(status().phase == XIAOTAI_BINDING_CONFIRMING && g_guest.acks == 1 && g_guest.ack_with_identity);
    fake_guest_ack_result(XIAOTAI_OK);
    assert(xiaotai_binding_bound() && fake_tts.submissions == 1 && !g_guest.open);
    shutdown();
}

static void failures(void)
{
    for (int example = 0; example < 4; ++example) {
        boot(XIAOTAI_BINDING_VOICE_SERVER, "123456", 1, 0);
        assert(announce() == XIAOTAI_OK);
        if (example == 0) assert(fake_tts_head(503, "audio/pcm", 0, false) == XIAOTAI_FAIL);
        if (example == 1) assert(fake_tts_head(200, "audio/pcm", 0, true) == XIAOTAI_FAIL);
        if (example == 2) assert(fake_tts_head(200, "audio/pcm", 160 * 1024 + 1, false) == XIAOTAI_ERR_INVALID_SIZE);
        if (example == 3) assert(fake_tts_head(200, "application/json", -1, false) == XIAOTAI_FAIL);
        fake_tts_done(XIAOTAI_FAIL, 0);
        assert(fake_audio.stops == 1 && !events);
        fake_clock_advance_to(300000);
        assert(fake_tts.submissions == 1);
        fake_clock_advance_to(300001);
        assert(fake_tts.submissions == 2);
        fake_tts_done(XIAOTAI_OK, 200); /* empty success is still failure */
        assert(fake_audio.finishes == 0 && events == 1 && last_result == XIAOTAI_FAIL);
        assert(status().phase == XIAOTAI_BINDING_WAITING && stats().announcements_failed == 1);
        shutdown();
    }

    for (int odd = 0; odd <= 1; ++odd) {
        boot(XIAOTAI_BINDING_VOICE_SERVER, "123456", 1, 0);
        assert(announce() == XIAOTAI_OK);
        assert(fake_tts_head(200, "audio/pcm", -1, false) == XIAOTAI_OK);
        assert(fake_tts_write(pcm, 3) == XIAOTAI_OK && fake_audio.size == 2);
        fake_tts_done(odd ? XIAOTAI_OK : XIAOTAI_ERR_TIMEOUT, 200);
        fake_clock_advance_to(999999);
        assert(fake_tts.submissions == 1 && fake_audio.finishes == 0 && fake_audio.stops == 1);
        assert(events == 1 && last_result == (odd ? XIAOTAI_FAIL : XIAOTAI_ERR_TIMEOUT));
        assert(status().phase == XIAOTAI_BINDING_WAITING && stats().announcements_failed == 1);
        shutdown();
    }

    boot(XIAOTAI_BINDING_VOICE_SERVER, "123456", 1, 0);
    fake_tts.refuse = XIAOTAI_ERR_NO_MEM;
    assert(announce() == XIAOTAI_ERR_NO_MEM && fake_audio.stops == 1 && !events && !status().announcing);
    assert(stats().announcements == 0);
    shutdown();
}

static void media_result_order(void)
{
    for (int order = 0; order < 4; ++order) {
        boot(XIAOTAI_BINDING_VOICE_SERVER, "123456", 1, 0);
        assert(announce() == XIAOTAI_OK && fake_tts.pending);
        const fake_play_t play = fake_audio.play;
        assert(fake_tts_head(200, "audio/pcm", sizeof(pcm), false) == XIAOTAI_OK);
        const xiaotai_err_t terminal = order == 1 ? XIAOTAI_FAIL : XIAOTAI_ERR_TIMEOUT;
        if (order == 2) {
            fake_audio_done(play, terminal);
            assert(fake_tts.cancels == 1 && events == 1);
            fake_tts_done(XIAOTAI_ERR_PLATFORM_CANCELLED, 200);
        } else {
            if (order == 1) {
                assert(fake_tts_write(pcm, sizeof(pcm)) == XIAOTAI_OK);
                fake_audio.finish_refuse = XIAOTAI_ERR_INVALID_STATE;
                fake_tts_done(XIAOTAI_OK, 200);
            } else {
                fake_audio.write_refuse = XIAOTAI_ERR_INVALID_STATE;
                assert(fake_tts_write(pcm, sizeof(pcm)) == XIAOTAI_ERR_INVALID_STATE);
                fake_tts_done(XIAOTAI_ERR_INVALID_STATE, 200);
            }
            assert(status().announcing && !events);
            if (order == 3) fake_on_owner(do_cancel, NULL);
            fake_audio_done(play, terminal);
        }
        assert(!status().announcing && events == 1 && fake_tts.submissions == 1);
        assert(last_result == (order == 3 ? XIAOTAI_ERR_INVALID_STATE : terminal));
        assert(stats().announcements_failed == (order == 3 ? 0 : 1));
        shutdown();
    }
}

static void cancel_paths(void)
{
    for (int reason = 0; reason < 7; ++reason) {
        boot(XIAOTAI_BINDING_VOICE_SERVER, "123456", 1, 1);
        assert(announce() == XIAOTAI_OK);
        const fake_play_t old = fake_audio.play;
        if (reason == 0) fake_on_owner(do_stop, NULL);
        if (reason == 1) fake_on_owner(do_cancel, NULL);
        if (reason == 2) fake_guest_message("auth_grant",
            "{\"type\":\"auth_grant\",\"payload\":{\"device_id\":\"fixture\",\"device_key\":\"fixture-key\"}}");
        if (reason == 3) fake_guest_closed(XIAOTAI_FAIL);
        if (reason == 4) emit(XIAOTAI_EVENT_NET_DOWN);
        if (reason == 5) fake_on_owner(do_quiet, NULL);
        if (reason == 6) fake_clock_advance_to(1000001);
        assert(!status().announcing && events == 1 && last_result == XIAOTAI_ERR_INVALID_STATE);
        assert(fake_tts.cancels == 1 && fake_audio.stopped_id == old.id);
        assert(fake_tts_head(200, "audio/pcm", -1, false) == XIAOTAI_ERR_INVALID_STATE);
        assert(fake_tts_write(pcm, sizeof(pcm)) == XIAOTAI_ERR_INVALID_STATE && !fake_audio.writes);
        if (reason == 0) assert(announce() == XIAOTAI_ERR_BUSY); /* the borrowed TTS slot is not retired */
        fake_tts_done(XIAOTAI_ERR_PLATFORM_CANCELLED, 0);
        if (reason == 0) assert(announce() == XIAOTAI_OK);
        fake_audio_done(old, XIAOTAI_ERR_INVALID_STATE);
        assert(events == 1 && !stats().announcements_failed);
        shutdown();
    }

    boot(XIAOTAI_BINDING_VOICE_SERVER, "123456", 1, 0);
    assert(announce() == XIAOTAI_OK);
    fake_on_owner(do_activity, NULL);
    assert(activity && last_result == XIAOTAI_ERR_INVALID_STATE && !status().announcing);
    assert(announce() == XIAOTAI_ERR_BUSY);
    shutdown();
}

int main(void)
{
    local();
    local_cancellation_and_deadline();
    server_rounds();
    grant_between_rounds();
    failures();
    media_result_order();
    cancel_paths();
    return 0;
}
