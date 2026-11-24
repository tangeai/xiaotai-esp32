// SPDX-License-Identifier: MIT
#include "fake_media.h"
#include "fake_platform.h"
#include <assert.h>
#include <string.h>

struct fake_audio fake_audio;
struct fake_tts fake_tts;
static uint32_t sequence;

void fake_audio_reset(void)
{
    fake_audio = (struct fake_audio){.playback = true};
    fake_tts = (struct fake_tts){0};
}

void xiaotai_media_info(xiaotai_media_info_t *info)
{
    *info = (xiaotai_media_info_t){.playback = fake_audio.playback};
}

static void begin_play(xiaotai_media_play_fn_t done, void *ctx, uint32_t *id)
{
    assert(!fake_audio.play.id && done);
    *id = ++sequence;
    fake_audio.play = (fake_play_t){*id, done, ctx};
}

static bool refuses_equal(xiaotai_play_priority_t priority, uint32_t flags)
{
    return fake_audio.other_urgent && priority == XIAOTAI_PLAY_URGENT && (flags & XIAOTAI_PLAY_REFUSE_EQUAL);
}

xiaotai_err_t xiaotai_media_play_clips(const xiaotai_media_play_t *request, uint32_t *id)
{
    if (refuses_equal(request->priority, request->flags)) return XIAOTAI_ERR_BUSY;
    if (fake_audio.refuse != XIAOTAI_OK) return fake_audio.refuse;
    ++fake_audio.clips;
    fake_audio.clip = *request;
    begin_play(request->done, request->ctx, id);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_play_stream(const xiaotai_media_stream_t *request, uint32_t *id)
{
    if (refuses_equal(request->priority, request->flags)) return XIAOTAI_ERR_BUSY;
    if (fake_audio.refuse != XIAOTAI_OK) return fake_audio.refuse;
    ++fake_audio.streams;
    fake_audio.stream = *request;
    begin_play(request->done, request->ctx, id);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_play_write(uint32_t id, const void *data, size_t size, uint32_t timeout_ms)
{
    assert(id && timeout_ms && size && size <= 1024 && !(size & 1U));
    if (id != fake_audio.play.id) return XIAOTAI_ERR_INVALID_STATE;
    if (fake_audio.write_refuse != XIAOTAI_OK) return fake_audio.write_refuse;
    assert(fake_audio.size + size <= sizeof(fake_audio.bytes));
    memcpy(fake_audio.bytes + fake_audio.size, data, size);
    fake_audio.size += size;
    ++fake_audio.writes;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_play_finish(uint32_t id)
{
    assert(id);
    if (id != fake_audio.play.id) return XIAOTAI_ERR_INVALID_STATE;
    if (fake_audio.finish_refuse != XIAOTAI_OK) return fake_audio.finish_refuse;
    ++fake_audio.finishes;
    return XIAOTAI_OK;
}

void xiaotai_media_stop(uint32_t id)
{
    assert(id);
    if (id != fake_audio.play.id) return;
    fake_audio.play.id = 0;
    fake_audio.stopped_id = id;
    ++fake_audio.stops;
}

typedef struct { fake_play_t play; xiaotai_err_t result; } audio_done_t;
static void audio_done(void *ctx)
{
    const audio_done_t *call = ctx;
    if (call->play.id == fake_audio.play.id) fake_audio.play.id = 0;
    call->play.done(call->play.id, call->result, call->play.ctx);
}

void fake_audio_done(fake_play_t play, xiaotai_err_t result)
{
    assert(play.id && play.done);
    audio_done_t completion = {play, result};
    fake_on_owner(audio_done, &completion);
}

xiaotai_err_t xiaotai_platform_transfer(const xiaotai_platform_transfer_t *transfer,
                                        const xiaotai_platform_stream_t *stream, uint32_t *id)
{
    if (fake_tts.refuse != XIAOTAI_OK) return fake_tts.refuse;
    assert(!fake_tts.pending && stream->begin && stream->write && stream->done && !stream->read);
    assert(transfer->request.path && transfer->request.bearer && !transfer->request.body && !transfer->body_size);
    assert(strlen(transfer->request.path) < sizeof(fake_tts.path));
    assert(strlen(transfer->request.bearer) < sizeof(fake_tts.bearer));
    ++fake_tts.submissions;
    fake_tts.pending = true;
    fake_tts.req = *transfer;
    strcpy(fake_tts.path, transfer->request.path);
    strcpy(fake_tts.bearer, transfer->request.bearer);
    fake_tts.req.request.path = fake_tts.path;
    fake_tts.req.request.bearer = fake_tts.bearer;
    fake_tts.stream = *stream;
    *id = fake_tts.submissions;
    return XIAOTAI_OK;
}

void xiaotai_platform_transfer_cancel(uint32_t id)
{
    assert(id == fake_tts.submissions && fake_tts.pending);
    ++fake_tts.cancels;
}

xiaotai_err_t fake_tts_head(int status, const char *type, int64_t length, bool repeated)
{
    assert(fake_tts.pending);
    const xiaotai_platform_response_head_t head = {.http_status = status, .content_type = type,
        .content_length = length, .content_type_repeated = repeated};
    return fake_tts.stream.begin(fake_tts.stream.ctx, &head);
}

xiaotai_err_t fake_tts_write(const uint8_t *data, size_t size)
{
    assert(fake_tts.pending);
    return fake_tts.stream.write(fake_tts.stream.ctx, data, size);
}

static void tts_done(void *ctx)
{
    assert(fake_tts.pending);
    fake_tts.pending = false;
    fake_tts.stream.done(fake_tts.stream.ctx, ctx);
}

void fake_tts_done(xiaotai_err_t result, int status)
{
    xiaotai_platform_transfer_result_t completion = {.result = result, .http_status = status};
    fake_on_owner(tts_done, &completion);
}
