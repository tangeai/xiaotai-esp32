// SPDX-License-Identifier: MIT
#include "binding_internal.h"
#include "xiaotai/media.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/os.h"
#include "xiaotai/sounds.h"
#include <stdatomic.h>
#include <string.h>
#include <strings.h>

#define TTS_PATH "/v1/device/tts?code="
#define TTS_CODE_SIZE 6U
#define TTS_TIMEOUT_MS 20000U
#define TTS_RETRY_MS 300U
#define TTS_PCM_BYTES_MAX (160U * 1024U)
#define TTS_PCM_RATE 8000U
#define TTS_WRITE_MS 1000U
#define TTS_WRITE_BYTES 1024U

/* Only the owner touches the announcement. The platform worker borrows one
 * TTS slot until transfer_done (AUTH_BEARER already allows just one transfer).
 * Stopping invalidates its old play id before any later play can be admitted. */
static struct {
    atomic_bool active;
    uint32_t serial, attempt, play, timer;
    uint8_t rounds_left;
    bool server, retried;
} voice;

static struct {
    bool pending;                       /* owner only */
    uint32_t owner, transfer;
    atomic_uint play;                   /* zeroed by the owner on cancel */
    int64_t deadline_us;                 /* copied when queued; immutable through done */
    size_t received, delivered;         /* worker until done, then owner */
    bool half, play_ended;             /* worker until done, then owner */
    uint8_t byte;
} tts;

static bool active(void) { return atomic_load(&voice.active); }

static bool waiting_for(const binding_attempt_t *attempt)
{
    return attempt && binding_phase() == XIAOTAI_BINDING_WAITING && binding_window_left_ms(attempt) != 0U &&
           !xiaotai_quiesced() && (!binding_config()->resume_unbound_window_on_reconnect ||
             (binding_code_visible() && xiaotai_platform_guest_subscribed(attempt->guest)));
}

static bool still_current(void)
{
    const binding_attempt_t *attempt = binding_attempt();
    return waiting_for(attempt) && attempt->serial == voice.attempt && !xiaotai_activity_active();
}

static void detach_play(void)
{
    const uint32_t play = voice.play;
    voice.play = 0;
    if (tts.pending && tts.owner == voice.serial) atomic_store(&tts.play, 0U);
    if (play) xiaotai_media_stop(play);
}

static void complete(xiaotai_err_t error)
{
    if (!active()) return;
    atomic_store(&voice.active, false);
    xiaotai_timer_cancel(voice.timer);
    voice.timer = 0;
    detach_play();
    if (tts.pending && tts.owner == voice.serial) xiaotai_platform_transfer_cancel(tts.transfer);
    if (error != XIAOTAI_OK && error != XIAOTAI_ERR_INVALID_STATE) binding_count(BINDING_STAT_ANNOUNCEMENTS_FAILED);
    binding_publish();
    const xiaotai_args_t args = {.value = error};
    const xiaotai_err_t emitted = xiaotai_event_emit(XIAOTAI_EVENT_BINDING_ANNOUNCED, &args, NULL, 0);
    if (emitted != XIAOTAI_OK) XIAOTAI_LOGW(BINDING_LOG_SOURCE, "announcement result event not queued: 0x%x", (unsigned)emitted);
}

void xiaotai_binding_announce_stop(void) { complete(XIAOTAI_ERR_INVALID_STATE); }

static xiaotai_err_t open_server(const binding_attempt_t *attempt);

static void next_round(void *ctx)
{
    if (!active() || voice.serial != (uint32_t)(uintptr_t)ctx) return;
    voice.timer = 0;
    if (!still_current()) {
        complete(XIAOTAI_ERR_INVALID_STATE);
        return;
    }
    const xiaotai_err_t err = open_server(binding_attempt());
    if (err != XIAOTAI_OK) complete(err);
}

static void schedule(uint32_t delay)
{
    const xiaotai_err_t err = xiaotai_timer_start(delay, next_round, binding_serial_ctx(voice.serial), &voice.timer);
    if (err != XIAOTAI_OK) complete(err);
}

static void play_done(uint32_t id, xiaotai_err_t result, void *ctx)
{
    if (!active() || voice.serial != (uint32_t)(uintptr_t)ctx || id != voice.play) return;
    voice.play = 0;
    if (!still_current()) result = XIAOTAI_ERR_INVALID_STATE;
    if (result != XIAOTAI_OK || !voice.server || --voice.rounds_left == 0U) {
        complete(result);
    } else {
        schedule(binding_config()->announce_gap_ms);
    }
}

static uint32_t worker_play(void)
{
    if (xiaotai_quiesced() || xiaotai_activity_active() || xiaotai_time_us() >= tts.deadline_us) return 0;
    return atomic_load(&tts.play);
}

static xiaotai_err_t receive_head(void *ctx, const xiaotai_platform_response_head_t *head)
{
    (void)ctx;
    if (!worker_play()) return XIAOTAI_ERR_INVALID_STATE;
    if (head->http_status != 200 || head->truncated || head->content_type_repeated ||
        head->content_encoding_repeated || !head->content_type || strncasecmp(head->content_type, "audio/pcm", 9) ||
        (head->content_encoding && head->content_encoding[0] && strcasecmp(head->content_encoding, "identity"))) {
        return XIAOTAI_FAIL;
    }
    if (head->content_length > TTS_PCM_BYTES_MAX) return XIAOTAI_ERR_INVALID_SIZE;
    return XIAOTAI_OK;
}

static xiaotai_err_t write_pcm(uint32_t play, const void *data, size_t size)
{
    const xiaotai_err_t result = xiaotai_media_play_write(play, data, size, TTS_WRITE_MS);
    if (result == XIAOTAI_OK) tts.delivered += size;
    if (result == XIAOTAI_ERR_INVALID_STATE) tts.play_ended = true;
    return result;
}

/* Transfer pieces can split a PCM16 sample; only the final unpaired byte is
 * invalid. The platform's result tells the owner when no more bytes follow. */
static xiaotai_err_t receive_pcm(void *ctx, const uint8_t *data, size_t size)
{
    (void)ctx;
    const uint32_t play = worker_play();
    if (!play) return XIAOTAI_ERR_INVALID_STATE;
    if (size > TTS_PCM_BYTES_MAX - tts.received) return XIAOTAI_ERR_INVALID_SIZE;
    tts.received += size;
    if (size && tts.half) {
        const uint8_t sample[] = {tts.byte, *data++};
        const xiaotai_err_t err = write_pcm(play, sample, sizeof(sample));
        if (err != XIAOTAI_OK) return err;
        --size;
        tts.half = false;
    }
    while (size >= 2U) {
        if (!worker_play()) return XIAOTAI_ERR_INVALID_STATE;
        const size_t n = size >= TTS_WRITE_BYTES ? TTS_WRITE_BYTES : size & ~(size_t)1U;
        const xiaotai_err_t err = write_pcm(play, data, n);
        if (err != XIAOTAI_OK) return err;
        data += n;
        size -= n;
    }
    if (size) {
        tts.byte = data[0];
        tts.half = true;
    }
    return XIAOTAI_OK;
}

static void transfer_done(void *ctx, const xiaotai_platform_transfer_result_t *result)
{
    const uint32_t owner = (uint32_t)(uintptr_t)ctx;
    if (!tts.pending || tts.owner != owner) return;
    tts.pending = false;
    tts.transfer = 0;
    atomic_store(&tts.play, 0U);
    if (!active() || voice.serial != owner) return;
    if (!still_current() || result->result == XIAOTAI_ERR_PLATFORM_CANCELLED ||
        result->result == XIAOTAI_ERR_PLATFORM_STALE) {
        complete(XIAOTAI_ERR_INVALID_STATE);
        return;
    }
    /* A closed media stream rejects writes before its owner callback may run.
     * Only play_done knows whether it timed out, failed or was stopped. */
    if (tts.play_ended) return;
    xiaotai_err_t error = result->result;
    if (error == XIAOTAI_ERR_INVALID_STATE) {
        complete(error);
        return;
    }
    if (error == XIAOTAI_OK && result->http_status == 200 && tts.received) {
        if (tts.half) {
            complete(XIAOTAI_FAIL);
        } else {
            error = xiaotai_media_play_finish(voice.play);
            if (error != XIAOTAI_OK && error != XIAOTAI_ERR_INVALID_STATE) complete(error);
        }
        return;
    }
    if (error == XIAOTAI_OK) error = XIAOTAI_FAIL;
    /* Unlike the old whole-body download, a stream can have been heard
     * before a failure. Do not retry a prefix that may already have played. */
    if (!voice.retried && !tts.delivered) {
        voice.retried = true;
        detach_play();
        schedule(TTS_RETRY_MS);
    } else {
        complete(error);
    }
}

static xiaotai_err_t open_server(const binding_attempt_t *attempt)
{
    if (tts.pending) return XIAOTAI_ERR_BUSY;
    const xiaotai_media_stream_t audio = {
        .encoding = XIAOTAI_CODEC_PCM16, .rate_hz = TTS_PCM_RATE, .priority = XIAOTAI_PLAY_URGENT,
        .flags = XIAOTAI_PLAY_REFUSE_EQUAL,
        .done = play_done, .ctx = binding_serial_ctx(voice.serial),
    };
    xiaotai_err_t err = xiaotai_media_play_stream(&audio, &voice.play);
    if (err != XIAOTAI_OK) return err;

    char path[sizeof(TTS_PATH) + TTS_CODE_SIZE] = TTS_PATH;
    memcpy(path + sizeof(TTS_PATH) - 1U, attempt->code, TTS_CODE_SIZE);
    const xiaotai_platform_transfer_t request = {
        .request = {.service = XIAOTAI_PLATFORM_SERVICE_DEVICE, .method = XIAOTAI_PLATFORM_GET, .path = path,
                    .auth = XIAOTAI_PLATFORM_AUTH_BEARER, .bearer = attempt->temp_token, .timeout_ms = TTS_TIMEOUT_MS},
        .response_max = TTS_PCM_BYTES_MAX,
    };
    const xiaotai_platform_stream_t stream = {
        .begin = receive_head, .write = receive_pcm, .done = transfer_done, .ctx = binding_serial_ctx(voice.serial),
    };
    tts.pending = true;
    tts.owner = voice.serial;
    tts.deadline_us = attempt->deadline_us;
    tts.received = tts.delivered = 0;
    tts.half = tts.play_ended = false;
    tts.byte = 0;
    atomic_store(&tts.play, voice.play);
    err = xiaotai_platform_transfer(&request, &stream, &tts.transfer);
    if (err != XIAOTAI_OK) {
        detach_play();
        tts.pending = false;
    }
    xiaotai_identity_wipe(path, sizeof(path));
    return err;
}

static bool digits(const char *code, size_t size)
{
    if (!size) return false;
    for (size_t i = 0; i < size; ++i) if (code[i] < '0' || code[i] > '9') return false;
    return true;
}

static bool local_clips(xiaotai_media_play_t *play, const char *code, size_t size)
{
    if (size >= XIAOTAI_MEDIA_PLAY_CLIPS_MAX || !xiaotai_sounds_find(XIAOTAI_SOUND_GUIDE_BINDING)) return false;
    play->clips[0].name = XIAOTAI_SOUND_GUIDE_BINDING;
    for (size_t i = 0; i < size; ++i) {
        const char *name = xiaotai_sounds_digit((unsigned)(code[i] - '0'));
        if (!xiaotai_sounds_find(name)) return false;
        play->clips[i + 1U].name = name;
    }
    play->clip_count = (uint8_t)(size + 1U);
    return true;
}

static xiaotai_err_t busy(void)
{
    binding_count(BINDING_STAT_ANNOUNCEMENTS_BUSY);
    return XIAOTAI_ERR_BUSY;
}

xiaotai_err_t xiaotai_binding_announce(void)
{
    const binding_attempt_t *attempt = binding_attempt();
    if (!waiting_for(attempt)) return XIAOTAI_ERR_INVALID_STATE;
    if (active() || xiaotai_activity_active()) return busy();
    const xiaotai_binding_config_t *config = binding_config();
    xiaotai_media_info_t media;
    xiaotai_media_info(&media);
    const size_t size = strlen(attempt->code);
    if (config->voice == XIAOTAI_BINDING_VOICE_NONE || !media.playback || !digits(attempt->code, size))
        return XIAOTAI_ERR_NOT_SUPPORTED;

    if (++voice.serial == 0U) ++voice.serial;
    voice.attempt = attempt->serial;
    voice.server = false;
    voice.retried = false;
    voice.rounds_left = config->announce_rounds;
    xiaotai_err_t err = XIAOTAI_ERR_NOT_SUPPORTED;
    if (config->voice != XIAOTAI_BINDING_VOICE_SERVER) {
        xiaotai_media_play_t play = {
            .priority = XIAOTAI_PLAY_URGENT, .repeat = (uint16_t)(config->announce_rounds - 1U),
            .flags = XIAOTAI_PLAY_REFUSE_EQUAL,
            .gap_ms = config->announce_gap_ms, .done = play_done, .ctx = binding_serial_ctx(voice.serial),
        };
        if (local_clips(&play, attempt->code, size)) err = xiaotai_media_play_clips(&play, &voice.play);
    }
    if ((err == XIAOTAI_ERR_NOT_SUPPORTED || err == XIAOTAI_ERR_NOT_FOUND) &&
        config->voice != XIAOTAI_BINDING_VOICE_LOCAL && size == TTS_CODE_SIZE) {
        voice.server = true;
        err = open_server(attempt);
    }
    if (err == XIAOTAI_ERR_BUSY) return busy();
    if (err != XIAOTAI_OK) return err;
    atomic_store(&voice.active, true);
    binding_count(BINDING_STAT_ANNOUNCEMENTS);
    if (voice.server) binding_count(BINDING_STAT_ANNOUNCEMENTS_SERVER);
    binding_publish();
    return XIAOTAI_OK;
}

static void activity_changed(const xiaotai_event_t *event, void *ctx)
{
    (void)event; (void)ctx;
    if (xiaotai_activity_active()) xiaotai_binding_announce_stop();
}

xiaotai_err_t binding_voice_init(void)
{
    static const binding_voice_t hooks = {.halt = xiaotai_binding_announce_stop, .active = active};
    const xiaotai_err_t err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_ACTIVITY, activity_changed, NULL);
    if (err == XIAOTAI_OK) binding_voice_attach(&hooks);
    return err;
}
