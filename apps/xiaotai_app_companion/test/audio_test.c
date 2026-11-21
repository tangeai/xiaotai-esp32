// SPDX-License-Identifier: MIT
#include "audio_fakes.h"
#include "fakes.h"

#include <assert.h>
#include <string.h>

#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"

static xiaotai_signal_t fence;
static xiaotai_args_t last_session;
static unsigned session_calls;
static uint32_t activity;

static xiaotai_err_t session(const xiaotai_args_t *args)
{
    last_session = *args;
    ++session_calls;
    return XIAOTAI_OK;
}

static const xiaotai_app_companion_part_t recorder = {.name = "test-session", .session = session};
const xiaotai_app_companion_part_t *const xiaotai_app_companion_parts[] = {&recorder, NULL};

static void barrier(void *unused) { (void)unused; xiaotai_signal_give(&fence); }

static void flush(void)
{
    for (unsigned i = 0; i < 3; ++i) {
        assert(xiaotai_run(barrier, NULL) == XIAOTAI_OK);
        assert(xiaotai_signal_take(&fence, 3000));
    }
}

static void owner(xiaotai_task_fn_t body, void *arg)
{
    assert(xiaotai_run(body, arg) == XIAOTAI_OK);
    flush();
}

static void send(const char *intent, int32_t value, const char *text)
{
    xiaotai_args_t args = {.value = value};
    if (text) strcpy(args.text, text);
    assert(xiaotai_intent_post(intent, &args) == XIAOTAI_OK);
    flush();
}

static void boot(void)
{
    static const xiaotai_module_t probe = {.name = "probe", .init = companion_fixture_init};
    static const xiaotai_module_t *const modules[] = {&probe};
    static const xiaotai_application_t app = {.name = "companion", .init = xiaotai_app_companion_init,
        .start = xiaotai_app_companion_start, .foreground = xiaotai_app_companion_foreground};
    static const xiaotai_application_t *const apps[] = {&app};
    const uint32_t needs = XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_KEY;
    fixture = (companion_test_fixture_t){.needs = needs};
    fixture.net.state = XIAOTAI_NET_UP;
    fixture.binding.phase = XIAOTAI_BINDING_BOUND;
    fixture.platform.phase = XIAOTAI_PLATFORM_ONLINE;
    audio_fixture = (companion_audio_fixture_t){.status = {.ready = true},
        .info = {.capture = true, .capture_pcm = true, .capture_rate_hz = 8000, .playback = true}};
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = modules, .module_count = 1,
        .applications = apps, .application_count = 1, .available_needs = needs}) == XIAOTAI_OK);
    flush();
}

static void open_audio(void)
{
    send(XIAOTAI_INTENT_APP_COMPANION_SHOW, 0, XIAOTAI_APP_COMPANION_PAGE_AUDIO);
    assert(!strcmp(fixture.id, "audio") && !strcmp(fixture.title, "音频诊断"));
}

static uint8_t reference_alaw(int input)
{
    int magnitude = input < 0 ? -(input + 1) : input;
    int top = 255;
    unsigned segment = 0;
    while (magnitude > top) {
        top = (top << 1) + 1;
        ++segment;
    }
    const unsigned shift = segment > 1 ? segment + 3 : 4;
    const unsigned payload = (segment << 4) | (((unsigned)magnitude >> shift) & 15U);
    return (uint8_t)(payload ^ (input < 0 ? 0x55U : 0xd5U));
}

static void tone(void)
{
    static const int period[] = {0, 927, 1763, 2427, 2853, 3000, 2853, 2427, 1763, 927,
        0, -927, -1763, -2427, -2853, -3000, -2853, -2427, -1763, -927};
    const int64_t allocated = xiaotai_mem_host_outstanding();
    send(XIAOTAI_INTENT_APP_COMPANION_AUDIO, 0, NULL);
    const xiaotai_sound_t *sound = audio_fixture.play.clips[0].sound;
    assert(sound && sound->rate_hz == 8000 && sound->channels == 1 && sound->size == 4000);
    assert(sound->encoding == XIAOTAI_CODEC_G711A && sound->container == XIAOTAI_SOUND_RAW);
    for (size_t i = 0; i < sound->size; ++i) assert(sound->data[i] == reference_alaw(period[i % 20]));
    assert(!strcmp(fixture.status, "进行中") && xiaotai_mem_host_outstanding() == allocated + 1);
    send(XIAOTAI_INTENT_APP_COMPANION_AUDIO, 3, NULL);
    assert(audio_fixture.play_stops == 1 && audio_fixture.play.done);
    assert(sound->data[20] == reference_alaw(0));
}

static void play_done(void *ctx)
{
    const xiaotai_err_t *result = ctx;
    companion_fixture_play_done(*result);
}

static void key_audio_menu(void)
{
    const unsigned plays = audio_fixture.plays, stops = audio_fixture.play_stops;
    assert(fixture.items[2].state & XIAOTAI_STATE_DISABLED);
    send(fixture.items[2].intent, fixture.items[2].value, NULL);
    assert(audio_fixture.plays == plays);

    send(fixture.items[0].intent, fixture.items[0].value, NULL);
    assert(audio_fixture.plays == plays + 1U && (fixture.items[0].state & XIAOTAI_STATE_DISABLED));
    send(fixture.items[0].intent, fixture.items[0].value, NULL);
    assert(audio_fixture.plays == plays + 1U);

    send(fixture.actions[0].intent, fixture.actions[0].value, NULL);
    assert(audio_fixture.play_stops == stops + 1U && strcmp(fixture.id, "audio"));
    xiaotai_err_t cancelled = XIAOTAI_ERR_INVALID_STATE;
    owner(play_done, &cancelled);
    open_audio();
    assert(!strcmp(fixture.status, "空闲") && !(fixture.items[0].state & XIAOTAI_STATE_DISABLED));
}

static void record_done(void *ctx)
{
    const bool *success = ctx;
    assert(audio_fixture.record.pcm && audio_fixture.record.samples == 8000);
    audio_fixture.record.pcm[0] = 345;
    audio_fixture.record.pcm[7999] = -456;
    companion_fixture_record_done(*success ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE, *success ? 8000 : 20);
}

static void record_lifetime(void)
{
    const int64_t allocated = xiaotai_mem_host_outstanding();
    send(XIAOTAI_INTENT_APP_COMPANION_AUDIO, 1, NULL);
    assert(audio_fixture.records == 1);
    assert(!strcmp(fixture.status, "进行中") && xiaotai_mem_host_outstanding() == allocated + 1);
    send(XIAOTAI_INTENT_APP_COMPANION_BACK, 0, NULL);
    assert(strcmp(fixture.id, "audio") && audio_fixture.record_stops == 1);
    assert(xiaotai_mem_host_outstanding() == allocated + 1);
    bool success = false;
    owner(record_done, &success);
    assert(xiaotai_mem_host_outstanding() == allocated);
    open_audio();
    assert(!strcmp(fixture.status, "空闲") && (fixture.items[2].state & XIAOTAI_STATE_DISABLED));

    send(XIAOTAI_INTENT_APP_COMPANION_AUDIO, 1, NULL);
    success = true;
    owner(record_done, &success);
    assert(!strcmp(fixture.status, "已录音") && xiaotai_mem_host_outstanding() == allocated + 1);
    send(XIAOTAI_INTENT_APP_COMPANION_BACK, 0, NULL);
    open_audio();
    assert(!strcmp(fixture.status, "已录音"));
    send(XIAOTAI_INTENT_APP_COMPANION_AUDIO, 2, NULL);
    const xiaotai_sound_t *sound = audio_fixture.play.clips[0].sound;
    assert(sound->encoding == XIAOTAI_CODEC_PCM16 && sound->rate_hz == 8000 && sound->size == 16000);
    const int16_t *pcm = (const int16_t *)sound->data;
    assert(pcm[0] == 345 && pcm[7999] == -456 && xiaotai_mem_host_outstanding() == allocated + 1);
    send(XIAOTAI_INTENT_APP_COMPANION_AUDIO, 1, NULL);
    assert(audio_fixture.records == 2 && fixture.last_refusal.value == XIAOTAI_ERR_BUSY);
    xiaotai_err_t result = XIAOTAI_OK;
    owner(play_done, &result);
    assert(!strcmp(fixture.status, "已录音") && xiaotai_mem_host_outstanding() == allocated + 1);
}

static void session_started(void *ctx)
{
    const char *name = ctx;
    audio_fixture.status.active = true;
    audio_fixture.status.generation = 93;
    const xiaotai_activity_t request = {.name = name, .generation = 93, .resources = XIAOTAI_RESOURCE_AUDIO_OUT};
    assert(xiaotai_activity_begin(&request, &activity) == XIAOTAI_OK);
}

static void session_ended(void *unused)
{
    (void)unused;
    audio_fixture.status.active = false;
    audio_fixture.status.generation = 0;
    xiaotai_activity_end(activity);
}

static void unrelated_media(void *unused)
{
    (void)unused;
    ++audio_fixture.status.generation;
}

static void reset_audio(void *unused)
{
    (void)unused;
    companion_audio_reset();
}

static void reuse_and_detach(int64_t allocated)
{
    send(XIAOTAI_INTENT_APP_COMPANION_AUDIO, 2, NULL);
    const uint8_t *old = audio_fixture.play.clips[0].sound->data;
    const xiaotai_err_t ok = XIAOTAI_OK;
    owner(play_done, (void *)&ok);
    send(XIAOTAI_INTENT_APP_COMPANION_AUDIO, 1, NULL);
    assert((const uint8_t *)audio_fixture.record.pcm == old);
    assert(xiaotai_mem_host_outstanding() == allocated + 1);
    const unsigned views = fixture.views;
    owner(reset_audio, NULL);
    assert(xiaotai_mem_host_outstanding() == allocated + 1);
    bool success = false;
    owner(record_done, &success);
    assert(xiaotai_mem_host_outstanding() == allocated && fixture.views == views);
}

static void stop_sessions(void)
{
    static const char *const names[] = {"call", "ai", "room", "stream"};
    static const char *const actions[] = {"hangup", "talk.stop", "room.disconnect", "stream.stop"};
    for (unsigned i = 0; i < 4; ++i) {
        owner(session_started, (void *)names[i]);
        send(XIAOTAI_INTENT_APP_COMPANION_AUDIO, 3, NULL);
        assert(session_calls == i + 1U && last_session.value == 93 && !strcmp(last_session.text, actions[i]));
        owner(unrelated_media, NULL);
        send(XIAOTAI_INTENT_APP_COMPANION_AUDIO, 3, NULL);
        assert(session_calls == i + 1U && fixture.last_refusal.value == XIAOTAI_ERR_INVALID_STATE);
        owner(session_ended, NULL);
    }
}

static void stop_during_preemption(void)
{
    const unsigned previous = session_calls, local_stops = audio_fixture.play_stops;
    send(XIAOTAI_INTENT_APP_COMPANION_AUDIO, 0, NULL);
    assert(audio_fixture.play.done);
    owner(session_started, "call");
    send(XIAOTAI_INTENT_APP_COMPANION_AUDIO, 3, NULL);
    assert(session_calls == previous + 1U && last_session.value == 93 && !strcmp(last_session.text, "hangup"));
    assert(audio_fixture.play.done && audio_fixture.play_stops == local_stops);
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    owner(play_done, &result);
    owner(session_ended, NULL);
}

int main(void)
{
    assert(xiaotai_signal_init(&fence) == XIAOTAI_OK);
    boot();
    open_audio();
    assert(fixture.item_count == 4 && fixture.action_count == 1 && !strcmp(fixture.status, "空闲"));
    const xiaotai_args_t start = {0};
    assert(xiaotai_intent_post_external(XIAOTAI_INTENT_APP_COMPANION_AUDIO, &start) == XIAOTAI_ERR_NOT_ALLOWED);
    assert(audio_fixture.plays == 0);
    const int64_t allocated = xiaotai_mem_host_outstanding();
    tone();
    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    owner(play_done, &result);
    assert(!strcmp(fixture.status, "空闲") && xiaotai_mem_host_outstanding() == allocated);
    key_audio_menu();
    record_lifetime();
    stop_sessions();
    stop_during_preemption();
    reuse_and_detach(allocated);
    xiaotai_framework_reset();
    xiaotai_app_companion_reset();
    xiaotai_signal_deinit(&fence);
    return 0;
}
