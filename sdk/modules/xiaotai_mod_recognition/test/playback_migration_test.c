// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_recognition_playback.py
// Legacy recognition audio/playback ownership, with real media and journal owners.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "operation_fixture.h"
#include "recognition_journal.h"
#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/modules/recognition.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#define RC(name) XIAOTAI_RECOGNITION_##name
enum { WAIT_MS = 3000, WAV_HEADER = 44, FRAMES = 3200, WAV_BYTES = WAV_HEADER + 2 * FRAMES, MARKER = 1234 };

static xiaotai_signal_t owner_done, writer_waiting, writer_resume;
static _Atomic(void *) wav_buffer;
static atomic_uint wav_allocations, wav_frees, pcm_samples, stop_requests;
static atomic_bool hold_next_write, refuse_write, refuse_played_commit;
static uint8_t wav[WAV_BYTES];
static void (*owner_job)(void);
static xiaotai_media_play_t previous_play;
static uint32_t previous_id, call_activity;
static unsigned play_requests;
static bool two_items, task_returned;

void check(bool value, const char *expression, const char *file, int line)
{
    if (value) return;
    fprintf(stderr, "%s:%d: %s\n", file, line, expression);
    abort();
}

void *__real_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size);
void *__wrap_xiaotai_mem_alloc(xiaotai_mem_kind_t kind, size_t size)
{
    void *const pointer = __real_xiaotai_mem_alloc(kind, size);
    if (kind == XIAOTAI_MEM_BULK && size == WAV_BYTES) {
        CHECK(pointer && !atomic_exchange(&wav_buffer, pointer));
        atomic_fetch_add(&wav_allocations, 1);
    }
    return pointer;
}

void __real_xiaotai_mem_free(void *pointer);
void __wrap_xiaotai_mem_free(void *pointer)
{
    if (pointer && pointer == atomic_load(&wav_buffer)) {
        CHECK(atomic_exchange(&wav_buffer, NULL) == pointer);
        atomic_fetch_add(&wav_frees, 1);
    }
    __real_xiaotai_mem_free(pointer);
}

void __wrap_xiaotai_mem_stats(xiaotai_mem_kind_t kind, xiaotai_mem_stats_t *stats)
{
    (void)kind;
    *stats = (xiaotai_mem_stats_t){.free = 16U * 1024U * 1024U, .largest_block = 2U * 1024U * 1024U};
}

xiaotai_err_t __real_xiaotai_kv_set_str(const char *space, const char *key, const char *value, uint32_t timeout);
xiaotai_err_t __wrap_xiaotai_kv_set_str(const char *space, const char *key, const char *value, uint32_t timeout)
{
    if (!strcmp(space, "xt_recognition") && !strcmp(key, "pending") && strstr(value, "\"state\":2") &&
        atomic_exchange(&refuse_played_commit, false)) return XIAOTAI_FAIL;
    return __real_xiaotai_kv_set_str(space, key, value, timeout);
}

xiaotai_err_t __real_xiaotai_media_play_clips(const xiaotai_media_play_t *play, uint32_t *id);
xiaotai_err_t __wrap_xiaotai_media_play_clips(const xiaotai_media_play_t *play, uint32_t *id)
{
    CHECK(xiaotai_on_owner() && play->priority == XIAOTAI_PLAY_URGENT && play->clip_count == 1);
    CHECK(play->clips[0].sound->data == atomic_load(&wav_buffer));
    const xiaotai_recognition_journal_mirror_t *const journal = xiaotai_recognition_journal_mirror();
    CHECK(journal->item_count == play_requests + 1U &&
          journal->items[play_requests].state == XIAOTAI_RECOGNITION_JOURNAL_PLAY_STARTED);
    const xiaotai_err_t result = __real_xiaotai_media_play_clips(play, id);
    if (!play_requests && result == XIAOTAI_OK) {
        previous_play = *play;
        previous_id = *id;
    }
    ++play_requests;
    return result;
}

void __real_xiaotai_media_stop(uint32_t id);
void __wrap_xiaotai_media_stop(uint32_t id)
{
    CHECK(xiaotai_on_owner() && id && atomic_load(&wav_buffer));
    atomic_fetch_add(&stop_requests, 1);
    __real_xiaotai_media_stop(id);
}

static xiaotai_err_t output_side(void *ctx) { (void)ctx; return XIAOTAI_OK; }

static xiaotai_err_t output_write(void *ctx, void *raw, size_t size)
{
    (void)ctx;
    const int16_t *const samples = raw;
    unsigned markers = 0;
    for (size_t i = 0; i < size / sizeof(*samples); ++i) markers += samples[i] == MARKER;
    if (!markers) return XIAOTAI_OK;
    const uint8_t *const borrowed = atomic_load(&wav_buffer);
    CHECK(borrowed);
    atomic_fetch_add(&pcm_samples, markers);
    if (atomic_exchange(&hold_next_write, false)) {
        xiaotai_signal_give(&writer_waiting);
        CHECK(xiaotai_signal_take(&writer_resume, WAIT_MS));
        CHECK(atomic_load(&wav_buffer) == borrowed && borrowed[WAV_BYTES - 1U] == MARKER >> 8);
    }
    return atomic_exchange(&refuse_write, false) ? XIAOTAI_FAIL : XIAOTAI_OK;
}

const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index)
{
    static const xiaotai_audio_output_t output = {
        .desc = {.encoding = XIAOTAI_CODEC_PCM16, .rate_hz = 8000, .channels = 1},
        .open = output_side, .write = output_write, .close = output_side,
    };
    return index ? NULL : &output;
}

const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index) { (void)index; return NULL; }

static void owner_execute(void *ctx)
{
    (void)ctx;
    if (owner_job) owner_job();
    xiaotai_signal_give(&owner_done);
}

static void on_owner(void (*fn)(void))
{
    owner_job = fn;
    CHECK(xiaotai_run(owner_execute, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_signal_take(&owner_done, WAIT_MS));
}

static xiaotai_recognition_status_t status(void)
{
    xiaotai_recognition_status_t out;
    for (unsigned i = 0; i < WAIT_MS; ++i) {
        if (xiaotai_recognition_status(&out) == XIAOTAI_OK) return out;
        xiaotai_delay_ms(1);
    }
    CHECK(false);
    return (xiaotai_recognition_status_t){0};
}

static xiaotai_recognition_status_t wait_state(xiaotai_recognition_phase_t phase, bool pending)
{
    const int64_t deadline = xiaotai_time_us() + WAIT_MS * 1000;
    do {
        const xiaotai_recognition_status_t s = status();
        if (s.phase == phase && s.cleanup_pending == pending) return s;
        xiaotai_delay_ms(1);
    } while (xiaotai_time_us() < deadline);
    const xiaotai_recognition_status_t s = status();
    fprintf(stderr, "phase=%u cleanup=%u error=%d token=%u\n", (unsigned)s.phase,
            (unsigned)s.cleanup_pending, s.error, (unsigned)s.token);
    CHECK(false);
    return s;
}

#define SPEECH(id, order) "{\"item_id\":\"" id "\",\"order\":" order ",\"scheme_id\":\"fixture\"," \
    "\"scheme_version\":1,\"output_mode\":\"audio\",\"status\":\"succeeded\",\"result\":null,\"error\":null," \
    "\"speech\":{\"status\":\"ready\",\"error\":null," \
    "\"url\":\"/v1/recognition/requests/request/audio/" id "\",\"format\":\"wav\"}}"

static void return_task(void)
{
    if (!fake_queries || !fake_stream.done) return;
    static const char prefix[] = "{\"code\":200,\"data\":{\"request_id\":\"request\",\"device_id\":\"dev\","
        "\"status_revision\":1,\"status\":\"succeeded\",\"error\":null,\"results\":[";
    char body[1024];
    const int size = snprintf(body, sizeof(body), "%s%s%s]}}", prefix, SPEECH("item_1", "1"),
                              two_items ? "," SPEECH("item_2", "2") : "");
    CHECK(fake_queries == 1U && size > 0 && (size_t)size < sizeof(body));
    fake_query_done(body, (size_t)size, size, 17U, 200, XIAOTAI_OK);
    task_returned = true;
}

static void begin_wav(void)
{
    const xiaotai_platform_response_head_t head = {
        .http_status = 200, .content_type = "audio/wav", .content_length = WAV_BYTES,
    };
    CHECK(fake_stream.begin && fake_stream.done);
    CHECK(fake_stream.begin(fake_stream.ctx, &head) == XIAOTAI_OK);
    CHECK(fake_stream.write(fake_stream.ctx, wav, 43U) == XIAOTAI_OK);
    CHECK(fake_stream.write(fake_stream.ctx, wav + 43U, WAV_BYTES - 43U) == XIAOTAI_OK);
    CHECK(atomic_load(&wav_buffer));
}

static void return_wav(void) { fake_download_done(200, WAV_BYTES, XIAOTAI_OK); }

static void preempt(void)
{
    const xiaotai_activity_t activity = {.name = "call"};
    CHECK(xiaotai_activity_begin(&activity, &call_activity) == XIAOTAI_OK);
}

static void end_call(void) { xiaotai_activity_end(call_activity); }

static void delayed_play_done(void)
{
    CHECK(previous_id && previous_play.done && atomic_load(&wav_buffer));
    previous_play.done(previous_id, XIAOTAI_OK, previous_play.ctx);
}

static void put16(size_t at, unsigned value)
{
    wav[at] = (uint8_t)value;
    wav[at + 1U] = (uint8_t)(value >> 8);
}

static void put32(size_t at, unsigned value)
{
    put16(at, value);
    put16(at + 2U, value >> 16);
}

static void build_wav(bool invalid)
{
    memcpy(wav, "RIFF", 4U);
    put32(4U, WAV_BYTES - 8U + (invalid ? 2U : 0U));
    memcpy(wav + 8U, "WAVEfmt ", 8U);
    put32(16U, 16U);
    put16(20U, 1U);
    put16(22U, 1U);
    put32(24U, 8000U);
    put32(28U, 16000U);
    put16(32U, 2U);
    put16(34U, 16U);
    memcpy(wav + 36U, "data", 4U);
    put32(40U, 2U * FRAMES);
    for (size_t offset = WAV_HEADER; offset < WAV_BYTES; offset += 2U) put16(offset, MARKER);
}

static void wait_writes(unsigned expected_plays)
{
    CHECK(xiaotai_signal_take(&writer_waiting, WAIT_MS));
    CHECK(wait_state(RC(PLAYBACK), true).consumption == RC(PLAY_STARTED));
    CHECK(play_requests == expected_plays && atomic_load(&wav_allocations) == expected_plays);
    CHECK(atomic_load(&wav_frees) == expected_plays - 1U);
}

static void verify_bounded_stop(void)
{
    const int64_t deadline = xiaotai_time_us() + WAIT_MS * 1000;
    while (atomic_load(&stop_requests) < 3U && xiaotai_time_us() < deadline) xiaotai_delay_ms(1);
    xiaotai_delay_ms(150);
    CHECK(atomic_load(&stop_requests) == 3U && !atomic_load(&wav_frees) && atomic_load(&wav_buffer));
    CHECK(wait_state(RC(CANCELLING), true).consumption == RC(PLAY_STARTED));
}

int main(int argc, char **argv)
{
    CHECK(argc == 1 || argc == 2);
    const char *const scenario = argc == 1 ? "complete" : argv[1];
    two_items = !strcmp(scenario, "complete");
    const bool preempt_play = !strcmp(scenario, "preempt"), late_http = !strcmp(scenario, "late-http"),
        malformed = !strcmp(scenario, "malformed"), sink_error = !strcmp(scenario, "sink-error"),
        commit_error = !strcmp(scenario, "commit-error");
    CHECK(two_items || preempt_play || late_http || malformed || sink_error || commit_error);
    build_wav(malformed);
    xiaotai_signal_t *const signals[] = {&owner_done, &writer_waiting, &writer_resume};
    for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); ++i) CHECK(xiaotai_signal_init(signals[i]) == XIAOTAI_OK);
    CHECK(xiaotai_kv_init() == XIAOTAI_OK);
    CHECK(xiaotai_kv_set_str("xt_recognition", "pending",
        "{\"version\":1,\"device_id\":\"dev\",\"request_id\":\"request\",\"revision\":1,\"seconds\":1}", WAIT_MS) == XIAOTAI_OK);
    static const xiaotai_priority_t priorities[] = {{"call", 20}, {XIAOTAI_RECOGNITION_ACTIVITY, 10}, {0}};
    static const xiaotai_module_t media = {.name = "media", .init = xiaotai_engine_media_init, .start = xiaotai_engine_media_start};
    static const char *const depends[] = {"media", NULL};
    static const xiaotai_module_t recognition = {.name = "recognition", .depends = depends, .activities = priorities,
        .init = xiaotai_module_recognition_init, .start = xiaotai_module_recognition_start};
    const xiaotai_module_t *const modules[] = {&media, &recognition};
    CHECK(xiaotai_framework_start(&(const xiaotai_framework_config_t){.modules = modules, .module_count = 2}) == XIAOTAI_OK);
    CHECK(wait_state(RC(RECOVERY), false).request_id[0]);
    xiaotai_media_status_t speaker;
    xiaotai_media_status(&speaker);
    CHECK(speaker.ready);
    CHECK(xiaotai_recognition_query() == XIAOTAI_OK);
    (void)wait_state(RC(QUERY), true);
    const int64_t query_deadline = xiaotai_time_us() + WAIT_MS * 1000;
    do {
        on_owner(return_task);
        if (!task_returned) xiaotai_delay_ms(1);
    } while (!task_returned && xiaotai_time_us() < query_deadline);
    CHECK(task_returned);
    (void)wait_state(RC(RESULT), false);
    CHECK(xiaotai_recognition_resume("request") == XIAOTAI_OK);
    const xiaotai_recognition_status_t downloading = wait_state(RC(DOWNLOAD), true);
    CHECK(!strcmp(downloading.item_id, "item_1"));
    on_owner(begin_wav);
    if (late_http) {
        CHECK(xiaotai_recognition_cancel(downloading.token) == XIAOTAI_OK);
        CHECK(wait_state(RC(CANCELLING), true).playback_paused && fake_transfer_cancelled);
        CHECK(atomic_load(&wav_buffer) && !atomic_load(&wav_frees));
    }
    atomic_store(&hold_next_write, true);
    on_owner(return_wav);
    if (malformed || late_http) {
        const xiaotai_recognition_status_t s = wait_state(malformed ? RC(FAILED) : RC(CANCELLED), false);
        CHECK(s.playback_paused && s.consumption == RC(UNTOUCHED) && !s.token);
        CHECK(!play_requests && !atomic_load(&pcm_samples) && !atomic_load(&wav_buffer) && atomic_load(&wav_frees) == 1U);
        CHECK(!xiaotai_activity_active());
    } else {
        wait_writes(1U);
        if (preempt_play) {
            on_owner(preempt);
            verify_bounded_stop();
        }
        atomic_store(&refuse_write, sink_error);
        atomic_store(&refuse_played_commit, commit_error);
        xiaotai_signal_give(&writer_resume);
        if (preempt_play) {
            CHECK(wait_state(RC(CANCELLED), false).consumption == RC(PLAY_STARTED));
            CHECK(atomic_load(&pcm_samples) < FRAMES && atomic_load(&wav_frees) == 1U);
            on_owner(end_call);
        } else if (sink_error || commit_error) {
            const xiaotai_recognition_status_t s = wait_state(sink_error ? RC(CANCELLED) : RC(FAILED), false);
            CHECK(s.consumption == RC(PLAY_STARTED) && s.error == XIAOTAI_FAIL);
            CHECK(sink_error ? atomic_load(&pcm_samples) < FRAMES : atomic_load(&pcm_samples) == FRAMES);
            CHECK(atomic_load(&wav_frees) == 1U && !atomic_load(&refuse_played_commit));
            char pending[512];
            CHECK(xiaotai_kv_get_str("xt_recognition", "pending", pending, sizeof(pending), WAIT_MS) == XIAOTAI_OK);
            CHECK(strstr(pending, "\"state\":1") && !strstr(pending, "\"state\":2"));
        } else {
            CHECK(!strcmp(wait_state(RC(DOWNLOAD), true).item_id, "item_2"));
            CHECK(atomic_load(&wav_frees) == 1U && atomic_load(&pcm_samples) == FRAMES);
            atomic_store(&hold_next_write, true);
            on_owner(begin_wav);
            on_owner(return_wav);
            wait_writes(2U);
            on_owner(delayed_play_done);
            CHECK(wait_state(RC(PLAYBACK), true).consumption == RC(PLAY_STARTED) && atomic_load(&wav_frees) == 1U);
            xiaotai_signal_give(&writer_resume);
            CHECK(wait_state(RC(RESULT), false).consumption == RC(PLAYED));
            CHECK(atomic_load(&pcm_samples) == 2U * FRAMES && atomic_load(&wav_frees) == 2U);
        }
        CHECK(!atomic_load(&wav_buffer) && !xiaotai_activity_active());
    }
    printf("PASS: %s, WAV alloc/free=%u/%u, output_samples=%u, stop_requests=%u\n", scenario,
           atomic_load(&wav_allocations), atomic_load(&wav_frees), atomic_load(&pcm_samples), atomic_load(&stop_requests));
    xiaotai_media_reset();
    xiaotai_framework_reset();
    xiaotai_recognition_reset();
    for (size_t i = 0; i < sizeof(signals) / sizeof(*signals); ++i) xiaotai_signal_deinit(signals[i]);
    return 0;
}
