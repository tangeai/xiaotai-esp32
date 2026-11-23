// SPDX-License-Identifier: MIT
#include "clip_playout.h"

#include <string.h>
#include "g711.h"
#include "mixer.h"
#include "registry.h"
#include "resample.h"
#include "xiaotai/framework.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

enum {
    COMPLETIONS_MAX = 8, SOURCE_BLOCK = 64, PLAY_BLOCK_MS = 20, RETRY_MS = 5,
};

typedef struct {
    const xiaotai_audio_decoder_t *decoder;
    xiaotai_media_format_t format;
    void *state;
} decoder_slot_t;

typedef enum { COMPLETION_FREE, COMPLETION_RESERVED, COMPLETION_READY, COMPLETION_POSTED } completion_state_t;
typedef struct {
    completion_state_t state;
    xiaotai_media_play_fn_t fn;
    void *ctx;
    media_stream_buffer_t *stream;
    uint32_t id;
    xiaotai_err_t result;
} completion_t;

typedef struct {
    media_clip_request_t request;
    uint32_t id;
    completion_t *done;
} playback_t;

struct media_clip_player {
    xiaotai_critical_t lock;
    bool initialized, live, cancelled, mixer;
    uint32_t next_id;
    xiaotai_err_t cancel_result;
    playback_t pending, running;
    completion_t completions[COMPLETIONS_MAX];

    const xiaotai_audio_output_t *output;
    decoder_slot_t decoders[MEDIA_SOUND_DECODERS_MAX];
    unsigned decoder_count;
    int16_t *pcm, *source;
    xiaotai_media_rate_t *converter;
    size_t block, source_capacity;

    media_sound_cursor_t cursor;
    const decoder_slot_t *decoding;
    unsigned clip;
    uint16_t repeats;
    bool clip_open, natural_end, conversion, capture_clock;
    int16_t *decoded;
    size_t source_offset, source_count, silence, flush;
    int64_t deadline;
    uint32_t fractional_us;
};

/* Completion slots stay valid even when host teardown frees the PCM workset.
 * A slot is not recycled until its already-posted owner job has taken it. */
static media_clip_player_t s_player;

static const decoder_slot_t *find_decoder(const media_clip_player_t *p, const media_sound_cursor_t *cursor)
{
    for (unsigned i = 0; i < p->decoder_count; ++i) {
        const decoder_slot_t *slot = &p->decoders[i];
        if (slot->decoder == cursor->decoder && slot->format.encoding == cursor->format.encoding &&
            slot->format.rate_hz == cursor->format.rate_hz && slot->format.channels == cursor->format.channels &&
            slot->format.packet_ms == cursor->format.packet_ms)
            return slot;
    }
    return NULL;
}

/* Whole-container validation precedes walking frame headers. Reserve only the
 * largest packet the prepared assets actually use, not the format maximum. */
static size_t decoded_capacity(const media_sound_cursor_t *cursor)
{
    if (!cursor->decoder) return 0;
    const size_t rate = cursor->format.rate_hz;
    if (cursor->container != XIAOTAI_SOUND_PACKETS) return rate * cursor->format.packet_ms / 1000U;
    size_t largest = 0;
    for (size_t at = 0; at < cursor->size;) {
        const uint8_t *packet = cursor->payload + at;
        const size_t bytes = (size_t)packet[0] | (size_t)packet[1] << 8U;
        const size_t samples = cursor->decoder->packet_samples(packet + 2, bytes, (uint32_t)rate);
        if (!samples || samples > rate * XIAOTAI_MEDIA_PACKET_MS_MAX / 1000U) return 0;
        if (samples > largest) largest = samples;
        at += bytes + 2;
    }
    return largest;
}

static void release_workset(media_clip_player_t *p)
{
    while (p->decoder_count) {
        decoder_slot_t *slot = &p->decoders[--p->decoder_count];
        slot->decoder->close(slot->state);
    }
    xiaotai_mem_free(p->pcm);
    xiaotai_mem_free(p->source);
    xiaotai_mem_free(p->converter);
    p->pcm = p->source = NULL;
    p->converter = NULL;
    p->output = NULL;
}

static xiaotai_err_t prepare_decoder(media_clip_player_t *p, const media_sound_cursor_t *cursor)
{
    if (find_decoder(p, cursor)) return XIAOTAI_OK;
    if (p->decoder_count == MEDIA_SOUND_DECODERS_MAX) return XIAOTAI_ERR_NO_MEM;
    void *state = NULL;
    const xiaotai_err_t err = cursor->decoder->open(&cursor->format, &state);
    if (err == XIAOTAI_OK) p->decoders[p->decoder_count++] = (decoder_slot_t){cursor->decoder, cursor->format, state};
    return err;
}

static xiaotai_err_t prepare_stream_decoder(media_clip_player_t *p, xiaotai_media_format_t format)
{
    if (format.encoding == XIAOTAI_CODEC_PCM16 || format.encoding == XIAOTAI_CODEC_G711A ||
        !xiaotai_media_rate_supported(format.rate_hz, p->output->desc.rate_hz)) return XIAOTAI_OK;
    const media_sound_cursor_t cursor = {.format = format, .decoder = media_select_decoder(&format)};
    if (!cursor.decoder) return XIAOTAI_OK;
    const xiaotai_err_t err = prepare_decoder(p, &cursor);
    return err == XIAOTAI_ERR_NOT_SUPPORTED ? XIAOTAI_OK : err;
}

xiaotai_err_t media_clip_player_create(const xiaotai_audio_output_t *out, media_clip_player_t **player)
{
    if (!player) return XIAOTAI_ERR_INVALID_ARG;
    *player = NULL;
    if (!out || !out->write || !out->desc.channels ||
        out->desc.channels > sizeof(out->desc.roles) / sizeof(out->desc.roles[0]) ||
        out->desc.encoding != XIAOTAI_CODEC_PCM16 ||
        !xiaotai_media_rate_supported(out->desc.rate_hz, out->desc.rate_hz)) return XIAOTAI_ERR_NOT_SUPPORTED;
    media_clip_player_t *p = &s_player;
    if (p->live) return XIAOTAI_ERR_INVALID_STATE;
    if (!p->initialized) {
        xiaotai_critical_init(&p->lock);
        p->initialized = true;
    }
    p->output = out;
    p->mixer = false;
    p->capture_clock = false;
    p->block = out->desc.rate_hz * PLAY_BLOCK_MS / 1000U;
    if (out->info.max_write_frames && out->info.max_write_frames < p->block) p->block = out->info.max_write_frames;
    const bool converter = xiaotai_media_rate_supported(out->desc.rate_hz == 8000U ? 16000U : 8000U,
                                                        out->desc.rate_hz);
    p->source_capacity = converter ? SOURCE_BLOCK : 0;
    xiaotai_err_t err = XIAOTAI_OK;
    const size_t count = xiaotai_sounds_count();
    for (size_t i = 0; i < count; ++i) {
        media_sound_cursor_t cursor;
        if (media_sound_cursor_open(&cursor, xiaotai_sounds_at(i)) != XIAOTAI_OK || !cursor.decoder ||
            !xiaotai_media_rate_supported(cursor.format.rate_hz, out->desc.rate_hz)) continue;
        const size_t samples = decoded_capacity(&cursor);
        if (!samples) continue;
        err = prepare_decoder(p, &cursor);
        if (err == XIAOTAI_ERR_NOT_SUPPORTED) continue;
        if (err != XIAOTAI_OK) goto fail;
        if (samples > p->source_capacity) p->source_capacity = samples;
    }
    xiaotai_media_format_t format;
    for (size_t i = 0; media_required_stream_format_at(i, &format); ++i) {
        err = prepare_stream_decoder(p, format);
        if (err != XIAOTAI_OK) goto fail;
    }
    /* A native-rate stream does not need a corresponding declaration or clip. */
    const xiaotai_audio_decoder_t *registered;
    for (size_t i = 0; (registered = media_registered_decoder_at(i)) != NULL; ++i) {
        err = prepare_stream_decoder(p, media_stream_format(registered->encoding, out->desc.rate_hz));
        if (err != XIAOTAI_OK) goto fail;
    }
    p->pcm = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, p->block * out->desc.channels * sizeof(int16_t));
    if (p->source_capacity) p->source = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, p->source_capacity * sizeof(int16_t));
    if (converter) p->converter = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, sizeof(*p->converter));
    if (!p->pcm || (p->source_capacity && !p->source) || (converter && !p->converter)) {
        err = XIAOTAI_ERR_NO_MEM;
        goto fail;
    }
    p->live = true;
    *player = p;
    return XIAOTAI_OK;

fail:
    release_workset(p);
    return err;
}

uint32_t media_clip_player_decoder_stack_bytes(const media_clip_player_t *p)
{
    uint32_t bytes = 0;
    for (unsigned i = 0; p && i < p->decoder_count; ++i) {
        const uint32_t required = p->decoders[i].decoder->stack_bytes;
        if (required > bytes) bytes = required;
    }
    return bytes;
}

xiaotai_err_t media_clip_request_prepare(const media_clip_player_t *p, const xiaotai_media_play_t *request,
                                         media_clip_request_t *prepared)
{
    if (!request || !prepared || !request->clip_count || request->clip_count > XIAOTAI_MEDIA_PLAY_CLIPS_MAX ||
        (unsigned)request->priority > XIAOTAI_PLAY_URGENT ||
        (request->flags & ~(uint32_t)(XIAOTAI_PLAY_OVER_SESSION | XIAOTAI_PLAY_REFUSE_EQUAL)))
        return XIAOTAI_ERR_INVALID_ARG;
    for (unsigned i = 0; i < request->clip_count; ++i) {
        if (!request->clips[i].sound && (!request->clips[i].name || !request->clips[i].name[0]))
            return XIAOTAI_ERR_INVALID_ARG;
    }
    if (!p) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!p->live) return XIAOTAI_ERR_INVALID_STATE;
    if (!p->mixer && (request->flags & XIAOTAI_PLAY_OVER_SESSION)) return XIAOTAI_ERR_NOT_SUPPORTED;
    *prepared = (media_clip_request_t){.done = request->done, .ctx = request->ctx, .generation = request->generation,
        .flags = request->flags, .repeat = request->repeat, .gap_ms = request->gap_ms,
        .clip_count = request->clip_count, .priority = request->priority};
    for (unsigned i = 0; i < request->clip_count; ++i) {
        const xiaotai_sound_t *sound = request->clips[i].sound;
        if (!sound) sound = xiaotai_sounds_find(request->clips[i].name);
        if (!sound) return XIAOTAI_ERR_NOT_FOUND;
        media_sound_cursor_t *cursor = &prepared->clips[i];
        if (media_sound_cursor_open(cursor, sound) != XIAOTAI_OK ||
            !xiaotai_media_rate_supported(cursor->format.rate_hz, p->output->desc.rate_hz))
            return XIAOTAI_ERR_NOT_SUPPORTED;
        if (cursor->decoder) {
            const size_t samples = decoded_capacity(cursor);
            if (!find_decoder(p, cursor) || !samples || samples > p->source_capacity) return XIAOTAI_ERR_NOT_SUPPORTED;
        }
    }
    return XIAOTAI_OK;
}

xiaotai_err_t media_clip_stream_prepare(const media_clip_player_t *p, const xiaotai_media_stream_t *request,
                                        media_clip_request_t *prepared)
{
    if (!request || !prepared || request->rate_hz < XIAOTAI_MEDIA_RATE_MIN_HZ ||
        request->rate_hz > XIAOTAI_MEDIA_RATE_MAX_HZ || (unsigned)request->priority > XIAOTAI_PLAY_URGENT ||
        (request->flags & ~(uint32_t)(XIAOTAI_PLAY_OVER_SESSION | XIAOTAI_PLAY_REFUSE_EQUAL)))
        return XIAOTAI_ERR_INVALID_ARG;
    if (!p) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!p->live) return XIAOTAI_ERR_INVALID_STATE;
    if ((!p->mixer && (request->flags & XIAOTAI_PLAY_OVER_SESSION)) ||
        !xiaotai_media_rate_supported(request->rate_hz, p->output->desc.rate_hz) ||
        !XIAOTAI_CODEC_IS_AUDIO(request->encoding)) return XIAOTAI_ERR_NOT_SUPPORTED;

    const xiaotai_media_format_t format = media_stream_format(request->encoding, request->rate_hz);
    const xiaotai_audio_decoder_t *decoder = NULL;
    if (format.encoding != XIAOTAI_CODEC_PCM16 && format.encoding != XIAOTAI_CODEC_G711A) {
        decoder = media_select_decoder(&format);
        const media_sound_cursor_t cursor = {.format = format, .decoder = decoder};
        if (!decoder || !find_decoder(p, &cursor)) return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    *prepared = (media_clip_request_t){.done = request->done, .ctx = request->ctx,
        .generation = request->generation, .flags = request->flags, .priority = request->priority};
    return media_stream_buffer_create(&format, decoder, &prepared->stream);
}

void media_clip_request_discard(media_clip_request_t *request)
{
    if (request) {
        media_stream_buffer_release(request->stream);
        request->stream = NULL;
    }
}

static playback_t *admitted(media_clip_player_t *p)
{
    if (p->pending.id) return &p->pending;
    if (p->running.id && !p->cancelled) return &p->running;
    return NULL;
}

static void complete(playback_t *play, xiaotai_err_t result)
{
    media_stream_buffer_revoke(play->request.stream);
    if (play->done) {
        play->done->result = result;
        play->done->state = COMPLETION_READY;
    }
    play->id = 0;
    play->done = NULL;
    play->request.stream = NULL;
}

xiaotai_err_t media_clip_player_submit(media_clip_player_t *p, const media_clip_request_t *request, uint32_t *id)
{
    if (id) *id = 0;
    if (!p || !request || (!request->clip_count && !request->stream)) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_critical_enter(&p->lock);
    const playback_t *current = admitted(p);
    if (!p->live || (current && (request->priority == XIAOTAI_PLAY_CUE ||
         request->priority < current->request.priority ||
         (request->priority == current->request.priority && (request->flags & XIAOTAI_PLAY_REFUSE_EQUAL))))) {
        const xiaotai_err_t err = p->live ? XIAOTAI_ERR_BUSY : XIAOTAI_ERR_INVALID_STATE;
        xiaotai_critical_exit(&p->lock);
        return err;
    }
    completion_t *completion = NULL;
    if (request->done || request->stream) {
        for (unsigned i = 0; i < COMPLETIONS_MAX; ++i) {
            if (p->completions[i].state == COMPLETION_FREE) { completion = &p->completions[i]; break; }
        }
        if (!completion) {
            xiaotai_critical_exit(&p->lock);
            return XIAOTAI_ERR_BUSY;
        }
    }
    if (p->pending.id) complete(&p->pending, XIAOTAI_ERR_INVALID_STATE);
    if (p->running.id && !p->cancelled) {
        p->cancelled = true;
        p->cancel_result = XIAOTAI_ERR_INVALID_STATE;
        media_stream_buffer_revoke(p->running.request.stream);
    }
    if (++p->next_id == 0) ++p->next_id;
    if (completion) *completion = (completion_t){.state = COMPLETION_RESERVED, .fn = request->done,
        .ctx = request->ctx, .stream = request->stream, .id = p->next_id};
    p->pending = (playback_t){.request = *request, .id = p->next_id, .done = completion};
    if (id) *id = p->next_id;
    xiaotai_critical_exit(&p->lock);
    return XIAOTAI_OK;
}

media_stream_buffer_t *media_clip_player_find_stream(media_clip_player_t *p, uint32_t id)
{
    if (!p || !id) return NULL;
    xiaotai_critical_enter(&p->lock);
    const playback_t *play = admitted(p);
    media_stream_buffer_t *stream = play && play->id == id ? play->request.stream : NULL;
    media_stream_buffer_retain(stream);
    xiaotai_critical_exit(&p->lock);
    return stream;
}

uint32_t media_clip_player_id(media_clip_player_t *p)
{
    if (!p) return 0;
    xiaotai_critical_enter(&p->lock);
    const playback_t *play = admitted(p);
    const uint32_t id = play ? play->id : 0;
    xiaotai_critical_exit(&p->lock);
    return id;
}

bool media_clip_player_needs_work(media_clip_player_t *p)
{
    if (!p) return false;
    xiaotai_critical_enter(&p->lock);
    bool work = p->pending.id || p->running.id;
    for (unsigned i = 0; !work && i < COMPLETIONS_MAX; ++i) work = p->completions[i].state == COMPLETION_READY;
    xiaotai_critical_exit(&p->lock);
    return work;
}

static void cancel(media_clip_player_t *p, uint32_t id, uint32_t generation, bool only_plain, xiaotai_err_t why)
{
    if (!p) return;
    xiaotai_critical_enter(&p->lock);
    if (p->pending.id && (!id || p->pending.id == id) &&
        (!only_plain || !(p->pending.request.flags & XIAOTAI_PLAY_OVER_SESSION)) &&
        (!generation || p->pending.request.generation == generation)) complete(&p->pending, why);
    if (p->running.id && !p->cancelled && (!id || p->running.id == id) &&
        (!only_plain || !(p->running.request.flags & XIAOTAI_PLAY_OVER_SESSION)) &&
        (!generation || p->running.request.generation == generation)) {
        p->cancelled = true;
        p->cancel_result = why;
        media_stream_buffer_revoke(p->running.request.stream);
    }
    xiaotai_critical_exit(&p->lock);
}

void media_clip_player_cancel(media_clip_player_t *p, uint32_t id, xiaotai_err_t why) { cancel(p, id, 0, false, why); }
void media_clip_player_cancel_generation(media_clip_player_t *p, uint32_t generation)
{
    if (generation) cancel(p, 0, generation, false, XIAOTAI_ERR_INVALID_STATE);
}

void media_clip_player_enable_mixer(media_clip_player_t *p, bool enabled) { if (p) p->mixer = enabled; }
void media_clip_player_cancel_plain(media_clip_player_t *p) { cancel(p, 0, 0, true, XIAOTAI_ERR_INVALID_STATE); }

bool media_clip_player_over_session(media_clip_player_t *p, uint32_t id)
{
    if (!p || !id || !p->mixer) return false;
    xiaotai_critical_enter(&p->lock);
    const playback_t *play = admitted(p);
    const bool over = play && play->id == id && (play->request.flags & XIAOTAI_PLAY_OVER_SESSION);
    xiaotai_critical_exit(&p->lock);
    return over;
}

static void dispatch(void *ctx)
{
    completion_t *slot = ctx;
    xiaotai_critical_enter(&s_player.lock);
    const completion_t completed = *slot;
    *slot = (completion_t){0};
    xiaotai_critical_exit(&s_player.lock);
    media_stream_buffer_release(completed.stream);
    if (completed.fn) completed.fn(completed.id, completed.result, completed.ctx);
}

static bool post_done(media_clip_player_t *p)
{
    for (unsigned i = 0; i < COMPLETIONS_MAX; ++i) {
        completion_t *slot = &p->completions[i];
        xiaotai_critical_enter(&p->lock);
        const bool ready = slot->state == COMPLETION_READY;
        media_stream_buffer_t *stream = ready ? slot->stream : NULL;
        if (ready) slot->state = COMPLETION_POSTED;
        xiaotai_critical_exit(&p->lock);
        media_stream_buffer_notify(stream);
        if (ready && xiaotai_run(dispatch, slot) != XIAOTAI_OK) {
            xiaotai_critical_enter(&p->lock);
            slot->state = COMPLETION_READY;
            xiaotai_critical_exit(&p->lock);
            return false;
        }
    }
    return true;
}

void media_clip_player_destroy(media_clip_player_t *p)
{
    if (!p) return;
    media_stream_buffer_t *retire[COMPLETIONS_MAX] = {0};
    xiaotai_critical_enter(&p->lock);
    p->live = false;
    if (p->pending.id) complete(&p->pending, XIAOTAI_ERR_INVALID_STATE);
    if (p->running.id) complete(&p->running, XIAOTAI_ERR_INVALID_STATE);
    for (unsigned i = 0; i < COMPLETIONS_MAX; ++i) {
        completion_t *done = &p->completions[i];
        if (done->state == COMPLETION_READY) {
            retire[i] = done->stream;
            done->stream = NULL;
        }
    }
    p->cancelled = false;
    xiaotai_critical_exit(&p->lock);
    for (unsigned i = 0; i < COMPLETIONS_MAX; ++i) {
        media_stream_buffer_notify(retire[i]);
        media_stream_buffer_release(retire[i]);
    }
    (void)post_done(p);
    release_workset(p);
}

static void begin(media_clip_player_t *p)
{
    p->clip = 0;
    p->repeats = p->running.request.repeat;
    p->clip_open = p->natural_end = false;
    p->silence = p->flush = 0;
    p->deadline = 0;
    p->fractional_us = 0;
}

static xiaotai_err_t open_clip(media_clip_player_t *p)
{
    media_stream_buffer_t *stream = p->running.request.stream;
    if (stream) {
        p->cursor = (media_sound_cursor_t){.format = *media_stream_buffer_format(stream),
                                          .decoder = media_stream_buffer_decoder(stream)};
        size_t capacity;
        p->decoded = media_stream_buffer_pcm(stream, &capacity);
    } else {
        p->cursor = p->running.request.clips[p->clip];
        p->decoded = p->source;
    }
    p->decoding = p->cursor.decoder ? find_decoder(p, &p->cursor) : NULL;
    if (p->cursor.decoder && !p->decoding) return XIAOTAI_FAIL;
    if (p->decoding && p->decoding->decoder->reset && p->decoding->decoder->reset(p->decoding->state) != XIAOTAI_OK)
        return XIAOTAI_FAIL;
    p->conversion = p->cursor.format.rate_hz != p->output->desc.rate_hz;
    if (p->conversion && xiaotai_media_rate_open(p->converter, p->cursor.format.rate_hz, 1,
                                                p->output->desc.rate_hz) != XIAOTAI_OK) return XIAOTAI_FAIL;
    p->source_offset = p->source_count = 0;
    p->clip_open = true;
    return XIAOTAI_OK;
}

static void set_silence(int16_t *pcm, size_t frames) { memset(pcm, 0, frames * sizeof(*pcm)); }

static xiaotai_err_t stream_samples(media_clip_player_t *p, media_stream_buffer_t *stream, bool *starved)
{
    const uint8_t *payload;
    size_t bytes, samples, capacity;
    int16_t *pcm = media_stream_buffer_pcm(stream, &capacity);
    p->source_count = 0;
    switch (media_stream_buffer_read(stream, capacity, &payload, &bytes, &samples)) {
    case MEDIA_STREAM_STARVED:
        *starved = true;
        return XIAOTAI_OK;
    case MEDIA_STREAM_END:
        return XIAOTAI_OK;
    case MEDIA_STREAM_TIMEOUT:
        return XIAOTAI_ERR_TIMEOUT;
    case MEDIA_STREAM_REVOKED:
        return XIAOTAI_ERR_INVALID_STATE;
    case MEDIA_STREAM_DATA:
        break;
    }
    if (p->decoding) {
        size_t written = 0;
        if (p->decoding->decoder->decode(p->decoding->state, payload, bytes, pcm, capacity, &written) != XIAOTAI_OK ||
            written != samples) return XIAOTAI_FAIL;
    } else if (p->cursor.format.encoding == XIAOTAI_CODEC_G711A) {
        xiaotai_media_alaw_decode(payload, samples, pcm);
    } else {
        /* The stream PCM format is native PCM16, unlike file byte order. */
        memcpy(pcm, payload, bytes);
    }
    p->source_count = samples;
    return XIAOTAI_OK;
}

static xiaotai_err_t clip_samples(media_clip_player_t *p, int16_t *pcm, size_t capacity, size_t *written)
{
    *written = 0;
    media_stream_buffer_t *stream = p->running.request.stream;
    if (!stream && !p->conversion && !p->decoding) return media_sound_cursor_read(&p->cursor, NULL, pcm, capacity, written);
    while (*written < capacity) {
        if (p->source_offset == p->source_count) {
            const size_t limit = p->decoding ? p->source_capacity : SOURCE_BLOCK;
            bool starved = false;
            const xiaotai_err_t err = stream ? stream_samples(p, stream, &starved) :
                media_sound_cursor_read(&p->cursor, p->decoding ? p->decoding->state : NULL,
                                        p->decoded, limit, &p->source_count);
            if (err != XIAOTAI_OK) return err;
            p->source_offset = 0;
            if (starved) {
                set_silence(pcm + *written, capacity - *written);
                *written = capacity;
                return XIAOTAI_OK;
            }
            if (!p->source_count) {
                if (p->conversion) *written += xiaotai_media_rate_finish(p->converter, pcm + *written,
                                                                          capacity - *written);
                return XIAOTAI_OK;
            }
            if (p->conversion) {
                uint8_t *bytes = (uint8_t *)p->decoded;
                for (size_t i = 0; i < p->source_count; ++i) {
                    const uint16_t sample = (uint16_t)p->decoded[i];
                    bytes[2 * i] = (uint8_t)sample;
                    bytes[2 * i + 1] = (uint8_t)(sample >> 8U);
                }
            }
        }

        const size_t remaining = p->source_count - p->source_offset;
        const size_t room = capacity - *written;
        size_t taken;
        if (p->conversion) {
            const size_t made = xiaotai_media_rate_process(p->converter,
                (const uint8_t *)(p->decoded + p->source_offset), remaining, &taken, pcm + *written, room);
            if (!made && !taken) return XIAOTAI_FAIL;
            *written += made;
        } else {
            taken = remaining < room ? remaining : room;
            memcpy(pcm + *written, p->decoded + p->source_offset, taken * sizeof(*pcm));
            *written += taken;
        }
        p->source_offset += taken;
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t fill(media_clip_player_t *p, size_t limit, size_t *frames)
{
    *frames = 0;
    while (*frames < limit && !p->natural_end) {
        const size_t room = limit - *frames;
        if (p->silence) {
            const size_t count = p->silence < room ? p->silence : room;
            set_silence(p->pcm + *frames, count);
            *frames += count;
            p->silence -= count;
            continue;
        }
        if (!p->clip_open && open_clip(p) != XIAOTAI_OK) return XIAOTAI_FAIL;
        size_t read;
        const xiaotai_err_t err = clip_samples(p, p->pcm + *frames, room, &read);
        if (err != XIAOTAI_OK) return err == XIAOTAI_ERR_TIMEOUT || err == XIAOTAI_ERR_INVALID_STATE ? err : XIAOTAI_FAIL;
        if (read) { *frames += read; continue; }
        p->clip_open = false;
        if (++p->clip < p->running.request.clip_count) continue;
        if (p->repeats) {
            if (p->repeats != XIAOTAI_PLAY_FOREVER) --p->repeats;
            p->clip = 0;
            p->silence = (size_t)(((uint64_t)p->running.request.gap_ms * p->output->desc.rate_hz) / 1000U);
        } else {
            p->natural_end = true;
            p->flush = p->output->flush_frames ? p->output->flush_frames(p->output->self) : 0;
        }
    }
    if (p->natural_end && *frames < limit && p->flush) {
        const size_t room = limit - *frames;
        const size_t count = p->flush < room ? p->flush : room;
        set_silence(p->pcm + *frames, count);
        *frames += count;
        p->flush -= count;
    }
    return XIAOTAI_OK;
}

static uint32_t wait_until(int64_t deadline, int64_t now)
{
    if (deadline <= now) return 0;
    return (uint32_t)((deadline - now + 999) / 1000);
}

static uint32_t retry_if_needed(media_clip_player_t *p, uint32_t wait)
{
    if (!post_done(p) && wait > RETRY_MS) return RETRY_MS;
    return wait;
}

static void finish(media_clip_player_t *p, uint32_t id, xiaotai_err_t result)
{
    xiaotai_critical_enter(&p->lock);
    if (p->running.id == id) {
        complete(&p->running, p->cancelled ? p->cancel_result : result);
        p->cancelled = false;
    }
    xiaotai_critical_exit(&p->lock);
}

static bool select_running(media_clip_player_t *p, uint32_t id, bool overlay, bool *start)
{
    xiaotai_critical_enter(&p->lock);
    if (p->running.id && p->cancelled) {
        complete(&p->running, p->cancel_result);
        p->cancelled = false;
    }
    const playback_t *play = admitted(p);
    const bool matches = id && play && play->id == id &&
                         (!overlay || (play->request.flags & XIAOTAI_PLAY_OVER_SESSION));
    *start = matches && !p->running.id;
    if (*start) {
        p->running = p->pending;
        p->pending.id = 0;
        p->pending.done = NULL;
        p->pending.request.stream = NULL;
    }
    const bool powered = matches && p->running.id == id;
    xiaotai_critical_exit(&p->lock);
    return powered;
}

uint32_t media_clip_player_step(media_clip_player_t *p, uint32_t powered_id, xiaotai_err_t power_result)
{
    if (!p) return XIAOTAI_WAIT_FOREVER;
    bool start;
    const bool powered = select_running(p, powered_id, false, &start);
    if (!powered) {
        const bool done = post_done(p);
        return !done || media_clip_player_needs_work(p) ? RETRY_MS : XIAOTAI_WAIT_FOREVER;
    }
    if (power_result != XIAOTAI_OK) {
        finish(p, powered_id, XIAOTAI_FAIL);
        return retry_if_needed(p, 0);
    }
    if (start) begin(p);
    const int64_t now = xiaotai_time_us();
    const uint32_t paced_wait = wait_until(p->deadline, now);
    if (paced_wait) return retry_if_needed(p, paced_wait);

    size_t frames;
    const xiaotai_err_t err = fill(p, p->block, &frames);
    if (err != XIAOTAI_OK || !frames) {
        finish(p, powered_id, err);
        return retry_if_needed(p, 0);
    }
    const size_t channels = p->output->desc.channels;
    if (channels > 1) {
        for (size_t frame = frames; frame--;) {
            const int16_t sample = p->pcm[frame];
            for (size_t channel = 0; channel < channels; ++channel) p->pcm[frame * channels + channel] = sample;
        }
    }
    if (media_clip_player_id(p) != powered_id) return retry_if_needed(p, 0);
    if (p->output->write(p->output->self, p->pcm, frames * channels * sizeof(*p->pcm)) != XIAOTAI_OK) {
        finish(p, powered_id, XIAOTAI_FAIL);
        return retry_if_needed(p, 0);
    }
    if (!p->capture_clock && !(p->output->info.flags & XIAOTAI_AUDIO_OUTPUT_PACED)) {
        const uint32_t rate = p->output->desc.rate_hz;
        const uint32_t numerator = (uint32_t)frames * 1000000U + p->fractional_us;
        p->fractional_us = numerator % rate;
        p->deadline = now + numerator / rate;
    }
    return retry_if_needed(p, wait_until(p->deadline, xiaotai_time_us()));
}

void media_clip_player_capture_clock(media_clip_player_t *p, bool enabled)
{
    if (p) p->capture_clock = enabled;
}

uint32_t media_clip_player_mix(media_clip_player_t *p, uint32_t id, int16_t *mono, size_t frames)
{
    if (!p || !p->mixer || !mono || !frames || frames > p->block) return 0;
    bool start;
    if (!select_running(p, id, true, &start)) {
        (void)post_done(p);
        return 0;
    }
    if (start) begin(p);
    size_t got = 0;
    const xiaotai_err_t err = fill(p, frames, &got);
    if (err != XIAOTAI_OK || !got) {
        finish(p, id, err);
        (void)post_done(p);
        return 0;
    }
    if (media_clip_player_id(p) != id) return 0;
    media_mix_pcm16(mono, p->pcm, got);
    return id;
}

void media_clip_player_mix_written(media_clip_player_t *p, uint32_t token, xiaotai_err_t result)
{
    if (!p || !token) return;
    if (result != XIAOTAI_OK) finish(p, token, XIAOTAI_FAIL);
    else if (p->natural_end && !p->flush) finish(p, token, XIAOTAI_OK);
    (void)post_done(p);
}
