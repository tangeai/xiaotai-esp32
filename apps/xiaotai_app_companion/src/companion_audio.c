// SPDX-License-Identifier: MIT
#include "companion_internal.h"

#include <string.h>

#include "xiaotai/media.h"
#include "xiaotai/port/mem.h"

enum { TONE_RATE = 8000, TONE_BYTES = TONE_RATE / 2, RECORD_RATE_MAX = 48000 };
typedef enum { AUDIO_TONE, AUDIO_RECORD, AUDIO_REPLAY, AUDIO_STOP } audio_action_t;

typedef struct {
    xiaotai_sound_t sound;
    int16_t pcm[];
} audio_buffer_t;

static struct {
    audio_buffer_t *saved, *borrowed;
    uint32_t id, shown_generation;
    audio_action_t action;
    bool stopping, failed;
} s;

static void refresh(void)
{
    if (companion_on_page(XIAOTAI_APP_COMPANION_PAGE_AUDIO)) companion_render();
}

static void record_done(uint32_t id, xiaotai_err_t result, uint32_t samples, void *ctx)
{
    audio_buffer_t *buffer = ctx;
    if (s.borrowed != buffer || s.id != id) {
        /* A reset detached the UI; the engine returned its last borrow. */
        xiaotai_mem_free(buffer);
        return;
    }
    s.borrowed = NULL;
    s.id = 0;
    const bool complete = result == XIAOTAI_OK && samples == buffer->sound.rate_hz;
    s.failed = !s.stopping && !complete && result != XIAOTAI_ERR_INVALID_STATE;
    if (!s.stopping && complete) s.saved = buffer;
    else xiaotai_mem_free(buffer);
    s.stopping = false;
    refresh();
}

static void play_done(uint32_t id, xiaotai_err_t result, void *ctx)
{
    audio_buffer_t *buffer = ctx;
    if (s.borrowed != buffer || s.id != id) {
        xiaotai_mem_free(buffer);
        return;
    }
    s.borrowed = NULL;
    s.id = 0;
    s.failed = !s.stopping && result != XIAOTAI_OK && result != XIAOTAI_ERR_INVALID_STATE;
    if (s.action == AUDIO_REPLAY) s.saved = buffer;
    else xiaotai_mem_free(buffer);
    s.stopping = false;
    refresh();
}

static audio_buffer_t *allocate(uint32_t rate, xiaotai_codec_t encoding, size_t bytes)
{
    audio_buffer_t *buffer = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*buffer) + bytes);
    if (buffer) buffer->sound = (xiaotai_sound_t){.encoding = encoding, .container = XIAOTAI_SOUND_RAW,
        .rate_hz = rate, .channels = 1, .size = bytes, .data = (const uint8_t *)buffer->pcm};
    return buffer;
}

static xiaotai_err_t start_record(void)
{
    xiaotai_media_info_t info;
    xiaotai_media_info(&info);
    if (!info.capture_pcm || !info.capture_rate_hz || info.capture_rate_hz > RECORD_RATE_MAX)
        return XIAOTAI_ERR_NOT_SUPPORTED;
    const bool reuse = s.saved && s.saved->sound.rate_hz == info.capture_rate_hz;
    audio_buffer_t *buffer = reuse ? s.saved :
        allocate(info.capture_rate_hz, XIAOTAI_CODEC_PCM16, info.capture_rate_hz * sizeof(int16_t));
    if (!buffer) return XIAOTAI_ERR_NO_MEM;
    const xiaotai_media_record_t request = {.pcm = buffer->pcm, .samples = info.capture_rate_hz,
        .done = record_done, .ctx = buffer};
    uint32_t id = 0;
    const xiaotai_err_t err = xiaotai_media_record(&request, &id);
    if (err != XIAOTAI_OK) {
        if (!reuse) xiaotai_mem_free(buffer);
        return err;
    }
    if (!reuse) xiaotai_mem_free(s.saved);
    s.saved = NULL;
    s.borrowed = buffer;
    s.id = id;
    s.action = AUDIO_RECORD;
    return XIAOTAI_OK;
}

static audio_buffer_t *make_tone(void)
{
    /* The legacy diagnostic plays 25 20-ms frames, each with eight periods of its
     * 400 Hz, 3000-count PCM sine after G.711 A-law quantization. */
    static const uint8_t period[] = {0xd5, 0xf9, 0xee, 0x97, 0x93, 0x92, 0x93, 0x97, 0xee, 0xf9,
        0xd5, 0x79, 0x6e, 0x17, 0x13, 0x12, 0x13, 0x17, 0x6e, 0x79};
    audio_buffer_t *buffer = allocate(TONE_RATE, XIAOTAI_CODEC_G711A, TONE_BYTES);
    if (!buffer) return NULL;
    for (size_t i = 0; i < TONE_BYTES; i += sizeof(period)) memcpy((uint8_t *)buffer->pcm + i, period, sizeof(period));
    return buffer;
}

static xiaotai_err_t start_play(audio_action_t action)
{
    const bool replay = action == AUDIO_REPLAY;
    audio_buffer_t *buffer = replay ? s.saved : make_tone();
    if (!buffer) return replay ? XIAOTAI_ERR_INVALID_STATE : XIAOTAI_ERR_NO_MEM;
    const xiaotai_media_play_t request = {.clips = {{.sound = &buffer->sound}}, .clip_count = 1,
        .priority = XIAOTAI_PLAY_CUE, .flags = XIAOTAI_PLAY_REFUSE_EQUAL, .done = play_done, .ctx = buffer};
    uint32_t id = 0;
    const xiaotai_err_t err = xiaotai_media_play_clips(&request, &id);
    if (err != XIAOTAI_OK) {
        if (!replay) xiaotai_mem_free(buffer);
        return err;
    }
    if (replay) s.saved = NULL;
    s.borrowed = buffer;
    s.id = id;
    s.action = action;
    return XIAOTAI_OK;
}

/* Navigation can stop only the operation borrowed by this page. A hangup is
 * reserved for the user's explicit Stop, and goes through the session owner. */
void companion_audio_leave(void)
{
    if (!s.borrowed || s.stopping) return;
    s.stopping = true;
    if (s.action == AUDIO_RECORD) xiaotai_media_record_stop(s.id);
    else xiaotai_media_stop(s.id);
}

static xiaotai_err_t stop(const xiaotai_media_status_t *media)
{
    /* A session can preempt diagnostics before the terminal callback has
     * returned the old borrow. Only the current owner may handle this Stop. */
    if (media->active && media->generation) {
        const char *owner = NULL;
        const uint32_t activity = xiaotai_activity_current(&owner);
        if (!activity || s.shown_generation != media->generation ||
            xiaotai_activity_generation(activity) != media->generation) return XIAOTAI_ERR_INVALID_STATE;
        const char *action = !strcmp(owner, "call") ? "hangup" : !strcmp(owner, "ai") ? "talk.stop" :
                             !strcmp(owner, "room") ? "room.disconnect" : !strcmp(owner, "stream") ? "stream.stop" : NULL;
        if (!action) return XIAOTAI_ERR_NOT_SUPPORTED;
        xiaotai_args_t args = {0};
        strcpy(args.text, action);
        memcpy(&args.value, &media->generation, sizeof(args.value));
        return xiaotai_intent_post(XIAOTAI_INTENT_APP_COMPANION_SESSION, &args);
    }
    companion_audio_leave();
    return XIAOTAI_OK;
}

static void on_audio(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!companion_on_page(XIAOTAI_APP_COMPANION_PAGE_AUDIO)) {
        ++companion.status.stale;
        return;
    }
    xiaotai_media_status_t media;
    xiaotai_media_status(&media);
    xiaotai_err_t err;
    if (args->value == AUDIO_STOP) err = stop(&media);
    else if (args->value < AUDIO_TONE || args->value > AUDIO_REPLAY) err = XIAOTAI_ERR_INVALID_ARG;
    else if (xiaotai_quiesced() || s.borrowed || media.recording || media.playing || media.active)
        err = XIAOTAI_ERR_BUSY;
    else if (!media.ready) err = XIAOTAI_ERR_INVALID_STATE;
    else if (args->value == AUDIO_RECORD) err = start_record();
    else err = start_play((audio_action_t)args->value);
    if (err != XIAOTAI_OK) {
        companion_refuse(XIAOTAI_INTENT_APP_COMPANION_AUDIO, err);
        if (err != XIAOTAI_ERR_BUSY && err != XIAOTAI_ERR_INVALID_ARG && err != XIAOTAI_ERR_INVALID_STATE) s.failed = true;
    } else s.failed = false;
    refresh();
}

static void build(xiaotai_view_t *view, int32_t arg)
{
    (void)arg;
    static const char *const labels[] = {"播放提示音", "录音", "回放录音", "停止"};
    companion_view_buffer_t *buffer = companion.buffer;
    xiaotai_media_status_t media;
    xiaotai_media_status(&media);
    s.shown_generation = media.active ? media.generation : 0;
    const bool busy = s.borrowed || media.recording || media.playing || media.active;
    view->kind = XIAOTAI_VIEW_LIST;
    view->status = !media.ready || s.failed ? "异常" : busy ? "进行中" : s.saved ? "已录音" : "空闲";
    for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        const bool disabled = i != AUDIO_STOP && (!media.ready || busy || (i == AUDIO_REPLAY && !s.saved));
        buffer->items[i] = (xiaotai_view_item_t){.label = labels[i], .intent = XIAOTAI_INTENT_APP_COMPANION_AUDIO,
            .value = (int32_t)i, .state = disabled ? XIAOTAI_STATE_DISABLED : 0};
    }
    buffer->actions[0] = (xiaotai_view_action_t){.intent = XIAOTAI_INTENT_APP_COMPANION_BACK,
        .icon = XIAOTAI_ICON_BACK, .role = XIAOTAI_ROLE_BACK};
    view->items = buffer->items;
    view->item_count = sizeof(labels) / sizeof(labels[0]);
    view->actions = buffer->actions;
    view->action_count = 1;
}

static void changed(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    refresh();
}

xiaotai_err_t companion_audio_init(void)
{
    static const companion_page_t page = {.id = XIAOTAI_APP_COMPANION_PAGE_AUDIO, .title = "音频诊断", .build = build};
    xiaotai_err_t err = companion_page_register(&page);
    if (err == XIAOTAI_OK) err =
        xiaotai_intent_handle_ex(XIAOTAI_INTENT_APP_COMPANION_AUDIO, on_audio, NULL, XIAOTAI_INTENT_LOCAL);
    static const xiaotai_subscriber_descriptor_t events[] = {
        {XIAOTAI_EVENT_MEDIA_OUTPUT_FREE, changed, NULL},
        {XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR, changed, NULL},
        {XIAOTAI_EVENT_ACTIVITY, changed, NULL},
    };
    for (size_t i = 0; err == XIAOTAI_OK && i < sizeof(events) / sizeof(events[0]); ++i)
        err = xiaotai_event_subscribe_static(&events[i]);
    return err;
}

void companion_audio_reset(void)
{
    xiaotai_mem_free(s.saved);
    /* A pending done() still owns the borrowed pointer even after this
     * host-only reset; its callback frees it without publishing a view. */
    memset(&s, 0, sizeof(s));
}
