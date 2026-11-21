// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_ai_audio_drain.py
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "trace.h"
#include "xiaotai/at.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"

static char s_output[98304];
static const char *s_input;
static size_t s_left, s_used;
static bool s_external = XIAOTAI_MEDIA_TRACE_ARRIVALS != 0;
static bool s_feed_in_dump;
static bool s_start_in_dump;
static bool s_start_in_arrivals;
static uint32_t s_generation;
static _Thread_local bool s_trace_lock_busy;

bool __real_xiaotai_mutex_lock(xiaotai_mutex_t *mutex, uint32_t timeout_ms);

bool __wrap_xiaotai_mutex_lock(xiaotai_mutex_t *mutex, uint32_t timeout_ms)
{
    if (s_trace_lock_busy) {
        assert(timeout_ms == 0);
        s_trace_lock_busy = false;
        return false;
    }
    return __real_xiaotai_mutex_lock(mutex, timeout_ms);
}

static size_t read_wire(void *ctx, uint8_t *data, size_t size, uint32_t wait_ms)
{
    (void)ctx;
    if (!s_left) {
        xiaotai_delay_ms(wait_ms);
        return 0;
    }
    const size_t n = s_left < size ? s_left : size;
    memcpy(data, s_input, n);
    s_input += n;
    s_left -= n;
    return n;
}

static xiaotai_err_t write_wire(void *ctx, const char *data, size_t size)
{
    (void)ctx;
    assert(size >= 2 && !memcmp(data + size - 2, "\r\n", 2));
    if ((size >= 7 && !memcmp(data, "+TRACE:", 7)) || (size >= 11 && !memcmp(data, "+AUDIO-DUMP", 11)))
        assert(!xiaotai_on_owner());
    if (s_feed_in_dump && size >= 8 && !memcmp(data, "+TRACE: ", 8)) {
        s_feed_in_dump = false;
        media_trace_frame(false, 11, (const uint8_t *)"abc", 3, 51, 0, 3, 200, 20);
    }
    if (s_start_in_dump && size >= 8 && !memcmp(data, "+TRACE: ", 8)) {
        s_start_in_dump = false;
        s_generation = 81;
    }
    if (s_start_in_arrivals && size >= 13 && !memcmp(data, "+ATRACE:BEGIN", 13)) {
        s_start_in_arrivals = false;
        s_generation = 91;
    }
    assert(s_used + size < sizeof(s_output));
    memcpy(s_output + s_used, data, size);
    s_used += size;
    s_output[s_used] = 0;
    return XIAOTAI_OK;
}

static void send(const char *request)
{
    s_input = request;
    s_left = strlen(request);
    s_used = 0;
    s_output[0] = 0;
    for (unsigned i = 0; i < 5 && s_left; ++i) assert(xiaotai_at_service(20) == XIAOTAI_OK);
    assert(!s_left);
}

static void expect(const char *request, const char *text, bool exact)
{
    send(request);
    const bool found = exact ? strcmp(s_output, text) == 0 : strstr(s_output, text) != NULL;
    if (!found) fprintf(stderr, "%s -> %s; expected %s\n", request, s_output, text);
    assert(found);
}

static void fake_part_stats(void *self, xiaotai_audio_stats_t *out)
{
    (void)self;
    *out = (xiaotai_audio_stats_t){.overruns = 2, .lost_frames = 10, .errors = 7};
}

static void synthetic_section(const xiaotai_reply_t *out)
{
    assert(xiaotai_on_owner());
    out->write(out->ctx, "ready=1\n\n+OWN-EXTRA: samples=2\nstatus=ok");
}

static void empty_section(const xiaotai_reply_t *out) { (void)out; }

static xiaotai_err_t invalid_probe(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv; (void)reply;
    assert(false);
    return XIAOTAI_FAIL;
}

static xiaotai_err_t init(void)
{
    assert(xiaotai_eventlog_init(NULL) == XIAOTAI_OK);
    assert(xiaotai_engine_at_init() == XIAOTAI_OK);
    assert(xiaotai_at_local_register("AT+BAD??", NULL, invalid_probe) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_at_local_register("AT+?", NULL, invalid_probe) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_at_local_register("AT+BAD?TAIL", NULL, invalid_probe) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_at_local_register("AT+BAD?", "<name>", invalid_probe) == XIAOTAI_ERR_INVALID_ARG);
    assert(xiaotai_diag_register("aud-test", synthetic_section) == XIAOTAI_OK);
    assert(xiaotai_diag_register("app.voice_test", synthetic_section) == XIAOTAI_OK);
    assert(xiaotai_diag_register("empty-test", empty_section) == XIAOTAI_OK);
    return media_diag_register();
}

bool __wrap_xiaotai_mem_has_external(void) { return s_external; }

void __wrap_xiaotai_media_status(xiaotai_media_status_t *status)
{
    *status = (xiaotai_media_status_t){.ready = true, .generation = s_generation, .active = s_generation != 0};
}

void __wrap_xiaotai_media_info(xiaotai_media_info_t *info)
{
    *info = (xiaotai_media_info_t){.capture_pcm = true, .capture_rate_hz = 8000, .processor = "echo_gate"};
}

static bool s_echo_enabled = true;
static unsigned s_echo_changes;

bool __wrap_xiaotai_media_echo_gate_enabled(void)
{
    assert(xiaotai_on_owner());
    return s_echo_enabled;
}

xiaotai_err_t __wrap_xiaotai_media_echo_gate_set(bool enabled)
{
    assert(xiaotai_on_owner() && s_generation == 15U);
    s_echo_enabled = enabled;
    ++s_echo_changes;
    return XIAOTAI_OK;
}

#if XIAOTAI_MEDIA_TEST_COMMANDS
static xiaotai_media_play_t s_play;
static unsigned s_play_id, s_stops, s_tones;
static uint8_t *s_borrow;
static void *s_record_ctx;
static xiaotai_media_record_fn_t s_record_done;

xiaotai_err_t __wrap_xiaotai_media_play_clips(const xiaotai_media_play_t *request, uint32_t *id)
{
    assert(xiaotai_on_owner());
    s_play = *request;
    *id = ++s_play_id;
    return XIAOTAI_OK;
}

void __wrap_xiaotai_media_stop(uint32_t id)
{
    assert(xiaotai_on_owner() && id == s_play_id);
    ++s_stops;
    s_play.done(id, XIAOTAI_ERR_INVALID_STATE, s_play.ctx);
}

xiaotai_err_t __wrap_media_session_record_alaw(uint8_t *data, uint32_t samples, xiaotai_media_record_fn_t done,
                                              void *ctx, uint32_t *id)
{
    assert(xiaotai_on_owner() && samples == 8000 && !s_borrow);
    s_borrow = data;
    s_record_ctx = ctx;
    s_record_done = done;
    *id = 42;
    memset(data, 0xd5, samples);
    return XIAOTAI_OK;
}

void __wrap_xiaotai_media_record_stop(uint32_t id)
{
    assert(xiaotai_on_owner() && s_record_done && id == 42);
    s_record_done(id, XIAOTAI_ERR_INVALID_STATE, 0, s_record_ctx);
    s_record_done = NULL;
    s_borrow = NULL;
}

xiaotai_err_t __wrap_xiaotai_media_uplink_tone(uint32_t generation, uint32_t duration_ms)
{
    assert(xiaotai_on_owner() && duration_ms == 500);
    if (!generation) return XIAOTAI_ERR_INVALID_STATE;
    assert(generation == s_generation);
    ++s_tones;
    return XIAOTAI_OK;
}

static void verify_optional_commands(void)
{
    static const uint8_t sound_bytes[] = {0xd5};
    static const xiaotai_sound_t sounds[] = {
        {.name = "guide.test", .data = sound_bytes, .size = sizeof(sound_bytes), .rate_hz = 8000,
         .channels = 1, .encoding = XIAOTAI_CODEC_G711A},
    };
    static const xiaotai_sound_pack_t pack = {"fixture", sounds, 1};
    assert(xiaotai_sounds_register(&pack) == XIAOTAI_OK);
    expect("AT+SOUND\r", "+SOUND: name=guide.test kind=guide\r\nOK\r\n", true);
    expect("AT+SOUND PLAY guide.test 12x456\r", "ERROR: INVALID_ARG GUIDE_DIGITS\r\n", true);
    expect("AT+SOUND PLAY guide.test 123456\r", "OK\r\n", true);
    assert(s_play.clip_count == 7 && s_play.priority == XIAOTAI_PLAY_URGENT && media_diag_audio_active());
    assert(!strcmp(s_play.clips[0].name, "guide.test") && !strcmp(s_play.clips[6].name, "digit.6"));
    expect("AT+SOUND STOP\r", "OK\r\n", true);
    assert(s_stops == 1 && !media_diag_audio_active());
    expect("AT+AUDIO TONE\r", "OK\r\n", true);
    assert(s_play.repeat == 199 && s_play.clips[0].sound->size == 20);

    s_generation = 15;
    expect("AT+AUDIO ECHO 2\r", "ERROR: INVALID_ARG\r\n", true);
    expect("AT+AUDIO ECHO 0\r", "OK\r\n", true);
    expect("AT+STATUS media\r", "+ECHO: enabled=0", false);
    expect("AT+AUDIO ECHO 1\r", "OK\r\n", true);
    expect("AT+STATUS media\r", "+ECHO: enabled=1", false);
    assert(s_echo_changes == 2 && s_generation == 15U);
    expect("AT+AUDIO STOP\r", "ERROR: BUSY ACTIVITY\r\n", true);
    assert(s_stops == 1 && media_diag_audio_active());
    expect("AT+AUDIO SENDTONE\r", "OK\r\n", true);
    assert(s_tones == 1);
    s_generation = 0;
    expect("AT+AUDIO STOP\r", "OK\r\n", true);
    expect("AT+AUDIO PLAY\r", "ERROR: INVALID_STATE\r\n", true);
    expect("AT+AUDIO RECORD\r", "OK\r\n", true);
    assert(s_record_done && media_diag_audio_active());
    expect("AT+AUDIO DUMP\r", "ERROR: BUSY\r\n", true);
    s_record_done(42, XIAOTAI_OK, 8000, s_record_ctx);
    s_record_done = NULL;
    s_borrow = NULL;
    assert(!media_diag_audio_active());
    expect("AT+AUDIO PLAY\r", "OK\r\n", true);
    assert(s_play.clips[0].sound && s_play.clips[0].sound->size == 8000);
    expect("AT+AUDIO STOP\r", "OK\r\n", true);
    expect("AT+AUDIO DUMP\r", "+AUDIO-DUMP: format=PCMA rate=8000 channels=1 source=record bytes=8000", false);
    assert(strstr(s_output, "+AUDIO-DUMP-DATA: seq=0 hex=D5D5D5"));
    assert(strstr(s_output, "+AUDIO-DUMP-END: chunks=63 fnv="));
    expect("AT+AUDIO DUMP\r", "ERROR: INVALID_STATE\r\n", true);
    static const xiaotai_media_param_t keep[] = {{"diagnostics.keep_record_after_dump", 1}};
    assert(xiaotai_media_tune(keep, 1) == XIAOTAI_OK);
    expect("AT+AUDIO RECORD\r", "OK\r\n", true);
    uint8_t *const first = s_borrow;
    s_record_done(42, XIAOTAI_OK, 8000, s_record_ctx);
    s_record_done = NULL;
    s_borrow = NULL;
    expect("AT+AUDIO DUMP\r", "+AUDIO-DUMP-END: chunks=63 fnv=", false);
    expect("AT+AUDIO DUMP\r", "+AUDIO-DUMP-END: chunks=63 fnv=", false);
    expect("AT+AUDIO PLAY\r", "OK\r\n", true);
    expect("AT+AUDIO STOP\r", "OK\r\n", true);
    expect("AT+AUDIO RECORD\r", "OK\r\n", true);
    assert(s_borrow == first);
    expect("AT+AUDIO STOP\r", "OK\r\n", true);
    xiaotai_sounds_reset();
}

static void verify_trace(void)
{
    static const xiaotai_media_param_t stop[] = {{"diagnostics.trace_stop_on_dump", 1}};
    static const xiaotai_media_param_t live[] = {{"diagnostics.trace_stop_on_dump", 0}};
    static const xiaotai_media_param_t idle[] = {{"diagnostics.trace_dump_idle_only", 1}};
    assert(xiaotai_media_tune(stop, 1) == XIAOTAI_OK);
    const int64_t baseline = xiaotai_mem_host_outstanding();
    s_external = false;
    expect("AT+TRACE ARM 49\r", "ERROR: INVALID_ARG FRAME_COUNT\r\n", true);
    expect("AT+TRACE ARM 3001\r", "ERROR: INVALID_ARG FRAME_COUNT\r\n", true);
    expect("AT+TRACE ARM\r", "OK\r\n", true);
    media_trace_frame(true, 65537, (const uint8_t *)"abc", 3, 31, 11, 2, 0, 20);
    media_trace_frame(false, 2, (const uint8_t *)"abc", 3, 22, 0, 3, UINT32_C(4294901234), 40);
    expect("AT+TRACE DUMP\r", "+TRACE: active=0 tx=1 rx=1 capacity=750 skipped=0\r\n"
           "+TRACE:0,0,1,3,1a47e90b,31,11,2\r\n"
           "+TRACE:1,0,2,3,1a47e90b,22,0,3,4294901234,40\r\nOK\r\n", true);
    media_trace_frame(true, 3, NULL, 0, 0, 0, 3, 0, 0);
    expect("AT+TRACE DUMP\r", "tx=1 rx=1 capacity=750", false);
    s_external = true;
    expect("AT+TRACE ARM\r", "OK\r\n", true);
    expect("AT+TRACE DUMP\r", "tx=0 rx=0 capacity=3000", false);
    expect("AT+TRACE ARM 30000\r", "OK\r\n", true);
    expect("AT+TRACE ARM 30001\r", "ERROR: INVALID_ARG FRAME_COUNT\r\n", true);
    expect("AT+TRACE DUMP\r", "tx=0 rx=0 capacity=30000", false);
    xiaotai_mem_host_fail_nth(2);
    expect("AT+TRACE ARM 50\r", "ERROR: NO_MEM\r\n", true);
    expect("AT+TRACE DUMP\r", "tx=0 rx=0 capacity=0", false);
    expect("AT+TRACE OFF\r", "OK\r\n", true);
    assert(xiaotai_mem_host_outstanding() == baseline);
    assert(xiaotai_media_tune(live, 1) == XIAOTAI_OK);
    expect("AT+TRACE ARM 50\r", "OK\r\n", true);
    s_trace_lock_busy = true;
    media_trace_frame(false, 10, (const uint8_t *)"abc", 3, 50, 0, 3, 180, 20);
    assert(!s_trace_lock_busy);
    s_feed_in_dump = true;
    expect("AT+TRACE DUMP\r", "+TRACE: active=1 tx=0 rx=0 capacity=50 skipped=1\r\nOK\r\n", true);
    expect("AT+TRACE DUMP\r", "+TRACE: active=1 tx=0 rx=1 capacity=50 skipped=1\r\n"
           "+TRACE:1,0,11,3,1a47e90b,51,0,3,200,20\r\nOK\r\n", true);
    assert(xiaotai_media_tune(idle, 1) == XIAOTAI_OK);
    s_generation = 80;
    expect("AT+TRACE DUMP\r", "ERROR: INVALID_STATE ACTIVITY\r\n", true);
    s_generation = 0;
    s_start_in_dump = true;
    expect("AT+TRACE DUMP\r", "+TRACE: active=1 tx=0 rx=1 capacity=50 skipped=1\r\n"
           "ERROR: INVALID_STATE ACTIVITY\r\n", true);
    s_generation = 0;
    expect("AT+TRACE OFF\r", "OK\r\n", true);
    const int16_t dc[] = {666, 666, 666, 666};
    const int16_t ac[] = {1000, -1000, 1000, -1000};
    assert(media_trace_db(dc, 4) == 0 && media_trace_db(ac, 4) == 59);
}
#endif

#if XIAOTAI_MEDIA_TRACE_ARRIVALS
static void unexpected_reply(void *ctx, const char *text)
{
    (void)ctx; (void)text;
    assert(false);
}

static void verify_arrivals(void)
{
    const uint8_t payload[] = {3, 9, 0xf0};
    const xiaotai_media_packet_t packet = {.data = payload, .size = sizeof(payload), .timestamp_ms = 899};
    media_trace_session_open(3, 0);
    media_trace_arrival(3, 912, &packet);
    expect("AT+AUDIOTRACE?\r", "+ATRACE:BEGIN,g=0,count=0\r\n+ATRACE:END\r\nOK\r\n", true);
    const xiaotai_reply_t framework_reply = {.write = unexpected_reply};
    assert(xiaotai_command_run("AT+AUDIOTRACE?", &framework_reply, 500) == XIAOTAI_ERR_NOT_FOUND);
    expect("AT+CLI OFF\r", "OK\r\n", true);
    expect("AT+AUDIOTRACE?\r", "ERROR: INVALID_STATE CLI_DISABLED\r\n", true);
    expect("AT+CLI ON\r", "OK\r\n", true);
    expect("AT+AUDIOTRACE??\r", "ERROR: NOT_FOUND UNKNOWN_COMMAND\r\n", true);
    expect("AT+AUDIOTRACE? EXTRA\r", "ERROR: INVALID_ARG ARGUMENT_COUNT\r\n", true);

    const uint32_t allocations = xiaotai_mem_host_allocations();
    media_trace_session_open(4, XIAOTAI_MEDIA_SESSION_TRACE_ARRIVALS);
    media_trace_arrival(3, 913, &packet);
    media_trace_session_close(3);
    media_trace_arrival(4, 914, &packet);
    media_trace_session_close(4);
    media_trace_arrival(4, 915, &packet);
    assert(xiaotai_mem_host_allocations() == allocations);
    expect("AT+AUDIOTRACE?\r", "+ATRACE:BEGIN,g=4,count=1\r\n+ATRACE:0,914,899,3,3,9\r\n+ATRACE:END\r\nOK\r\n", true);
    s_generation = 4;
    expect("AT+AUDIOTRACE?\r", "ERROR: INVALID_STATE AUDIO_TRACE_BUSY\r\n", true);
    s_generation = 0;
    s_start_in_arrivals = true;
    expect("AT+AUDIOTRACE?\r", "+ATRACE:BEGIN,g=4,count=1\r\nERROR: INVALID_STATE AUDIO_TRACE_BUSY\r\n", true);
    s_generation = 0;
    media_trace_session_open(5, XIAOTAI_MEDIA_SESSION_TRACE_ARRIVALS);
    const uint32_t usage = xiaotai_mem_host_allocations();
    for (uint32_t i = 0; i < 1050; ++i) media_trace_arrival(5, i, &packet);
    assert(xiaotai_mem_host_allocations() == usage);
    expect("AT+AUDIOTRACE?\r", "+ATRACE:BEGIN,g=5,count=1024\r\n+ATRACE:0,0,899,3,3,9", false);
    assert(strstr(s_output, "+ATRACE:1023,1023,899,3,3,9\r\n+ATRACE:END") && !strstr(s_output, "+ATRACE:1024,"));
}
#endif

static void verify_topics(void)
{
    const xiaotai_media_stats_t stats = {.mic_rms = 15, .reference_peak = 235, .capture_errors = 19,
        .target_ms = 140, .write_failed = 3, .tx_sent = 9, .uplink_clipped = 7};
    const int32_t dc[] = {-150, 300, 0, -10};
    media_diag_capture_window(&stats, dc);
    media_diag_playout_window(&stats, 3300, 2, 15, 1);
    media_diag_uplink_window(&stats);
    assert(xiaotai_eventlog_topic_count() == 3);
    xiaotai_eventlog_topic_snapshot_t topic;
    assert(xiaotai_eventlog_topic_read(0, &topic) && !strcmp(topic.desc->name, "capture"));
    assert(!strcmp(topic.desc->fields[0], "mic_rms") && topic.values[0] == 15);
    assert(!strcmp(topic.desc->fields[3], "ref_peak") && topic.values[3] == 235);
    assert(!strcmp(topic.desc->fields[12], "errors") && topic.values[12] == 19);
    assert(!strcmp(topic.desc->fields[13], "mic_dc_offset") && topic.values[13] == 32768 - 150);
    assert(topic.values[14] == 32768 + 300 && topic.values[15] == 32768 && topic.values[16] == 32768 - 10);
    assert(xiaotai_eventlog_topic_read(1, &topic) && !strcmp(topic.desc->name, "playout"));
    assert(topic.values[0] == 140 && topic.values[8] == 3);
    assert(!strcmp(topic.desc->fields[11], "peak") && topic.values[11] == 3300);
    assert(topic.values[12] == 2 && topic.values[13] == 15 && topic.values[14] == 1);
    assert(xiaotai_eventlog_topic_read(2, &topic) && !strcmp(topic.desc->name, "uplink"));
    assert(topic.values[0] == 9 && topic.values[14] == 7);
}

int main(int argc, char **argv)
{
    static const xiaotai_at_io_t io = {.name = "m11", .read = read_wire, .write = write_wire};
    const xiaotai_at_config_t console = {.task_stack_bytes = XIAOTAI_AT_NO_TASK, .no_platform_console = true};
    const xiaotai_module_t owner = {.name = "fixture", .init = init, .start = xiaotai_engine_at_start};
    const xiaotai_module_t *const mods[] = {&owner};
    assert(xiaotai_at_configure(&console) == XIAOTAI_OK);
    assert(xiaotai_at_attach(&io) == XIAOTAI_OK);
    assert(xiaotai_framework_start(&(xiaotai_framework_config_t){.modules = mods, .module_count = 1}) == XIAOTAI_OK);
    assert(xiaotai_at_service(2) == XIAOTAI_OK);
    if (argc == 2 && !strcmp(argv[1], "help")) {
        send("AT+HELP\r");
        fputs(s_output, stdout);
    } else {
        static const xiaotai_audio_input_t input = {
            .desc = {.channels = 2, .encoding = XIAOTAI_CODEC_PCM16,
                     .roles = {XIAOTAI_CHANNEL_MIC, XIAOTAI_CHANNEL_REFERENCE}},
            .stats = fake_part_stats,
        };
        const int16_t pcm[] = {INT16_MIN, 0, 582, -227};
        media_diag_input(&input);
        media_diag_capture(&input, pcm, 2);
        const char *audio = "+AUDIO: blocks=1 overflow=2 errors=7\r\n"
                            "+AUDIO-SLOT: slot=0 role=mic peak=32768 nonzero=2\r\n"
                            "+AUDIO-SLOT: slot=1 role=reference peak=227 nonzero=1\r\nOK\r\n";
        expect("AT+AUDIO\r", audio, true);
        expect("AT+STATUS media\r", audio, false);
        assert(strstr(s_output, " pcm_written=0 write_failed=0\r\n"));
        expect("AT+STATUS aud-test\r", "+AUD-TEST: ready=1\r\n+OWN-EXTRA: samples=2\r\n"
                                      "+AUD-TEST: status=ok\r\nOK\r\n", true);
        expect("AT+STATUS app.voice_test\r", "+APP-VOICE_TEST: ready=1\r\n+OWN-EXTRA: samples=2\r\n"
                                            "+APP-VOICE_TEST: status=ok\r\nOK\r\n", true);
        expect("AT+STATUS\r", "+FRAMEWORK: owner_stack=", false);
        assert(strstr(s_output, "+AUD-TEST: ready=1") && strstr(s_output, "+AUDIO-SLOT: slot=1"));
        expect("AT+STATUS empty-test\r", "ERROR: NOT_FOUND SECTION\r\n", true);
        expect("AT+STATUS missing\r", "ERROR: NOT_FOUND SECTION\r\n", true);
        expect("AT+STATUS ?\r", "ERROR: INVALID_ARG SECTION_NAME\r\n", true);
        expect("AT+STATUS app..voice_test\r", "ERROR: INVALID_ARG SECTION_NAME\r\n", true);
        expect("AT+HELP\r", "+HELP: AT+STATUS [<section>]\r\n", false);
        verify_topics();
#if XIAOTAI_MEDIA_TEST_COMMANDS
        verify_optional_commands();
        verify_trace();
#else
        expect("AT+AUDIO STOP\r", "ERROR: INVALID_ARG UNKNOWN_VERB\r\n", true);
        expect("AT+AUDIO ECHO 0\r", "ERROR: INVALID_ARG UNKNOWN_VERB\r\n", true);
        expect("AT+SOUND\r", "ERROR: NOT_FOUND UNKNOWN_COMMAND\r\n", true);
        expect("AT+TRACE ARM\r", "ERROR: NOT_FOUND UNKNOWN_COMMAND\r\n", true);
#endif
#if XIAOTAI_MEDIA_TRACE_ARRIVALS
        verify_arrivals();
#else
        expect("AT+AUDIOTRACE?\r", "ERROR: NOT_FOUND UNKNOWN_COMMAND\r\n", true);
#endif
    }
    media_diag_detach();
    xiaotai_framework_reset();
    xiaotai_at_reset();
    xiaotai_eventlog_reset();
    return 0;
}
