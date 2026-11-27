// SPDX-License-Identifier: MIT
#include "diag.h"
#include "xiaotai/media_config.h"

#if XIAOTAI_MEDIA_TEST_COMMANDS
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "g711.h"
#include "record.h"
#include "xiaotai/port/mem.h"

enum { TONE_RATE = 8000, TONE_SAMPLES = 20, TONE_CYCLES = 200 };

static uint8_t s_tone[TONE_SAMPLES];
static const xiaotai_sound_t s_tone_sound = {
    .name = "test.tone", .encoding = XIAOTAI_CODEC_G711A, .container = XIAOTAI_SOUND_RAW,
    .rate_hz = TONE_RATE, .channels = 1, .data = s_tone, .size = sizeof(s_tone),
};
typedef struct {
    atomic_uint refs;
    uint8_t data[];
} recording_blob_t;

static uint32_t s_record_id, s_play_id;
static recording_blob_t *s_record_blob;
static xiaotai_sound_t s_record_sound = {
    .name = "test.record", .encoding = XIAOTAI_CODEC_G711A, .container = XIAOTAI_SOUND_RAW, .channels = 1,
};

static void release_blob(void *ctx)
{
    recording_blob_t *blob = ctx;
    if (blob && atomic_fetch_sub(&blob->refs, 1) == 1) xiaotai_mem_free(blob);
}

static void forget_recording(void)
{
    release_blob(s_record_blob);
    s_record_blob = NULL;
    s_record_sound.data = NULL;
    s_record_sound.size = 0;
}

void media_at_test_detach(void)
{
    // The media owner has stopped playback; the recorder returns active
    // borrows through record_done() even if its command is torn down.
    forget_recording();
    s_record_id = s_play_id = 0;
}

static void record_done(uint32_t id, xiaotai_err_t result, uint32_t samples, void *ctx)
{
    recording_blob_t *blob = ctx;
    if (s_record_id != id) {
        release_blob(blob);
        return;
    }
    s_record_id = 0;
    media_diag_diagnostic_end(true, id);
    if (result == XIAOTAI_OK && samples == s_record_sound.rate_hz) {
        s_record_blob = blob;
        s_record_sound.data = blob->data;
        s_record_sound.size = samples;
    } else {
        release_blob(blob);
    }
}

static void play_done(uint32_t id, xiaotai_err_t result, void *ctx)
{
    (void)result; (void)ctx;
    if (s_play_id == id) s_play_id = 0;
    media_diag_diagnostic_end(false, id);
}

static xiaotai_err_t play(const xiaotai_sound_t *sound, uint16_t repeat)
{
    if (s_play_id || s_record_id) return XIAOTAI_ERR_BUSY;
    const xiaotai_media_play_t request = {
        .clips = {{.sound = sound}}, .clip_count = 1, .priority = XIAOTAI_PLAY_URGENT,
        .repeat = repeat, .done = play_done,
    };
    uint32_t id = 0;
    const xiaotai_err_t err = xiaotai_media_play_clips(&request, &id);
    if (err == XIAOTAI_OK) {
        s_play_id = id;
        media_diag_diagnostic_begin(false, id);
    }
    return err;
}

static xiaotai_err_t xiaotai_media_at_test_tone(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv; (void)reply;
    return play(&s_tone_sound, TONE_CYCLES - 1);
}

static xiaotai_err_t xiaotai_media_at_test_record(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv;
    if (!xiaotai_at_reply_is_console(reply)) return XIAOTAI_ERR_NOT_ALLOWED;
    if (s_play_id || s_record_id) return XIAOTAI_ERR_BUSY;
    xiaotai_media_info_t info;
    xiaotai_media_info(&info);
    if (!info.capture_pcm || !info.capture_rate_hz) return XIAOTAI_ERR_NOT_SUPPORTED;
    // A same-rate recording can replace its old buffer after admission.
    const bool reuse = s_record_sound.rate_hz == info.capture_rate_hz && s_record_blob &&
                       atomic_load(&s_record_blob->refs) == 1;
    recording_blob_t *blob = reuse ? s_record_blob :
        xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*blob) + info.capture_rate_hz);
    if (!blob) return XIAOTAI_ERR_NO_MEM;
    if (!reuse) atomic_init(&blob->refs, 1);
    uint32_t id = 0;
    const xiaotai_err_t err = media_session_record_alaw(blob->data, info.capture_rate_hz, record_done, blob, &id);
    if (err != XIAOTAI_OK) {
        if (!reuse) release_blob(blob);
        return err;
    }
    if (!reuse) release_blob(s_record_blob);
    s_record_blob = NULL;
    s_record_sound.data = NULL;
    s_record_sound.size = 0;
    s_record_sound.rate_hz = info.capture_rate_hz;
    s_record_id = id;
    media_diag_diagnostic_begin(true, id);
    return XIAOTAI_OK;
}

static xiaotai_err_t xiaotai_media_at_test_play(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv; (void)reply;
    if (s_record_id) return XIAOTAI_ERR_BUSY;
    return s_record_sound.data ? play(&s_record_sound, 0) : XIAOTAI_ERR_INVALID_STATE;
}

static xiaotai_err_t xiaotai_media_at_test_stop(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv;
    xiaotai_media_status_t status;
    xiaotai_media_status(&status);
    if (status.generation) return xiaotai_at_fail(reply, XIAOTAI_ERR_BUSY, "ACTIVITY");
    if (s_record_id) xiaotai_media_record_stop(s_record_id);
    if (s_play_id) xiaotai_media_stop(s_play_id);
    return XIAOTAI_OK;
}

static xiaotai_err_t xiaotai_media_at_test_dump(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv;
    if (!xiaotai_at_reply_is_console(reply)) return XIAOTAI_ERR_NOT_ALLOWED;
    if (s_record_id || s_play_id) return XIAOTAI_ERR_BUSY;
    if (!s_record_blob) return XIAOTAI_ERR_INVALID_STATE;
    char header[72];
    snprintf(header, sizeof(header), "format=PCMA rate=%" PRIu32 " channels=1 source=record", s_record_sound.rate_hz);
    const xiaotai_at_blob_t blob = {
        .name = "AUDIO-DUMP", .header = header, .data = s_record_sound.data, .size = s_record_sound.size,
        .release = release_blob, .ctx = s_record_blob,
    };
    atomic_fetch_add(&s_record_blob->refs, 1);
    const xiaotai_err_t err = xiaotai_at_blob(reply, &blob);
    if (err != XIAOTAI_OK) release_blob(s_record_blob);
    else if (!xiaotai_media_param("diagnostics.keep_record_after_dump", 0)) forget_recording();
    return err;
}

static xiaotai_err_t xiaotai_media_at_test_sendtone(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)argc; (void)argv; (void)reply;
    xiaotai_media_status_t status;
    xiaotai_media_status(&status);
    return xiaotai_media_uplink_tone(status.generation, 500);
}

static xiaotai_err_t xiaotai_media_at_test_echo(int argc, const char *const *argv, const xiaotai_reply_t *reply)
{
    (void)reply;
    if (argc != 1 || (strcmp(argv[0], "0") && strcmp(argv[0], "1"))) return XIAOTAI_ERR_INVALID_ARG;
    return xiaotai_media_echo_gate_set(argv[0][0] == '1');
}

xiaotai_err_t media_at_test_register(void)
{
    static const int16_t waveform[TONE_SAMPLES] = {
        0, 927, 1763, 2427, 2853, 3000, 2853, 2427, 1763, 927,
        0, -927, -1763, -2427, -2853, -3000, -2853, -2427, -1763, -927,
    };
    for (size_t i = 0; i < TONE_SAMPLES; ++i) s_tone[i] = xiaotai_media_alaw_encode_sample(waveform[i]);
    static const xiaotai_at_verb_t verbs[] = {
        {.word = "TONE", .run = xiaotai_media_at_test_tone},
        {.word = "RECORD", .run = xiaotai_media_at_test_record},
        {.word = "PLAY", .run = xiaotai_media_at_test_play},
        {.word = "STOP", .run = xiaotai_media_at_test_stop},
        {.word = "DUMP", .run = xiaotai_media_at_test_dump},
        {.word = "SENDTONE", .run = xiaotai_media_at_test_sendtone},
        {.word = "ECHO", .min_args = 1, .max_args = 1, .usage = "<0|1>", .run = xiaotai_media_at_test_echo},
    };
    return xiaotai_at_verbs_register("AT+AUDIO", verbs, sizeof(verbs) / sizeof(*verbs));
}
#endif
